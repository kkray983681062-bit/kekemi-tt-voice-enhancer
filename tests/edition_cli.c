#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/controller.h"

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    TTEdition edition = strcmp(argv[1], "entertainment") == 0 ? TT_EDITION_ENTERTAINMENT : TT_EDITION_GAME;
    TTResult result;
    if (argc == 3 && strcmp(argv[2], "read") == 0) result = tt_read_config_for(edition);
    else if (argc == 4 && strcmp(argv[2], "set") == 0) result = tt_apply_bitrate_for(edition, atoi(argv[3]));
    else return 2;
    if (result.code != TT_OK) {
        printf("%s %lu\n", tt_code_name(result.code), (unsigned long)result.system_error);
        return 1;
    }
    printf("OK %d %d %d\n", result.config.bitrate, result.config.channel, result.config.codec);
    return 0;
}
