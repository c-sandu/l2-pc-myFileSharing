#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <dirent.h>
#include <map>
#include <string>
#include "utils.h"
#include "commands.h"

void error(const char *msg) {
    fprintf(stderr, "%s", msg);
    exit(1);
}

/**
 * Wrapper pentru send, introduce tipul mesajului si lungimea acestuia in
 * pachet.
 */
int mySend(int socket, message msg) {
    unsigned char buffer[BUFLEN];
    memcpy(buffer, &(msg.type), 2);
    memcpy(buffer + 2, &(msg.length), 2);
    memcpy(buffer + 4, msg.payload, msg.length);

    return send(socket, buffer, BUFLEN, 0);
}

/**
 * Wrapper pentru receive, extrage tipul mesajului si lungimea acestuia din
 * pachet.
 */
int myRecv(int socket, message* dest) {
    unsigned char buffer[BUFLEN];
    int ret = recv(socket, buffer, BUFLEN, 0);

    if (ret > 0) {
        dest->type = (buffer[1] << 8) + buffer[0];
        dest->length = (buffer[3] << 8) + buffer[2];
        memcpy(dest->payload, buffer + 4, dest->length);
    }
    return ret;
}

/**
 * A fost utilizata la testare, afiseaza un mesaj;
 */
void printmsg(message msg) {
    printf("mesaj = { type = %d\n \
        length = %d\n \
        content = `", msg.type, msg.length);
    for (int i = 0; i < msg.length; ++i) {
        printf("%c", msg.payload[i]);
    }

    printf("`\n}\n");
}