#include <windows.h>
#include <stdio.h>
#include "../src/locator.h"

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    wchar_t normalized[TT_PATH_CAP];
    if (!tt_location_validate_for(TT_EDITION_GAME, argv[1], normalized)) {
        fputs("FAIL: 32-bit TT executable is rejected\n", stderr);
        return 1;
    }
    puts("PASS: 32-bit TT executable can be selected");
    return 0;
}
