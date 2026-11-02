#include "../src/controller.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv) {
    TTResult result;
    const char *action;
    if (argc == 2 && strcmp(argv[1], "--read") == 0) {
        result = tt_read_config();
        action = "READ";
    } else if (argc == 3 && strcmp(argv[1], "--set") == 0) {
        char *end = NULL;
        errno = 0;
        long value = strtol(argv[2], &end, 10);
        if (!argv[2][0] || !end || *end || errno == ERANGE) {
            puts("OUT_OF_RANGE");
            return 1;
        }
        result = tt_apply_bitrate((int)value);
        action = "APPLIED";
    } else {
        puts("BAD_ARGUMENTS");
        return 1;
    }
    if (result.code != TT_OK) {
        printf("%s %lu\n", tt_code_name(result.code), (unsigned long)result.system_error);
        return 1;
    }
    printf("%s %d %d %d\n", action, result.config.bitrate,
           result.config.channel, result.config.codec);
    return 0;
}
