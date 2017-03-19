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



int listen_port; // portul pe care se asculta conexiunile

std::map<std::string, user> users; // utilizatorii din sistem
std::map<int, client> clients;

char path[4 * MAX_STR_LENGTH]; // path-ul complet al cwd

fd_set read_fds;
fd_set temp_fds;


std::map<int, sv_filestat> files_to_transfer; // fisierele care sunt sau au fost
// transferate.
std::map<int, std::map<int, int> > cl_to_sv;
// ^ mapare intre file-descriptorii pt fiecare client si cei de pe server
// fiecare fisier deschis spre transfer in orice client va avea file
// descriptor-ul fisierului deschis aici, pe server

/**
 * Verifica daca un fisier este in curs de upload(pentru a evita download-ul sau
 * stergerea lui)
 */
bool is_uploading(char *full_path) {
    std::map<int, sv_filestat>::iterator it;

    for (it = files_to_transfer.begin(); it != files_to_transfer.end(); it++) {
        if (!strcmp(it->second.full_path, full_path) && it->second.is_in_upload)
            return true;
    }
    return false;
}

/**
 * Verifica daca un fisier este in curs de download(pentru a evita stergerea
 * lui)
 */
bool is_downloading(char *full_path) {
    std::map<int, sv_filestat>::iterator it;

    for (it = files_to_transfer.begin(); it != files_to_transfer.end(); it++) {
        if (!strcmp(it->second.full_path, full_path) && it->second.is_in_download)
            return true;
    }
    return false;
}

/**
 * Functie apelata in sv_init(), parseaza fisierul cu userii si creeaza
 * intrarile corespunzatoare in users.
 * Creeaza directorul user-ului, daca acesta nu exista.
 * Parcurge fisierele din directorul user-ului si le baga in lista de fisiere.
 */
void parse_ucf(char *filename) {
    FILE *ucf = fopen(filename, "r");
    struct stat st = {0};
    char buf1[MAX_STR_LENGTH], buf2[MAX_STR_LENGTH];
    char path_buffer[BUFLEN], path_buffer2[BUFLEN];
    int n;

    DIR *dir;
    dirent *ent;

    fscanf(ucf, "%d", &n);
    for (int i = 0; i < n; ++i) {
        fscanf(ucf, "%s %s", buf1, buf2);
        user newUser;
        newUser.name = std::string(buf1);
        newUser.pass = std::string(buf2);
        newUser.connected = false;
        newUser.socket = -1;
        users[std::string(buf1)] = newUser;

        strcpy(path_buffer, path);
        strcat(path_buffer, "/");
        strcat(path_buffer, buf1);

        if (stat(path_buffer, &st) == -1) // directorul userului nu exista
            mkdir(path_buffer, 0700);

        dir = opendir(path_buffer);
        ent = readdir(dir);
        ent = readdir(dir);
        while ((ent = readdir(dir)) != NULL) { // mai am fisiere
            disk_file newfile;

            strcpy(path_buffer2, path_buffer);
            strcat(path_buffer2, "/");
            strcat(path_buffer2, ent->d_name);

            newfile.filename = ent->d_name;
            strcpy(newfile.full_path, path_buffer2);
            strcpy(newfile.share_status, "PRIVATE");

            users[std::string(buf1)].files[std::string(newfile.filename)] = newfile;
        }

    }
    fclose(ucf);
}

/**
 * Functie apelata in init(), parseaza fisierul cu fisierele share-uite si
 * modifica starea, daca e cazul, sau afiseaza la consola erorile din fisier.
 */
