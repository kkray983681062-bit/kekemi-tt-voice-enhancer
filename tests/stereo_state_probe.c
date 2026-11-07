/* Read-only checks of both live, fingerprinted audio engines. No SDK setters. */
#include "../src/controller.c"
#include <stdio.h>

int main(int argc, char **argv) {
    int first = TT_EDITION_GAME, last = TT_EDITION_COUNT;
    if (argc == 2 && strcmp(argv[1], "game") == 0) last = TT_EDITION_ENTERTAINMENT;
    else if (argc == 2 && strcmp(argv[1], "entertainment") == 0) first = TT_EDITION_ENTERTAINMENT;
    else if (argc != 1) return 2;
    for (int edition = first; edition < last; ++edition) {
        Target target;
        TTResult found = find_target((TTEdition)edition, &target);
        if (found.code != TT_OK) { puts(tt_code_name(found.code)); return 2; }
        TTChannelSnapshot state;
        BOOL ok = stereo_profile(&target) && channel_snapshot(&target, &state);
        Target incorrect = target;
        incorrect.engine_size = channel_layouts[edition == TT_EDITION_GAME ? 1 : 0].image_size;
        ok = ok && !stereo_profile(&incorrect);
        incorrect.engine_size = 0;
        ok = ok && !stereo_profile(&incorrect);
        if (ok) {
            const TTChannelLayout *layout = stereo_layout(&target);
            uintptr_t audio, device, sessions, encoder, codec;
            int sample_rate, bitrate;
            ok = read_bytes(target.process, target.engine_base + layout->audio_root, &audio, 8) &&
                read_bytes(target.process, audio + 0x60, &device, 8) &&
                read_bytes(target.process, device + layout->sessions_offset, &sessions, 8) &&
                read_bytes(target.process, sessions + 0xd8, &encoder, 8) &&
                read_bytes(target.process, encoder + 0x118, &codec, 8) &&
                read_bytes(target.process, codec + 0x60, &sample_rate, 4) &&
                read_bytes(target.process, codec + 0x6c, &bitrate, 4);
            if (ok) printf("%s capture=%d codec_channels=%d codec_sample_rate=%d codec_bps=%d\n",
                edition == TT_EDITION_GAME ? "GAME" : "ENTERTAINMENT",
                state.capture_channels, state.encoder_channels, sample_rate, bitrate);
        }
        CloseHandle(target.process);
        if (!ok) { puts("FAIL: fingerprint, layout or live state check"); return 1; }
    }
    puts("PASS: selected live codec layouts and mismatched-profile rejection");
    return 0;
}
