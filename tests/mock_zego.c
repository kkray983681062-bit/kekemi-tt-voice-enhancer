#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int bitrate;
    int channel;
    int codec;
} AudioConfig;

static AudioConfig current = {64, 1, 3};
static LONG reset_scheduled = 0;

static void record_event(const char *event, const AudioConfig *config) {
    FILE *file = fopen("Z:\\tmp\\tt-bitrate-fixture-events.log", "a");
    if (!file) return;
    fprintf(file, "%s %d %d %d\n", event, config->bitrate, config->channel, config->codec);
    fclose(file);
}

static DWORD WINAPI reset_once(LPVOID unused) {
    (void)unused;
    Sleep(1500);
    InterlockedExchange((LONG *)&current.bitrate, 64);
    record_event("RESET", &current);
    return 0;
}

__declspec(dllexport) int zego_express_get_audio_config(int index, AudioConfig *out) {
    (void)index;
    if (!out) return 1;
    *out = current;
    record_event("GET", out);
    return getenv("TT_MOCK_GET_ERROR") ? 10010 : 0;
}

__declspec(dllexport) int zego_express_set_audio_config(int index, const AudioConfig *config) {
    (void)index;
    if (!config) return 1;
    record_event("SET", config);
    const char *error = getenv("TT_MOCK_SET_ERROR");
    if (error) return atoi(error);
    current = *config;
    if (getenv("TT_MOCK_CLAMP")) current.bitrate = 128;
    if (getenv("TT_MOCK_RESET_ONCE") && InterlockedCompareExchange(&reset_scheduled, 1, 0) == 0) {
        HANDLE thread = CreateThread(NULL, 0, reset_once, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)module; (void)reason; (void)reserved;
    return TRUE;
}