void parse_sscf(char *filename) {
    FILE *sscf = fopen(filename, "r");
    int n;


    fscanf(sscf, "%d\n", &n);
    char buf[MAX_STR_LENGTH * 2], userbuf[MAX_STR_LENGTH], filebuf[MAX_STR_LENGTH];
    for (int i = 0; i < n; ++i) {
        fgets(buf, 2 * MAX_STR_LENGTH, sscf);
        char *pch = strtok(buf, ":\n");
        strcpy(userbuf, pch);
        pch = strtok(NULL, ":\n");
        strcpy(filebuf, pch);

        std::string user_str = std::string(userbuf);
        std::string file_str = std::string(filebuf);

        if (users.find(user_str) == users.end()) // nu exista userul
            printf("%s: Line %d: user `%s` does not exist\n", filename, i + 2, userbuf);
        else if (users[user_str].files.find(file_str) == users[user_str].files.end())
            printf("%s: Line %d: `%s` in %s's homedir does not exist\n", filename, i + 2, filename, userbuf);
        else
            strcpy(users[user_str].files[file_str].share_status, "SHARED");
    }

    fclose(sscf);
}

/**
 * parseaza argumentele si seteaza variabila path.
 */
void sv_init(char **args) {

    getcwd(path, sizeof(path));
    // printf("cwd: `%s`\n", path);

    char ucf_filename[MAX_STR_LENGTH];
    char sscf_filename[MAX_STR_LENGTH];

    sscanf(args[1], "%d", &listen_port);
    sscanf(args[2], "%s", ucf_filename);
    sscanf(args[3], "%s", sscf_filename);

    // printf("OK1\n");
    parse_ucf(ucf_filename);
    // printf("OK2\n");
    parse_sscf(sscf_filename);

    printf("Awaiting connections...\n");
}

/**
 * Executa comanda login, verificand corectitudinea argumentelor si trimite si
 * raspunsul inapoi clientului. Actualizeaza toate campurile relevante.
 *
 * cmd este comanda extrasa din mesajul de la client
 * socket este socket-ul pe care serverul comunica cu clientul care a initiat
 * comanda
 */
int exec_login(command cmd, int socket) {

    std::string username = std::string(cmd.args[0]);
    std::string pass = std::string(cmd.args[1]);

    std::map<std::string, user>::iterator it;
    it = users.find(username);
    if (it == users.end()) { // user-ul nu exista deloc
        clients[socket].failed_login_attempts++;

        if (clients[socket].failed_login_attempts == 3) {
            message msg_to_send;
            msg_to_send.type = CMD_ERROR; // eroare
            strcpy((char*)msg_to_send.payload, "-8  Brute-force detectat");
            msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

            int bytecount = mySend(socket, msg_to_send);
            if (bytecount < 0)
                error("exec_login: bruteforce: ERROR in send");
            FD_CLR(socket, &read_fds);
            close(socket);
            printf("bruteforce attempt: closed connection on socket %d\n", socket);
            return -8;
        }

        message msg_to_send;
        msg_to_send.type = CMD_ERROR; // eroare
        strcpy((char *)msg_to_send.payload, "-3  User/parola gresita");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_login: wrong credentials: ERROR in send");

        return -3;
    } else if (users[username].pass != pass) { // parola difera
        clients[socket].failed_login_attempts++;

        if (clients[socket].failed_login_attempts == 3) {
            message msg_to_send;
            msg_to_send.type = CMD_ERROR; // eroare
            strcpy((char*)msg_to_send.payload, "-8  Brute-force detectat");
            msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

            int bytecount = mySend(socket, msg_to_send);
            if (bytecount < 0)
                error("exec_login: bruteforce: ERROR in send");
            FD_CLR(socket, &read_fds);
            close(socket);
            printf("bruteforce attempt: closed connection on socket %d\n", socket);
            return -8;
        }

        message msg_to_send;
        msg_to_send.type = CMD_ERROR; // eroare
        strcpy((char *)msg_to_send.payload, "-3  User/parola gresita");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_login: wrong credentials: ERROR in send");

        return -3;
    } else { // user parola corecte
        users[username].connected = true;
        users[username].socket = socket;
        clients[socket].connected_user = username;
        clients[socket].failed_login_attempts = 0;

        message msg_to_send;
        msg_to_send.type = CMD_SUCCESS; // success
        cmd_to_char(cmd, (char*)msg_to_send.payload);
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_login: success: ERROR in send");
    }

    return 0;
}

