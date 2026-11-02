#ifndef TT_TARGET_H
#define TT_TARGET_H

#include <windows.h>
#include <stdint.h>

typedef enum { TT_EDITION_GAME = 0, TT_EDITION_ENTERTAINMENT = 1, TT_EDITION_COUNT = 2 } TTEdition;
typedef enum {
    TT_TARGET_FOUND, TT_TARGET_NOT_RUNNING, TT_TARGET_NO_ENGINE,
    TT_TARGET_MULTIPLE, TT_TARGET_ACCESS, TT_TARGET_SYSTEM
} TTTargetCode;

typedef struct {
    DWORD pid;
    uintptr_t engine_base;
    DWORD engine_size;
    FILETIME created;
    wchar_t engine_path[MAX_PATH];
} TTTarget;

const wchar_t *tt_edition_executable(TTEdition edition);
TTTargetCode tt_find_target(TTEdition edition, TTTarget *target, DWORD *system_error);
BOOL tt_same_process(HANDLE process, const TTTarget *target);

#endif
