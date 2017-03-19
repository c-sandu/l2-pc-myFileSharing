#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <dirent.h>
#include <map>
#include <string>
#include "utils.h"
#include "commands.h"

std::map<std::string, int> expected_argc;
// ^ pentru a verifica corectitudinea comenzilor de la stdin

std::map<std::string, cl_filestat> localfiles; // fisierele locale
std::map<int, int> sv_to_cl; // maparea fd_server -> fd_client
std::map<int, std::string> openfiles; // fisierele in curs de transfer

char current_user[MAX_STR_LENGTH] = "none"; // utilizatorul curent
char prompt[MAX_STR_LENGTH] = "$ "; // prompt-ul curent
char path[4 * MAX_STR_LENGTH]; // cwd

int pid; // id-ul procesului client

bool safe_to_quit = false;

FILE *log_file;


/**
 * Actualizeaza prompt-ul.
 */
void update_prompt(char *user) {
    if (!strcmp(user, "none"))
        strcpy(prompt, "$ ");
    else {
        strcpy(prompt, user);
        strcat(prompt, "> ");
    }
}

/**
 * Initializari care includ fisierele locale, pid, path
 */
void cl_init() {
    expected_argc[std::string("login")] = 2;
    expected_argc[std::string("logout")] = 0;
    expected_argc[std::string("getuserlist")] = 0;
    expected_argc[std::string("getfilelist")] = 1;
    expected_argc[std::string("upload")] = 1;
    expected_argc[std::string("download")] = 2;
    expected_argc[std::string("share")] = 1;
    expected_argc[std::string("unshare")] = 1;
    expected_argc[std::string("delete")] = 1;
    expected_argc[std::string("quit")] = 0;

    DIR *dir;
    dirent *ent;
    char path_buffer[BUFLEN];

    getcwd(path, sizeof(path));

    dir = opendir(path);
    while ((ent = readdir(dir)) != NULL) {
        strcpy(path_buffer, path);
        strcat(path_buffer, "/");
        strcat(path_buffer, ent->d_name);

        cl_filestat newfile;

        newfile.fd = -1;
        newfile.is_open = false;
        strcpy(newfile.full_path, path_buffer);

        localfiles[std::string(ent->d_name)] = newfile;

    }

    pid = getpid();

    char log_buf[BUFLEN];
    sprintf(log_buf, "client-%d.log", pid);

    log_file = fopen(log_buf, "w");

    printf("%s", prompt);
    fflush(stdout);
    fprintf(log_file, "%s", prompt);
    fflush(log_file);
}


/**
 * Numarul de fisiere aflate in transfer(pentru quit).
 */
int files_in_transfer() {
    int ret = 0;
    std::map<std::string, cl_filestat>::iterator it;
    for (it = localfiles.begin(); it != localfiles.end(); it++)
        if (it->second.is_open)
            ret++;
    return ret;
}