/**
 * Executa comanda logout, actualizand toate campurile relevante si trimite
 * raspunsul inapoi clientului.(doar CMD_SUCCESS)
 */
int exec_logout(command cmd, int socket) {

    std::string username = clients[socket].connected_user;
    users[username].socket = -1;
    users[username].connected = false;
    clients[socket].connected_user = std::string("none");

    message msg_to_send;
    msg_to_send.type = CMD_SUCCESS;
    cmd_to_char(cmd, (char*)msg_to_send.payload);
    msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

    int bytecount = mySend(socket, msg_to_send);
    if (bytecount < 0)
        error("exec_logout: success: ERROR in send");
    return 0;
}

/**
 * Executa comanda getuserlist, creeaza si trimite inapoi raspunsul.
 */
int exec_getuserlist(command cmd, int socket) {
    message msg_to_send;
    msg_to_send.type = CMD_SUCCESS;
    strcpy((char*)msg_to_send.payload, "");
    for (std::map<std::string, user>::iterator iter = users.begin(); iter != users.end(); iter++) {
        strcat((char*)msg_to_send.payload, iter->first.c_str());
        strcat((char*)msg_to_send.payload, "\n");
    }
    msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

    int bytecount = mySend(socket, msg_to_send);
    if (bytecount < 0)
        error("exec_getuserlist: success: ERROR in send");
    return 0;
}

/**
 * Executa comanda getfilelist, verifica corectitudinea argumentelor, creeaza si
 * trimite inapoi raspunsul.
 */
int exec_getfilelist(command cmd, int socket) {

    std::string user_str = std::string(cmd.args[0]);
    if (users.find(user_str) == users.end()) {
        message msg_to_send;
        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-11 Utilizator inexistent");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_getfilelist: wrong username: ERROR in send");
    } else {
        message msg_to_send;
        msg_to_send.type = CMD_SUCCESS;
        strcpy((char*)msg_to_send.payload, "");

        struct stat st = {0};
        std::map<std::string, disk_file>::iterator it;
        for (it = users[user_str].files.begin(); it != users[user_str].files.end(); it++) {
            strcat((char*)msg_to_send.payload, it->second.filename.c_str());
            stat(it->second.full_path, &st);
            char buf[BUFLEN];
            sprintf(buf, "\t%ld bytes\t", st.st_size);
            strcat((char*)msg_to_send.payload, buf);
            strcat((char*)msg_to_send.payload, it->second.share_status);
            strcat((char*)msg_to_send.payload, "\n");
        }
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_getfilelist: success: ERROR in send");
    }

    return 0;
}

/**
 * Executa comanda upload. (mai degraba o initiaza)
 * Daca nu sunt erori in argumente, creeaza fisierul in care se vor scrie datele
 * si trimite inapoi clientului file descriptor-ul de pe server.
 * Introduce fisierul nou creat in toate campurile relevante.
 */
