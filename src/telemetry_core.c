#include "telemetry_core.h"
#include <math.h>

/* Default layout for the verified ZEGO 3.25.0.52035 build. */
static const TTLayout default_layout = {0x20a4bd8, 0x19117e0, 0x18ce5f8, 0x18ced08};

static int pointer_ok(uintptr_t p) {
    return p >= 0x10000 && p <= (uintptr_t)0x00007fffffffffffULL && (p & 7) == 0;
}

static int fetch(TTMemoryReader read_memory, void *ctx, uintptr_t base,
                 uintptr_t offset, void *out, size_t length) {
    return pointer_ok(base) && offset <= UINTPTR_MAX - base &&
           read_memory(ctx, base + offset, out, length);
}

static int read_ptr(TTMemoryReader read_memory, void *ctx, uintptr_t base,
                    uintptr_t offset, uintptr_t *out) {
    return fetch(read_memory, ctx, base, offset, out, sizeof(*out)) && pointer_ok(*out);
}

static int list_bounds(TTMemoryReader read_memory, void *ctx, uintptr_t base,
                       uintptr_t offset, uintptr_t *begin, uintptr_t *end) {
    if (!read_ptr(read_memory, ctx, base, offset, begin) ||
        !read_ptr(read_memory, ctx, base, offset + 8, end)) return 0;
    return *end >= *begin && *end - *begin <= 16 * 16 && (*end - *begin) % 16 == 0;
}

TTSampleState tt_sample_with_layout(TTMemoryReader read_memory, void *ctx,
                                    uintptr_t engine_base, const TTLayout *layout, double *kbps) {
    uintptr_t root, gateway, channels, channels_end;
    if (!read_memory || !kbps || !layout || !pointer_ok(engine_base) ||
        !read_ptr(read_memory, ctx, engine_base, layout->engine_singleton, &root) ||
        !read_ptr(read_memory, ctx, root, 0x20, &gateway) ||
        !list_bounds(read_memory, ctx, gateway, 0xd0, &channels, &channels_end))
        return TT_SAMPLE_WAITING;

    for (uintptr_t slot = channels; slot < channels_end; slot += 16) {
        uintptr_t channel, native, vtable, owner, manager, manager_owner;
        uintptr_t publishers, publishers_end;
        if (!read_ptr(read_memory, ctx, slot, 0, &channel) ||
            !read_ptr(read_memory, ctx, channel, 0x10, &native) ||
            !read_ptr(read_memory, ctx, native, 0, &vtable) ||
            vtable != engine_base + layout->native_vtable ||
            !read_ptr(read_memory, ctx, native, 0x1c0, &owner) ||
            !read_ptr(read_memory, ctx, owner, 0x78, &manager) ||
            !read_ptr(read_memory, ctx, manager, 0x68, &manager_owner) ||
            manager_owner != owner ||
            !list_bounds(read_memory, ctx, manager, 0x1f8, &publishers, &publishers_end))
            continue;

        for (uintptr_t entry = publishers; entry < publishers_end; entry += 16) {
            uintptr_t publisher, stream, stream_owner, stream_again;
            int channel_id, state, state_again;
            double value;
            if (!read_ptr(read_memory, ctx, entry, 0, &publisher) ||
                !read_ptr(read_memory, ctx, publisher, 0, &vtable) ||
                vtable != engine_base + layout->publisher_vtable ||
                !fetch(read_memory, ctx, publisher, 0xf8, &channel_id, sizeof(channel_id)) ||
                channel_id != 0 ||
                !read_ptr(read_memory, ctx, publisher, 0x100, &stream) ||
                !read_ptr(read_memory, ctx, stream, 0, &vtable) ||
                vtable != engine_base + layout->stream_vtable ||
                !read_ptr(read_memory, ctx, stream, 0x408, &stream_owner) ||
                stream_owner != owner ||
                !fetch(read_memory, ctx, stream, 0x6d4, &state, sizeof(state)) || state != 6 ||
                !fetch(read_memory, ctx, stream, 0xc00, &value, sizeof(value)) ||
                !isfinite(value) || value < 0.0 || value > 1000.0 ||
                !fetch(read_memory, ctx, stream, 0x6d4, &state_again, sizeof(state_again)) ||
                state_again != 6 ||
                !read_ptr(read_memory, ctx, publisher, 0x100, &stream_again) || stream_again != stream)
                continue;
            *kbps = value;
            return TT_SAMPLE_ACTIVE;
        }
    }
    return TT_SAMPLE_WAITING;
}

TTSampleState tt_sample_core(TTMemoryReader read_memory, void *ctx,
                            uintptr_t engine_base, double *kbps) {
    return tt_sample_with_layout(read_memory, ctx, engine_base, &default_layout, kbps);
}
