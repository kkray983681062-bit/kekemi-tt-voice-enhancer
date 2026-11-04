#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { int bitrate, channel, codec; } AudioConfig;
static AudioConfig current = {64, 1, 3};
static int initial_rate = 64;
static LONG reset_scheduled = 0;

static void record(const char *event) {
    const char *path = getenv("TT_DUAL_EVENTS");
    if (!path) return;
    FILE *file = fopen(path, "a");
    if (file) { fprintf(file, "%s %d %d %d\n", event, current.bitrate, current.channel, current.codec); fclose(file); }
}

static DWORD WINAPI reset_once(LPVOID ignored) {
    (void)ignored;
    Sleep(1500);
    InterlockedExchange((LONG *)&current.bitrate, initial_rate);
    record("RESET");
    return 0;
}

int mock_get_by_channel(int index, int channel, AudioConfig *out) {
    (void)index;
    if (channel != 0 || !out) return 1;
    *out = current;
    record("GET");
    return 0;
}

int mock_set_by_channel(int index, AudioConfig config, int channel) {
    (void)index;
    if (channel != 0) return 1;
    current = config;
    record("SET");
    if (getenv("TT_DUAL_RESET") && InterlockedCompareExchange(&reset_scheduled, 1, 0) == 0) {
        HANDLE thread = CreateThread(NULL, 0, reset_once, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return 0;
}

#include "mock_modern_exports.h"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)module; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        const char *value = getenv("TT_DUAL_INITIAL");
        if (value) initial_rate = atoi(value);
        current.bitrate = initial_rate;
    }
    return TRUE;
}