int exec_upload(command cmd, int socket) {

    std::string user_str = clients[socket].connected_user;
    std::string file_str = cmd.args[0];

    if (users[user_str].files.find(file_str) != users[user_str].files.end()) {
        message msg_to_send;

        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-9  Fisier existent pe server");
        msg_to_send.length = strlen((char*)msg_to_send.payload);

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_upload: file overwrite: ERROR in send");
    } else {

        char path_buffer[BUFLEN];
        strcpy(path_buffer, path);
        strcat(path_buffer, "/");
        strcat(path_buffer, user_str.c_str());
        strcat(path_buffer, "/");
        strcat(path_buffer, file_str.c_str());
        // ^ am creat calea absoluta a fisierului nou

        int fd = open(path_buffer, O_WRONLY | O_CREAT);
        if (fd < 0)
            error("exec_upload: ERROR in open");
        // printf("FILE ON SERVER: %d\n", fd);

        sv_filestat newfs;
        newfs.filename = file_str;
        newfs.is_in_upload = true;
        newfs.is_in_download = false;
        newfs.cl_socket = socket;
        strcpy(newfs.full_path, path_buffer);
        files_to_transfer[fd] = newfs;

        disk_file newfile;
        newfile.filename = file_str;
        strcpy(newfile.full_path, newfs.full_path);
        strcpy(newfile.share_status, "PRIVATE");
        users[user_str].files[file_str] = newfile;

        message msg_to_send;
        msg_to_send.type = INIT_UPLOAD;
        sprintf((char*)msg_to_send.payload, "%d %s", fd, file_str.c_str());
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_upload: file ok: ERROR in send");
    }
    return 0;
}

/**
 * Executa comanda download. (mai degraba o initiaza)
 * Daca nu sunt erori in argumente si este permisa descarcarea, deschide
 * fisierul solicitat pentru citire si trimite inapoi clientului file
 * descriptor-ul acestuia.
 */
int exec_download(command cmd, int socket) {
    std::string user_str = clients[socket].connected_user;
    std::string requser_str = std::string(cmd.args[0]);
    std::string file_str = std::string(cmd.args[1]);

    // printf("user_str=`%s`; requser_str=`%s`\n", user_str.c_str(), requser_str.c_str());

    std::map<std::string, disk_file>::iterator it;
    it = users[requser_str].files.find(file_str);
    if (it == users[requser_str].files.end()) {
        message msg_to_send;

        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-4  Fisier inexistent");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        if (mySend(socket, msg_to_send) < 0)
            error("exec_download: wrong filename: ERROR in send");
    } else if ((!strcmp(it->second.share_status, "PRIVATE")) && (user_str != requser_str)) {
        message msg_to_send;

        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-5  Descarcare interzisa");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        if (mySend(socket, msg_to_send) < 0)
            error("exec_download: not shared: ERROR in send");
    } else {
        char path_buffer[BUFLEN];
        strcpy(path_buffer, path);
        strcat(path_buffer, "/");
        strcat(path_buffer, requser_str.c_str());
        strcat(path_buffer, "/");
        strcat(path_buffer, file_str.c_str());
        // ^ am creat calea absoluta a fisierului de deschis

        if (is_uploading(path_buffer)) {
            message msg_to_send;

            msg_to_send.type = CMD_ERROR;
            strcpy((char*)msg_to_send.payload, "-10 Fisier in transfer");
            msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

            if (mySend(socket, msg_to_send) < 0)
                error("exec_download: is_uploading: ERROR in send");
            return -10;
        }

        int fd = open(path_buffer, O_RDONLY);
        if (fd < 0)
            error("exec_upload: ERROR in open");
        // printf("FILE ON SERVER: %d\n", fd);

        sv_filestat newfs;
        newfs.filename = file_str;
        newfs.is_in_upload = false;
        newfs.is_in_download = true;
        newfs.cl_socket = socket;
        strcpy(newfs.full_path, path_buffer);
        files_to_transfer[fd] = newfs;

        message msg_to_send;
        msg_to_send.type = INIT_DOWNLOAD;
        sprintf((char*)msg_to_send.payload, "%d %s", fd, file_str.c_str());
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        if (mySend(socket, msg_to_send) < 0)
            error("exec_upload: file ok: ERROR in send");
    }
    return 0;
}

/**
 * Executa comanda share, actualizeaza structurile relevante, daca este posibil,
 * si trimite inapoi un raspuns corespunzator.
 */