int main(int argc, char *argv[]) {

    if (argc < 3) {
        error("Usage: ./client <IP_server> <port_server>\n");
    }

    cl_init();

    int sockfd; // socket-ul pe care comunic cu server-ul
    uint16_t fdmax, i;
    int byte_count;

    sockaddr_in serv_addr;

    fd_set read_fds;
    fd_set temp_fds;

    FD_ZERO(&read_fds);
    FD_ZERO(&temp_fds);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(atoi(argv[2]));
    inet_aton(argv[1], &serv_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR connecting");

    FD_SET(sockfd, &read_fds);
    FD_SET(0, &read_fds);
    fdmax = sockfd;

    unsigned char buffer[BUFLEN];
    command cmd_from_input;
    while (1) {
        temp_fds = read_fds;
        if (select(fdmax + 1, &temp_fds, NULL, NULL, NULL) == -1)
            error("ERROR in select");

        if (safe_to_quit && files_in_transfer() == 0) {
            close(sockfd);
            break;
        }

        // looking for data to read
        for (i = 0; i <= fdmax; ++i) {
            if (FD_ISSET(i, &temp_fds)) {
                if (i == 0) { // data from stdin
                    // should be a command
                    memset(buffer, 0, BUFLEN);
                    fgets((char*)buffer, BUFLEN - 1, stdin);

                    buffer[strlen((char*)buffer) - 1] = '\0';

                    // printf("am citit: `%s`\n", buffer);

                    fprintf(log_file, "%s\n%s", buffer, prompt);

                    cmd_from_input = create_cmd(buffer, current_user);
                    // ^ pentru a testa corectitudinea
                    // printf("buffer length: %ld\n", strlen((char*)buffer));

                    if (!strcmp(cmd_from_input.name, "WHITESPACE")) {
                        printf("%s", prompt);
                        fflush(stdout);
                        fprintf(log_file, "%s", prompt);
                        fflush(log_file);
                        continue;
                    } else if (!strcmp(cmd_from_input.name, "ERROR")) {
                        printf("Comanda inexistenta\n%s", prompt);
                        fflush(stdout);
                        fprintf(log_file, "Comanda inexistenta\n%s", prompt);
                        fflush(log_file);
                        continue;
                    } else if (strcmp(cmd_from_input.name, "login") && !strcmp(current_user, "none")) {
                        printf("-1 Clientul nu e autentificat\n%s", prompt);
                        fflush(stdout);
                        fprintf(log_file, "-1 Clientul nu e autentificat\n%s", prompt);
                        fflush(log_file);
                        continue;
                    } else if (cmd_from_input.argc < expected_argc[std::string(cmd_from_input.name)]) {
                        printf("Prea putine argumente\n%s", prompt);
                        fflush(stdout);
                        fprintf(log_file, "Prea putine argumente\n%s", prompt);
                        fflush(log_file);
                        continue;
                    } else if (cmd_from_input.argc > expected_argc[std::string(cmd_from_input.name)]) {
                        printf("Prea multe argumente\n%s", prompt);
                        fflush(stdout);
                        fprintf(log_file, "Prea multe argumente\n%s", prompt);
                        fflush(log_file);
                        continue;
                    } else if (!strcmp(cmd_from_input.name, "login") && strcmp(current_user, "none")) {
                        printf("-2 Sesiune deja deschisa\n%s", prompt);
                        fflush(stdout);
                        fprintf(log_file, "-2 Sesiune deja deschisa\n%s", prompt);
                        fflush(log_file);
                        continue;
                    } else if (!strcmp(cmd_from_input.name, "logout") && !strcmp(current_user, "none")) {
                        printf("-1 Clientul nu e autentificat\n%s", prompt);
                        fflush(stdout);
                        fprintf(log_file, "-1 Clientul nu e autentificat\n%s", prompt);
                        fflush(log_file);
                        continue;
                    } else if (!strcmp(cmd_from_input.name, "upload") && localfiles.find(std::string(cmd_from_input.args[0])) == localfiles.end()) {
                        printf("-4 Fisier inexistent\n%s", prompt);
                        fflush(stdout);
                        fprintf(log_file, "-4 Fisier inexistent\n%s", prompt);
                        fflush(log_file);
                        continue;
                    }// TODO: logging and pretty prompt

                    // creez mesajul
                    message msg_to_send;
                    msg_to_send.type = CMD_CLIENT;
                    msg_to_send.length = strlen((char*)buffer) + 1;
                    // printf("msg_to_send.length = %d\n", msg_to_send.length);
                    memcpy(msg_to_send.payload, buffer, msg_to_send.length);
                    // printmsg(msg_to_send);
                    // trimit comanda serverului
                    byte_count = mySend(sockfd, msg_to_send);
                    if (byte_count < 0)
                        error("ERROR writing to socket");

                } else if (i == sockfd) { // something from the server
                    message msg_to_receive;
                    if ((byte_count = myRecv(sockfd, &msg_to_receive)) <= 0) {
                        if (byte_count == 0) { // server hang up
                            printf("client: server(%d) hang up", i);
                        } else
                            error("ERROR in recv");
                        close(i);
                        FD_CLR(i, &read_fds);
                    } else {
                        if (msg_to_receive.type == CMD_ERROR) { // error
                            char errno[MAX_STR_LENGTH];
                            sscanf((char*)msg_to_receive.payload, "%s", errno);

                            printf("%s\n%s", msg_to_receive.payload, prompt);
                            fflush(stdout);
                            fprintf(log_file, "%s\n%s", msg_to_receive.payload, prompt);
                            fflush(log_file);

                            if (!strcmp(errno, "-8")) // BF
                                exit(-8);
                        } else if (msg_to_receive.type == CMD_SUCCESS) {
                            for (int j = 0; j < msg_to_receive.length; ++j)
                                buffer[j] = msg_to_receive.payload[j];

                            command cmd = create_cmd(buffer, current_user);

                            if (!strcmp(cmd.name, "login")) {
                                strcpy(current_user, cmd.args[0]);
                                update_prompt(current_user);
                                printf("%s", prompt);
                                fflush(stdout);
                                fprintf(log_file, "%s", prompt);
                                fflush(log_file);

                            } else if (!strcmp(cmd.name, "logout")) {
                                strcpy(current_user, "none");
                                update_prompt(current_user);
                                printf("%s", prompt);
                                fflush(stdout);
                                fprintf(log_file, "%s", prompt);
                                fflush(log_file);

                            } else if (!strcmp(cmd.name, "quit")) {
                                printf("Quitting...\n%s", prompt);
                                fprintf(log_file, "Quitting...\n%s", prompt);
                                fflush(log_file);
                                fflush(stdout);
                                safe_to_quit = true;

                            } else {
                                printf("%s\n%s", buffer, prompt);
                                fflush(stdout);
                                fprintf(log_file, "%s\n%s", buffer, prompt);
                                fflush(log_file);
                            }



                        } else if (msg_to_receive.type == INIT_UPLOAD) {
                            int sv_fd;
                            char filename[MAX_STR_LENGTH];
                            sscanf((char*)msg_to_receive.payload, "%d %s", &sv_fd, filename);

                            int cl_fd = open(localfiles[std::string(filename)].full_path, O_RDONLY);
                            if (cl_fd < 0)
                                error("ERROR on open");
                            sv_to_cl[sv_fd] = cl_fd;
                            openfiles[cl_fd] = std::string(filename);
                            localfiles[std::string(filename)].is_open = true;
                            localfiles[std::string(filename)].fd = cl_fd;

                            FD_SET(cl_fd, &read_fds);
                            if (cl_fd > fdmax)
                                fdmax = cl_fd;

                            message msg_to_send;
                            msg_to_send.type = INIT_UPLOAD2;
                            sprintf((char*)msg_to_send.payload, "%d %d", cl_fd, sv_fd);
                            msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

                            int byte_count = mySend(sockfd, msg_to_send);
                            if (byte_count < 0)
                                error("init_upload2: ERROR on send");


                        } else if (msg_to_receive.type == INIT_DOWNLOAD) {
                            int sv_fd;
                            char filename[MAX_STR_LENGTH];
                            sscanf((char*)msg_to_receive.payload, "%d %s", &sv_fd, filename);

                            char path_buffer[BUFLEN], pid_buf[MAX_STR_LENGTH];
                            sprintf(pid_buf, "%d_%s", pid, filename);
                            strcpy(path_buffer, path);
                            strcat(path_buffer, "/");
                            strcat(path_buffer, pid_buf);

                            int cl_fd = open(path_buffer, O_WRONLY | O_CREAT);
                            if (cl_fd < 0)
                                error("init_download: ERROR in open");

                            sv_to_cl[sv_fd] = cl_fd;
                            openfiles[cl_fd] = std::string(pid_buf);
                            localfiles[std::string(pid_buf)].is_open = true;
                            localfiles[std::string(pid_buf)].fd = cl_fd;
                            strcpy(localfiles[std::string(pid_buf)].full_path, path_buffer);

                            message msg_to_send;
                            msg_to_send.type = INIT_DOWNLOAD2;
                            sprintf((char*)msg_to_send.payload, "%d %d", cl_fd, sv_fd);
                            msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

                            if (mySend(sockfd, msg_to_send) < 0)
                                error("init_download2: ERROR on send");


                        } else if (msg_to_receive.type == FILE_TRANSFER) {
                            uint16_t cl_fd, sv_fd;
                            memcpy(&sv_fd, msg_to_receive.payload, 2);
                            cl_fd = sv_to_cl[sv_fd];

                            write(cl_fd, msg_to_receive.payload + 2, msg_to_receive.length - 2);


                        } else if (msg_to_receive.type == CLOSE_TRANSFER) {
                            uint16_t cl_fd, sv_fd;
                            memcpy(&sv_fd, msg_to_receive.payload, 2);
                            cl_fd = sv_to_cl[sv_fd];

                            close(cl_fd);
                            localfiles[openfiles[cl_fd]].is_open = false;
                            localfiles[openfiles[cl_fd]].fd = -1;

                            struct stat st = {0};
                            stat(localfiles[openfiles[cl_fd]].full_path, &st);

                            printf("Download finished: %s -- %ld bytes\n%s", openfiles[cl_fd].c_str(), st.st_size, prompt);
                            fflush(stdout);
                            fprintf(log_file, "Download finished: %s -- %ld bytes\n%s", openfiles[cl_fd].c_str(), st.st_size, prompt);
                            fflush(log_file);
                        }

                    }

                } else { // de citit din fisier
                    int byte_count = read(i, buffer, 4090);

                    if (byte_count < 0)
                        error("citire din fisier: ERROR on read");

                    if (byte_count > 0) { // nu am ajuns la sfarsit
                        message msg_to_send;
                        msg_to_send.type = FILE_TRANSFER;
                        memcpy(msg_to_send.payload, &i, 2);
                        memcpy(msg_to_send.payload + 2, buffer, byte_count);
                        msg_to_send.length = byte_count + 2;

                        if (mySend(sockfd, msg_to_send) < 0)
                            error("file_transfer: ERROR on send");
                    } else { // am ajuns la EOF
                        message msg_to_send;
                        msg_to_send.type = CLOSE_TRANSFER;
                        memcpy(msg_to_send.payload, &i, 2);
                        msg_to_send.length = 2;

                        FD_CLR(i, &read_fds);
                        close(i);
                        localfiles[openfiles[i]].is_open = false;
                        localfiles[openfiles[i]].fd = -1;

                        if (mySend(sockfd, msg_to_send) < 0)
                            error("close_transfer: ERROR on send");
                    }
                }
            }
        }
    }
    fflush(stdout);
    fprintf(log_file, "Clientul s-a deconectat");
    fflush(log_file);
    fclose(log_file);
    return 0;
}