#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../src/locator.h"

static void print_path(const wchar_t *prefix, const wchar_t *path) {
    wchar_t line[TT_PATH_CAP + 80];
    char utf8[(TT_PATH_CAP + 80) * 4];
    swprintf(line, sizeof(line) / sizeof(line[0]), L"%ls %ls", prefix, path);
    WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    puts(utf8);
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && wcscmp(argv[1], L"save") == 0) {
        puts(tt_location_save(argv[2]) ? "SAVED" : "INVALID_OR_ERROR");
    } else if (argc == 2 && wcscmp(argv[1], L"load") == 0) {
        wchar_t path[TT_PATH_CAP];
        if (tt_location_load(path)) print_path(L"SAVED", path);
        else puts("NONE");
    } else if (argc == 2 && wcscmp(argv[1], L"find") == 0) {
        TTLocation result = tt_location_find();
        if (result.code == TT_LOCATION_FOUND) {
            const wchar_t *source = result.source == TT_LOCATION_SAVED ? L"SAVED" :
                                    result.source == TT_LOCATION_RUNNING ? L"RUNNING" :
                                    result.source == TT_LOCATION_REGISTRY ? L"REGISTRY" :
                                    result.source == TT_LOCATION_SHORTCUT ? L"SHORTCUT" : L"DISK";
            print_path(source, result.path);
        } else puts(result.code == TT_LOCATION_MULTIPLE ? "MULTIPLE" : "NONE");
    } else if (argc == 3 && wcscmp(argv[1], L"start") == 0) {
        TTLaunch result = tt_location_start(argv[2]);
        if (result.code == TT_LAUNCH_STARTED) printf("STARTED %lu\n", (unsigned long)result.pid);
        else if (result.code == TT_LAUNCH_ALREADY_RUNNING) puts("ALREADY_RUNNING");
        else printf("FAILED %lu\n", (unsigned long)result.system_error);
    } else return 2;
    return 0;
}