int exec_share(command cmd, int socket) {

    std::string user_str = clients[socket].connected_user;
    std::string file_str = std::string(cmd.args[0]);

    if (users[user_str].files.find(file_str) == users[user_str].files.end()) {
        message msg_to_send;
        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-4  Fisier inexistent");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_share: wrong filename: ERROR in send");
    } else if (!strcmp(users[user_str].files[file_str].share_status, "SHARED")) {
        message msg_to_send;
        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-6  Fisierul este deja partajat");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_share: already shared: ERROR in send");
    } else {
        message msg_to_send;
        msg_to_send.type = CMD_SUCCESS;
        sprintf((char*)msg_to_send.payload, "200 Fisierul %s a fost partajat", cmd.args[0]);
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        strcpy(users[user_str].files[file_str].share_status, "SHARED");

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_share: success: ERROR in send");
    }
    return 0;
}

/**
 * Executa comanda unshare, actualizeaza structurile relevante, daca este
 * posibil, si trimite inapoi un raspuns corespunzator.
 */
int exec_unshare(command cmd, int socket) {

    std::string user_str = clients[socket].connected_user;
    std::string file_str = std::string(cmd.args[0]);

    if (users[user_str].files.find(file_str) == users[user_str].files.end()) {
        message msg_to_send;
        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-4  Fisier inexistent");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_unshare: wrong filename: ERROR in send");
    } else if (!strcmp(users[user_str].files[file_str].share_status, "PRIVATE")) {
        message msg_to_send;
        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-7  Fisier deja privat");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_unshare: already shared: ERROR in send");
    } else {
        message msg_to_send;
        msg_to_send.type = CMD_SUCCESS;
        sprintf((char*)msg_to_send.payload, "200 Fisierul %s a fost setat ca PRIVATE", cmd.args[0]);
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        strcpy(users[user_str].files[file_str].share_status, "PRIVATE");

        int bytecount = mySend(socket, msg_to_send);
        if (bytecount < 0)
            error("exec_unshare: success: ERROR in send");
    }
    return 0;
}

/**
 * Executa comanda delete, sterge fisierul cu unlink(), daca acest lucru este
 * posibil(exista si nu se afla in transferuri). Trimite inapoi un raspuns
 * relevant.
 */
int exec_delete(command cmd, int socket) {
    std::string file_str = cmd.args[0];
    std::string user_str = clients[socket].connected_user;

    std::map<std::string, disk_file>::iterator it;
    it = users[user_str].files.find(file_str);
    if (it == users[user_str].files.end()) {
        message msg_to_send;
        msg_to_send.type = CMD_ERROR;
        strcpy((char*)msg_to_send.payload, "-4  Fisier inexistent");
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        if (mySend(socket, msg_to_send) < 0)
            error("exec_delete: missing file: ERROR in send");
        return -4;
    } else {
        char path_buffer[BUFLEN];
        strcpy(path_buffer, path);
        strcat(path_buffer, "/");
        strcat(path_buffer, user_str.c_str());
        strcat(path_buffer, "/");
        strcat(path_buffer, file_str.c_str());

        if (is_uploading(path_buffer) || is_downloading(path_buffer)) {
            message msg_to_send;
            msg_to_send.type = CMD_ERROR;
            strcpy((char*)msg_to_send.payload, "-10 Fisier in transfer");
            msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

            if (mySend(socket, msg_to_send) < 0)
                error("exec_delete: file in use: ERROR in send");

            return -10;
        }

        users[user_str].files.erase(file_str);
        unlink(path_buffer);

        message msg_to_send;
        msg_to_send.type = CMD_SUCCESS;
        sprintf((char*)msg_to_send.payload, "200 Fisierul %s a fost sters", cmd.args[0]);
        msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

        if (mySend(socket, msg_to_send) < 0)
            error("exec_delete: success: ERROR in send");
    }

    return 0;
}

/**
 * Nimic de executat, practic. Quit-ul se realizeaza la client.
 */
