#ifndef COMMANDS_H
#define COMMANDS_H

#include <string.h>
#include <map>
#include <string>
#include "utils.h"


// structura pentru comenzi
struct command {
    char name[MAX_STR_LENGTH];
    int argc;
    char args[2][MAX_STR_LENGTH];
};



command create_cmd(unsigned char *cmd, char *user);

void cmd_to_char(command cmd, char *dest);

#endif