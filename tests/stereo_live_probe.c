/* Short, explicitly requested read/change/restore experiment for game SDK 52035.
   Reads scalar settings only. Never records PCM, packets, or conversation data. */
#include "../src/controller.c"
#include "../src/telemetry.c"
#include <stdio.h>

typedef struct {
    int capture_mode;
    int engine_mode;
    int capture_channels;
    int pending_channels;
} CaptureState;

static BOOL capture_state(const Target *target, CaptureState *state) {
    uintptr_t audio, device, engine, table;
    return read_bytes(target->process, target->engine_base + 0x20a4380, &audio, 8) && audio &&
        read_bytes(target->process, audio + 0x60, &device, 8) && device &&
        read_bytes(target->process, device, &table, 8) && table == target->engine_base + 0x1947f60 &&
        read_bytes(target->process, device + 0x748, &engine, 8) && engine &&
        read_bytes(target->process, engine, &table, 8) && table == target->engine_base + 0x1960750 &&
        read_bytes(target->process, device + 0x784, &state->capture_mode, 4) &&
        read_bytes(target->process, engine + 0x2cc, &state->engine_mode, 4) &&
        read_bytes(target->process, engine + 0x3474, &state->capture_channels, 4) &&
        read_bytes(target->process, engine + 0x3d34, &state->pending_channels, 4) &&
        state->capture_mode >= 0 && state->capture_mode <= 2 &&
        state->engine_mode >= 0 && state->engine_mode <= 2 &&
        state->capture_channels >= 1 && state->capture_channels <= 2;
}

static void print_state(const char *label, const AudioConfig *config, const CaptureState *state) {
    printf("%s bitrate=%d channel=%d codec=%d capture_mode=%d engine_mode=%d capture_channels=%d pending_channels=%d\n",
        label, config->bitrate, config->channel, config->codec, state->capture_mode,
        state->engine_mode, state->capture_channels, state->pending_channels);
    fflush(stdout);
}

static TTCode put_config(const Target *target, RemoteCall *remote, const AudioConfig *config, DWORD *error) {
    SIZE_T written;
    if (!WriteProcessMemory(target->process, (LPVOID)remote->data, config, sizeof(*config), &written) || written != sizeof(*config)) {
        *error = GetLastError(); return TT_REMOTE_FAILED;
    }
    return remote_call(target, remote, target->set_audio_config, TRUE, error);
}

static TTCode put_capture(const Target *target, RemoteCall *remote, int mode, DWORD *error) {
    /* Outer remote_call supplies the engine index. This shim replaces its second argument. */
    unsigned char shim[] = {0xba,0,0,0,0,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    uint64_t function = target->engine_base + 0xb97b10;
    memcpy(shim + 1, &mode, 4); memcpy(shim + 7, &function, 8);
    DWORD old; SIZE_T written;
    if (!VirtualProtectEx(target->process, (LPVOID)remote->code, 4096, PAGE_READWRITE, &old) ||
        !WriteProcessMemory(target->process, (LPVOID)(remote->code + 128), shim, sizeof(shim), &written) || written != sizeof(shim) ||
        !FlushInstructionCache(target->process, (LPCVOID)(remote->code + 128), sizeof(shim))) {
        *error = GetLastError(); return TT_REMOTE_FAILED;
    }
    return remote_call(target, remote, remote->code + 128, FALSE, error);
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--exercise-and-restore") != 0) {
        puts("Use --exercise-and-restore only with user authorization."); return 2;
    }
    TTTarget identity; DWORD error = 0;
    if (tt_find_target(TT_EDITION_GAME, &identity, &error) != TT_TARGET_FOUND || file_profile(identity.engine_path) != 0) {
        puts("UNSUPPORTED_GAME_ENGINE"); return 2;
    }
    Target target;
    TTResult found = find_target(TT_EDITION_GAME, &target);
    if (found.code != TT_OK) { puts(tt_code_name(found.code)); return 2; }
    int result = 2;
    RemoteCall remote = {0};
    CaptureState original_capture, observed_capture;
    AudioConfig original, observed, requested;
    if (target.pid != identity.pid || !tt_same_process(target.process, &identity) ||
        !resolve_exports(&target) || target.abi != ABI_MODERN64 ||
        !loaded_code_matches(target.process, target.engine_base, &profiles[0]) ||
        !capture_state(&target, &original_capture)) { puts("STATE_UNSUPPORTED"); goto done; }
    unsigned char mode_code[12];
    static const unsigned char expected[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48};
    if (!read_rva(&target, 0xb97b10, mode_code, sizeof(mode_code)) || memcmp(mode_code, expected, sizeof(expected))) {
        puts("CAPTURE_INTERFACE_MISMATCH"); goto done;
    }
    TTCode code = remote_prepare(&target, &remote, &error);
    if (code != TT_OK) goto done;
    code = remote_get(&target, &remote, &original, &error);
    if (code != TT_OK) { printf("ORIGINAL_READ %s %lu\n", tt_code_name(code), (unsigned long)error); goto done; }
    print_state("ORIGINAL", &original, &original_capture);
    requested = original; requested.channel = 2;
    code = put_capture(&target, &remote, 1, &error);
    printf("SET_CAPTURE %s %lu\n", tt_code_name(code), (unsigned long)error); fflush(stdout);
    if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    if (code != TT_OK) goto restore;
    code = put_config(&target, &remote, &requested, &error);
    printf("SET_CONFIG %s %lu\n", tt_code_name(code), (unsigned long)error); fflush(stdout);
    if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    if (code != TT_OK) goto restore;
    Sleep(2000);
    code = remote_get(&target, &remote, &observed, &error);
    if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    if (code == TT_OK && capture_state(&target, &observed_capture)) {
        print_state("OBSERVED", &observed, &observed_capture);
        result = observed.bitrate == original.bitrate && observed.codec == original.codec &&
            observed.channel == 2 && observed_capture.capture_mode == 1 &&
            observed_capture.engine_mode == 1 && observed_capture.capture_channels == 2 ? 0 : 3;
    }
restore:
    code = put_config(&target, &remote, &original, &error);
    printf("RESTORE_CONFIG %s %lu\n", tt_code_name(code), (unsigned long)error); fflush(stdout);
    if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    if (code != TT_OK) result = 5;
    code = put_capture(&target, &remote, original_capture.capture_mode, &error);
    printf("RESTORE_CAPTURE %s %lu\n", tt_code_name(code), (unsigned long)error); fflush(stdout);
    if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    if (code != TT_OK) result = 5;
    Sleep(2000);
    code = remote_get(&target, &remote, &observed, &error);
    if (code == TT_OK && capture_state(&target, &observed_capture)) {
        print_state("RESTORED", &observed, &observed_capture);
        if (memcmp(&original, &observed, sizeof(original)) ||
            memcmp(&original_capture, &observed_capture, sizeof(original_capture))) result = 5;
    } else result = 5;
done:
    remote_free(&target, &remote);
    CloseHandle(target.process);
    printf("RESULT %d (0=local capture/config passed; 3=not effective; 4=timeout; 5=restore incomplete)\n", result);
    return result;
}
