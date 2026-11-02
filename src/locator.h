#ifndef TT_LOCATOR_H
#define TT_LOCATOR_H

#include <windows.h>
#include "target.h"

enum { TT_PATH_CAP = 1024 };
typedef enum { TT_LOCATION_NONE, TT_LOCATION_FOUND, TT_LOCATION_MULTIPLE, TT_LOCATION_INCOMPLETE } TTLocationCode;
typedef enum {
    TT_LOCATION_SAVED, TT_LOCATION_RUNNING, TT_LOCATION_REGISTRY,
    TT_LOCATION_SHORTCUT, TT_LOCATION_DISK
} TTLocationSource;
typedef struct {
    TTLocationCode code;
    TTLocationSource source;
    wchar_t path[TT_PATH_CAP];
} TTLocation;

typedef enum { TT_LAUNCH_FAILED, TT_LAUNCH_STARTED, TT_LAUNCH_ALREADY_RUNNING } TTLaunchCode;
typedef struct { TTLaunchCode code; DWORD system_error; DWORD pid; } TTLaunch;

BOOL tt_location_validate(const wchar_t *input, wchar_t *normalized);
BOOL tt_location_load(wchar_t *path);
BOOL tt_location_save(const wchar_t *path);
TTLocation tt_location_find(void);
TTLaunch tt_location_start(const wchar_t *path);
BOOL tt_location_validate_for(TTEdition edition, const wchar_t *input, wchar_t *normalized);
BOOL tt_location_load_for(TTEdition edition, wchar_t *path);
BOOL tt_location_save_for(TTEdition edition, const wchar_t *path);
TTLocation tt_location_find_for(TTEdition edition);
TTLaunch tt_location_start_for(TTEdition edition, const wchar_t *path);

#endif
