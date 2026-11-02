/* A consent-gated, single-process integration probe. It keeps the same target handle
   for the entire read/change/restore cycle so a restart cannot redirect restoration. */
#include "../src/controller.c"
#include <stdio.h>

static TTCode put_config(Target *target, RemoteCall *remote, const AudioConfig *config, DWORD *error) {
    SIZE_T written = 0;
    if (!WriteProcessMemory(target->process, (LPVOID)remote->data, config, sizeof(*config), &written) ||
        written != sizeof(*config)) {
        *error = GetLastError();
        return TT_REMOTE_FAILED;
    }
    return remote_call(target, remote, target->set_audio_config, error);
}

static BOOL same_config(const AudioConfig *a, const AudioConfig *b) {
    return a->bitrate == b->bitrate && a->channel == b->channel && a->codec == b->codec;
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--confirm-exercise") != 0) {
        puts("CONFIRMATION_REQUIRED"); return 2;
    }
    Target target;
    TTResult found = find_target(TT_EDITION_ENTERTAINMENT, &target);
    if (found.code != TT_OK) { printf("FIND %s %lu\n", tt_code_name(found.code), (unsigned long)found.system_error); return 2; }
    if (!resolve_exports(&target)) { puts("INTERFACE_UNSUPPORTED"); CloseHandle(target.process); return 2; }
    RemoteCall remote;
    DWORD error = 0;
    TTCode code = remote_prepare(&target, &remote, &error);
    if (code != TT_OK) { printf("PREPARE %s %lu\n", tt_code_name(code), (unsigned long)error); CloseHandle(target.process); return 2; }
    int result = 2;
    AudioConfig original, requested, observed;
    code = remote_get(&target, &remote, &original, &error);
    if (code != TT_OK) {
        printf("READ %s %lu\n", tt_code_name(code), (unsigned long)error);
        goto done;
    }
    printf("ORIGINAL %d %d %d\n", original.bitrate, original.channel, original.codec); fflush(stdout);
    if (original.bitrate < TT_MIN_KBPS || original.bitrate > TT_MAX_KBPS) {
        puts("ORIGINAL_OUTSIDE_RESTORABLE_RANGE"); goto done;
    }
    requested = original;
    requested.bitrate = original.bitrate == 96 ? 128 : 96;
    code = put_config(&target, &remote, &requested, &error);
    printf("SET %s %lu\n", tt_code_name(code), (unsigned long)error); fflush(stdout);
    if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    BOOL set_ok = code == TT_OK;
    TTCode read_code = remote_get(&target, &remote, &observed, &error);
    if (read_code == TT_REMOTE_TIMEOUT) { puts("READBACK REMOTE_TIMEOUT"); result = 4; goto done; }
    BOOL tested = set_ok && read_code == TT_OK && same_config(&observed, &requested);
    if (read_code == TT_OK) printf("READBACK %d %d %d\n", observed.bitrate, observed.channel, observed.codec);
    else printf("READBACK %s %lu\n", tt_code_name(read_code), (unsigned long)error);
    fflush(stdout);
    if (!set_ok && read_code == TT_OK && same_config(&observed, &original)) {
        puts("UNCHANGED"); result = 3; goto done;
    }
    code = put_config(&target, &remote, &original, &error);
    printf("RESTORE %s %lu\n", tt_code_name(code), (unsigned long)error); fflush(stdout);
    if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    if (code != TT_OK) { result = 5; goto done; }
    code = remote_get(&target, &remote, &observed, &error);
    if (code == TT_OK) printf("RESTORED %d %d %d\n", observed.bitrate, observed.channel, observed.codec);
    else printf("RESTORED %s %lu\n", tt_code_name(code), (unsigned long)error);
    if (code == TT_REMOTE_TIMEOUT) result = 4;
    else if (code != TT_OK || !same_config(&observed, &original)) result = 5;
    else result = tested ? 0 : 3;
done:
    remote_free(&target, &remote);
    CloseHandle(target.process);
    return result;
}