int exec_quit(command cmd, int socket) {

    message msg_to_send;
    msg_to_send.type = CMD_SUCCESS;
    cmd_to_char(cmd, (char*)msg_to_send.payload);
    msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

    if (mySend(socket, msg_to_send) < 0)
        error("exec_quit: ERROR in send");

    return 0;
}

/**
 * Apeleaza functiile corespunzatoare, in functie de comanda primita.
 */
int execute(command cmd, int socket) {
    int result = 0;
    if (!strcmp(cmd.name, "login")) {
        result = exec_login(cmd, socket);
    } else if (!strcmp(cmd.name, "logout")) {
        result = exec_logout(cmd, socket);
    } else if (!strcmp(cmd.name, "getuserlist")) {
        result = exec_getuserlist(cmd, socket);
    } else if (!strcmp(cmd.name, "getfilelist")) {
        result = exec_getfilelist(cmd, socket);
    } else if (!strcmp(cmd.name, "upload")) {
        result = exec_upload(cmd, socket);
    } else if (!strcmp(cmd.name, "download")) {
        result = exec_download(cmd, socket);
    } else if (!strcmp(cmd.name, "share")) {
        result = exec_share(cmd, socket);
    } else if (!strcmp(cmd.name, "unshare")) {
        result = exec_unshare(cmd, socket);
    } else if (!strcmp(cmd.name, "delete")) {
        result = exec_delete(cmd, socket);
    } else if (!strcmp(cmd.name, "quit")) {
        result = exec_quit(cmd, socket);
    }

    // TODO: logging + errorcodes
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        error("Usage: ./server <port_server> <users_config_file> "
              "<static_shares_config_file>\n");

    sv_init(argv);

    int listen_sock, newsockfd;
    sockaddr_in serv_addr, cli_addr;

    int fdmax;
    socklen_t clilen;

    int i; // va parcurge toti descriptorii de fisier

    FD_ZERO(&read_fds);
    FD_ZERO(&temp_fds);

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0)
        error("ERORR opening socket");

    memset((char*) &serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(listen_port);

    int b = bind(listen_sock, (struct sockaddr *) &serv_addr, \
                 sizeof(struct sockaddr));
    if (b < 0)
        error("ERROR on binding");

    listen(listen_sock, MAX_CLIENTS);

    FD_SET(listen_sock, &read_fds);
    fdmax = listen_sock;

    int bytes_read; // nr de biti cititi din oricare socket sau file descriptor
    message received_message;
    command cmd;
    while (1) { // bucla principala, fara conditie de oprire
        temp_fds = read_fds;
        if (select(fdmax + 1, &temp_fds, NULL, NULL, NULL) == -1)
            error("ERROR in select");

        // looking for data to read
        for (i = 0; i <= fdmax; ++i) {
            if (FD_ISSET(i, &temp_fds)) {
                if (i == listen_sock) { // new connection
                    clilen = sizeof(cli_addr);
                    if ((newsockfd = accept(listen_sock, (struct sockaddr *)&cli_addr, &clilen)) == -1)
                        error("ERROR in accept");
                    else {
                        FD_SET(newsockfd, &read_fds);
                        if (newsockfd > fdmax)
                            fdmax = newsockfd;
                        printf("%s: new connection from %s on socket %d\n", argv[0], inet_ntoa(cli_addr.sin_addr), newsockfd);
                        client newclient;
                        newclient.connected_user = "";
                        newclient.failed_login_attempts = 0;
                        clients[newsockfd] = newclient;
                    }
                } else if (clients.find(i) != clients.end()) { // something from a client
                    if ((bytes_read = myRecv(i, &received_message)) <= 0) {
                        // error or connection closed by client
                        if (bytes_read == 0) { // connection closed
                            printf("%s: socket %d hung up\n", argv[0], i);
                            clients[i].connected_user = std::string("none");
                        } else
                            error("ERROR in recv");
                        close(i);
                        FD_CLR(i, &read_fds);
                    } else { // data from a client
                        if (received_message.type == CMD_CLIENT) { // command
                            // printf("%s: creating command from `%s`\n", argv[0], received_message.payload);
                            // printmsg(received_message);
                            cmd = create_cmd(received_message.payload, (char*)clients[i].connected_user.c_str());
                            // printf("\nAm creat comanda: `%s`\n", cmd.name);
                            execute(cmd, i);

                        } else if (received_message.type == INIT_UPLOAD2) {
                            // printmsg(received_message);

                            int cl_fd, sv_fd;
                            sscanf((char*) received_message.payload, "%d %d", &cl_fd, &sv_fd);

                            cl_to_sv[i][cl_fd] = sv_fd;
                            // ^ setez maparea ca sa stiu carui fisier de pe sv ii corespunde
                            // cel de la client

                        } else if (received_message.type == INIT_DOWNLOAD2) {
                            // printmsg(received_message);

                            int cl_fd, sv_fd;
                            sscanf((char*) received_message.payload, "%d %d", &cl_fd, &sv_fd);

                            cl_to_sv[i][cl_fd] = sv_fd;

                            FD_SET(sv_fd, &read_fds); // adaug fisierul in read_fds
                            if (sv_fd > fdmax)
                                fdmax = sv_fd;

                        } else if (received_message.type == FILE_TRANSFER) { // bucata de fisier
                            uint16_t cl_fd, sv_fd;
                            memcpy(&cl_fd, received_message.payload, 2);
                            sv_fd = cl_to_sv[i][cl_fd]; // socket-ul clientului este i
                            write(sv_fd, received_message.payload + 2, received_message.length - 2);

                        } else if (received_message.type == CLOSE_TRANSFER) {
                            uint16_t cl_fd, sv_fd;
                            memcpy(&cl_fd, received_message.payload, 2);
                            sv_fd = cl_to_sv[i][cl_fd];

                            close(sv_fd); // inchid fisierul uploadat
                            files_to_transfer[sv_fd].is_in_upload = false;

                            struct stat st = {0};
                            stat(files_to_transfer[sv_fd].full_path, &st);

                            message msg_to_send;
                            msg_to_send.type = CMD_SUCCESS;
                            sprintf((char*)msg_to_send.payload, "Upload finished: %s -- %ld bytes", files_to_transfer[sv_fd].filename.c_str(), st.st_size);
                            msg_to_send.length = strlen((char*)msg_to_send.payload) + 1;

                            if (mySend(i, msg_to_send) < 0)
                                error("upload success: ERROR on send");

                        }
                    }
                } else { // de citit din fisier, pentru download
                    unsigned char buffer[4096];
                    int byte_count = read(i, buffer, 4090);
                    int cl_socket = files_to_transfer[i].cl_socket;
                    uint16_t sv_fd;
                    sv_fd = i;

                    if (byte_count < 0)
                        error("citire din fisier: ERROR on read");

                    if (byte_count > 0) { //nu am ajuns la sfarsit
                        message msg_to_send;
                        msg_to_send.type = FILE_TRANSFER;
                        memcpy(msg_to_send.payload, &sv_fd, 2);
                        memcpy(msg_to_send.payload + 2, buffer, byte_count);
                        msg_to_send.length = byte_count + 2;

                        if (mySend(cl_socket, msg_to_send) < 0)
                            error("file_transfer: ERROR on send");
                    } else { // am ajuns la EOF
                        message msg_to_send;
                        msg_to_send.type = CLOSE_TRANSFER;
                        memcpy(msg_to_send.payload, &sv_fd, 2);
                        msg_to_send.length = 2;

                        FD_CLR(sv_fd, &read_fds);
                        close(sv_fd);
                        files_to_transfer[sv_fd].is_in_download = false;

                        if (mySend(cl_socket, msg_to_send) < 0)
                            error("close_transfer: ERROR on send");
                    }

                }
            }
        }

    }

    return 0;
}