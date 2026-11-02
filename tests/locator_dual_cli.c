#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../src/locator.h"

static void print_path(const wchar_t *path) {
    char utf8[TT_PATH_CAP * 4];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL);
    printf("SAVED %s\n", utf8);
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) return 2;
    TTEdition edition = wcscmp(argv[1], L"entertainment") == 0 ? TT_EDITION_ENTERTAINMENT : TT_EDITION_GAME;
    if (argc == 4 && wcscmp(argv[2], L"save") == 0) {
        puts(tt_location_save_for(edition, argv[3]) ? "SAVED" : "INVALID_OR_ERROR");
    } else if (argc == 3 && wcscmp(argv[2], L"load") == 0) {
        wchar_t path[TT_PATH_CAP];
        if (tt_location_load_for(edition, path)) print_path(path); else puts("NONE");
    } else if (argc == 4 && wcscmp(argv[2], L"start") == 0) {
        TTLaunch result = tt_location_start_for(edition, argv[3]);
        if (result.code == TT_LAUNCH_STARTED) printf("STARTED %lu\n", (unsigned long)result.pid);
        else if (result.code == TT_LAUNCH_ALREADY_RUNNING) puts("ALREADY_RUNNING");
        else printf("FAILED %lu\n", (unsigned long)result.system_error);
    } else return 2;
    return 0;
}
