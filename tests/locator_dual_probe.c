#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../src/locator.h"

int wmain(int argc, wchar_t **argv) {
    TTEdition edition = argc > 1 && wcscmp(argv[1], L"entertainment") == 0 ?
                        TT_EDITION_ENTERTAINMENT : TT_EDITION_GAME;
    TTLocation result = tt_location_find_for(edition);
    char path[TT_PATH_CAP * 4] = {0};
    if (result.code == TT_LOCATION_FOUND)
        WideCharToMultiByte(CP_UTF8, 0, result.path, -1, path, sizeof(path), NULL, NULL);
    printf("code=%d source=%d path=%s\n", result.code, result.source, path);
    return result.code == TT_LOCATION_FOUND ? 0 : 2;
}
