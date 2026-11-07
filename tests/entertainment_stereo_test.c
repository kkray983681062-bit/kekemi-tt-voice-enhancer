/* Explicitly authorized live test. Restores both the encoder config and capture mode. */
#include "../src/controller.c"
#include "../src/telemetry.c"
#include <stdio.h>

typedef struct {
    int mode, engine_mode, capture_channels, pending_channels;
    int codec_channels, sample_rate, bitrate;
} EntertainmentState;

static BOOL entertainment_state(const Target *target, EntertainmentState *state) {
    uintptr_t audio, device, engine, sessions, encoder, codec, value;
    return read_bytes(target->process, target->engine_base + 0x2115d80, &audio, 8) && audio &&
        read_bytes(target->process, audio + 0x60, &device, 8) && device &&
        read_bytes(target->process, device, &value, 8) && value == target->engine_base + 0x1bbae10 &&
        read_bytes(target->process, device + 0x748, &engine, 8) && engine &&
        read_bytes(target->process, engine, &value, 8) && value == target->engine_base + 0x1bd6260 &&
        read_bytes(target->process, device + 0x2284e0, &sessions, 8) && sessions &&
        read_bytes(target->process, sessions + 0xd8, &encoder, 8) && encoder &&
        read_bytes(target->process, encoder, &value, 8) && value == target->engine_base + 0x1bcc300 &&
        read_bytes(target->process, encoder + 0x118, &codec, 8) && codec &&
        read_bytes(target->process, codec + 8, &value, 8) && value == target->engine_base + 0xe46a50 &&
        read_bytes(target->process, device + 0x784, &state->mode, 4) &&
        read_bytes(target->process, engine + 0x2cc, &state->engine_mode, 4) &&
        read_bytes(target->process, engine + 0x3474, &state->capture_channels, 4) &&
        read_bytes(target->process, engine + 0x3d34, &state->pending_channels, 4) &&
        read_bytes(target->process, codec + 0x64, &state->codec_channels, 4) &&
        read_bytes(target->process, codec + 0x60, &state->sample_rate, 4) &&
        read_bytes(target->process, codec + 0x6c, &state->bitrate, 4);
}

static void show_state(const char *label, const AudioConfig *config, const EntertainmentState *state) {
    printf("%s config=%d,%d,%d capture_mode=%d capture_channels=%d codec_channels=%d sample_rate=%d codec_bps=%d\n",
        label, config->bitrate, config->channel, config->codec, state->mode,
        state->capture_channels, state->codec_channels, state->sample_rate, state->bitrate);
    fflush(stdout);
}

static BOOL wait_ent_state(const Target *target, int channels, int rate, EntertainmentState *state) {
    for (unsigned i = 0; i < 80; ++i) {
        if (entertainment_state(target, state) && state->capture_channels == channels &&
            state->codec_channels == channels && state->sample_rate == 48000 && state->bitrate == rate * 1000)
            return TRUE;
        Sleep(50);
    }
    return FALSE;
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--exercise-and-restore")) return 2;
    TTTarget identity; DWORD error = 0;
    if (tt_find_target(TT_EDITION_ENTERTAINMENT, &identity, &error) != TT_TARGET_FOUND ||
        file_profile(identity.engine_path) != 1) { puts("UNSUPPORTED_ENTERTAINMENT_ENGINE"); return 2; }
    Target target;
    TTResult found = find_target(TT_EDITION_ENTERTAINMENT, &target);
    if (found.code != TT_OK) { puts(tt_code_name(found.code)); return 2; }
    RemoteCall remote = {0};
    AudioConfig original, observed;
    EntertainmentState before, after;
    int result = 3;
    BOOL changed = FALSE;
    if (!tt_same_process(target.process, &identity) || !resolve_exports(&target) ||
        !loaded_code_matches(target.process, target.engine_base, &profiles[1]) ||
        !entertainment_state(&target, &before) ||
        remote_prepare(&target, &remote, &error) != TT_OK ||
        remote_get(&target, &remote, &original, &error) != TT_OK) goto done;
    show_state("ORIGINAL", &original, &before);
    if (original.bitrate < TT_MIN_KBPS || original.bitrate > TT_MAX_KBPS) goto done;

    TTResult step = tt_apply_channels_for(TT_EDITION_ENTERTAINMENT, 2);
    printf("STEREO %s\n", tt_code_name(step.code)); fflush(stdout);
    if (step.code != TT_OK) goto done;
    changed = TRUE;
    if (step.config.bitrate != original.bitrate || step.config.codec != original.codec ||
        step.config.channel != 2 || !wait_ent_state(&target, 2, original.bitrate, &after)) goto restore;
    show_state("STEREO_VERIFIED", &step.config, &after);

    step = tt_apply_bitrate_for(TT_EDITION_ENTERTAINMENT, 192);
    if (step.code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    if (step.code != TT_OK || step.config.channel != 2 ||
        !wait_ent_state(&target, 2, 192, &after)) goto restore;
    show_state("STEREO_192_VERIFIED", &step.config, &after);
    for (unsigned i = 0; i < 4; ++i) {
        Sleep(3000);
        TTMetric live = tt_read_telemetry_for(TT_EDITION_ENTERTAINMENT);
        printf("LIVE_AUDIO seconds=%u code=%d kbps=%.2f\n", (i + 1) * 3, live.code, live.kbps);
        fflush(stdout);
    }
    step = tt_apply_channels_for(TT_EDITION_ENTERTAINMENT, 3);
    if (step.code != TT_OUT_OF_RANGE) goto restore;
    step = tt_apply_channels_for(TT_EDITION_ENTERTAINMENT, 1);
    if (step.code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
    if (step.code != TT_OK || step.config.bitrate != 192 || !wait_ent_state(&target, 1, 192, &after)) goto restore;
    show_state("MONO_VERIFIED", &step.config, &after);
    result = 0;
restore:
    if (changed) {
        TTCode code = remote_config(&target, &remote, &original, &error);
        if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
        if (code != TT_OK) result = 5;
        code = remote_capture_mode(&target, &remote, before.mode, &error);
        if (code == TT_REMOTE_TIMEOUT) { result = 4; goto done; }
        if (code != TT_OK) result = 5;
        Sleep(1500);
        code = remote_get(&target, &remote, &observed, &error);
        if (code != TT_OK || !entertainment_state(&target, &after) ||
            memcmp(&original, &observed, sizeof(original)) || memcmp(&before, &after, sizeof(before))) result = 5;
        else show_state("RESTORED", &observed, &after);
    }
done:
    remote_free(&target, &remote);
    CloseHandle(target.process);
    printf("RESULT %d\n", result);
    return result;
}
