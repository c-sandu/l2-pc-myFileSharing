#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <map>
#include <stdint.h>

#define MAX_STR_LENGTH 50 // nume useri, fisiere etc.
#define MAX_CLIENTS 100
#define BUFLEN 4096 // buffere

#define CMD_ERROR       0   // comanda a generat eroare
#define CMD_SUCCESS     1   // comanda s-a executat cu succes
#define CMD_CLIENT      2   // pachet cu comanda de la client
#define INIT_UPLOAD     3   // pachet care initiaza upload-ul(sv -> cl)
#define INIT_UPLOAD2    4   // pachet care initiaza upload-ul(cl -> sv)
#define INIT_DOWNLOAD   5   // pachet care initiaza download-ul(sv -> cl)
#define INIT_DOWNLOAD2  6   // pachet care initiaza download-ul(cl -> sv)
#define FILE_TRANSFER   7   // pachet cu o parte dintr-un fisier (sv <-> cl)
#define CLOSE_TRANSFER  8   // pachet care anunta terminarea transferului (sv <-> cl)


// structura pentru fisierele de pe disc
struct disk_file {
    std::string filename;
    char share_status[MAX_STR_LENGTH]; // "SHARED" / "PRIVATE"
    char full_path[4 * MAX_STR_LENGTH];
};

// structura pentru user
struct user {
    std::string name;
    std::string pass;
    std::map<std::string, disk_file> files; // lista fisierelor detinute de user
    bool connected;
    int socket;
};

// structura pentru mesaje
struct message {
    uint16_t type; // tipurile sunt cele definite mai sus(0 - 8)
    uint16_t length;
    unsigned char payload[BUFLEN - 4];
};

// structura pentru clienti
struct client {
    std::string connected_user;
    int failed_login_attempts;
};

// structura pentru fisierele aflate in transfer pe server
struct sv_filestat {
    std::string filename;
    int cl_socket;
    char full_path[4 * MAX_STR_LENGTH];
    bool is_in_upload;
    bool is_in_download;
};

// structura pentru fisierele aflate in transfer la client
struct cl_filestat {
    int fd;
    char full_path[4 * MAX_STR_LENGTH];
    bool is_open;
};

// ----------------------------------------------------------------------------


void error(const char* msg);

int mySend(int socket, message msg);

int myRecv(int socket, message* msg);

void printmsg(message msg);

#endif