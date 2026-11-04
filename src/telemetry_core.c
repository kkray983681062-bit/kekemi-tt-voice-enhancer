#include "telemetry_core.h"
#include <math.h>

/* Default layout for the verified ZEGO 3.25.0.52035 build. */
static const TTLayout default_layout = {0x20a4bd8, 0x19117e0, 0x18ce5f8, 0x18ced08, TT_LAYOUT_MODERN64};

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

static int legacy_pointer_ok(uintptr_t pointer, size_t width) {
    if (width == 4)
        return pointer >= 0x10000 && pointer <= UINT32_MAX && (pointer & 3) == 0;
    return pointer_ok(pointer);
}

static int legacy_read_pointer(TTMemoryReader read_memory, void *ctx, uintptr_t address,
                               size_t width, uintptr_t *value) {
    if (!legacy_pointer_ok(address, width)) return 0;
    if (width == 4) {
        uint32_t result;
        if (!read_memory(ctx, address, &result, sizeof(result))) return 0;
        *value = result;
    } else {
        uint64_t result;
        if (!read_memory(ctx, address, &result, sizeof(result)) || result > UINTPTR_MAX) return 0;
        *value = (uintptr_t)result;
    }
    return legacy_pointer_ok(*value, width);
}

static int legacy_read_offset(TTMemoryReader read_memory, void *ctx, uintptr_t base,
                              uintptr_t offset, size_t width, void *value, size_t length) {
    return legacy_pointer_ok(base, width) && offset <= UINTPTR_MAX - base &&
           read_memory(ctx, base + offset, value, length);
}

static int legacy_pointer_offset(TTMemoryReader read_memory, void *ctx, uintptr_t base,
                                 uintptr_t offset, size_t width, uintptr_t *value) {
    return legacy_pointer_ok(base, width) && offset <= UINTPTR_MAX - base &&
           legacy_read_pointer(read_memory, ctx, base + offset, width, value);
}

static TTSampleState sample_legacy(TTMemoryReader read_memory, void *ctx, uintptr_t base,
                                   const TTLayout *layout, double *kbps) {
    const size_t width = layout->kind == TT_LAYOUT_LEGACY32 ? 4 : 8;
    const uintptr_t manager_offset = width == 4 ? 0x30 : 0x60;
    const uintptr_t vector_offset = width == 4 ? 0x118 : 0x1e0;
    const uintptr_t channel_offset = width == 4 ? 0x8c : 0xe8;
    const uintptr_t stream_offset = width == 4 ? 0x90 : 0xf0;
    const uintptr_t state_offset = width == 4 ? 0x584 : 0x674;
    const uintptr_t rate_offset = width == 4 ? 0x968 : 0xb68;
    uintptr_t root, manager, begin, end;
    if (!legacy_pointer_offset(read_memory, ctx, base, layout->engine_singleton, width, &root) ||
        !legacy_pointer_offset(read_memory, ctx, root, manager_offset, width, &manager) ||
        !legacy_pointer_offset(read_memory, ctx, manager, vector_offset, width, &begin) ||
        !legacy_pointer_offset(read_memory, ctx, manager, vector_offset + width, width, &end) ||
        end < begin || end - begin > 16 * width * 2 || (end - begin) % (width * 2) != 0)
        return TT_SAMPLE_WAITING;

    for (uintptr_t slot = begin; slot < end; slot += width * 2) {
        uintptr_t publisher, publisher_vtable, stream, stream_vtable, stream_again;
        uintptr_t root_again, manager_again, begin_again, end_again, publisher_again;
        int channel, state, state_again;
        double value;
        if (!legacy_read_pointer(read_memory, ctx, slot, width, &publisher) ||
            !legacy_pointer_offset(read_memory, ctx, publisher, 0, width, &publisher_vtable) ||
            layout->publisher_vtable > UINTPTR_MAX - base || publisher_vtable != base + layout->publisher_vtable ||
            !legacy_read_offset(read_memory, ctx, publisher, channel_offset, width, &channel, sizeof(channel)) || channel != 0 ||
            !legacy_pointer_offset(read_memory, ctx, publisher, stream_offset, width, &stream) ||
            !legacy_pointer_offset(read_memory, ctx, stream, 0, width, &stream_vtable) ||
            layout->stream_vtable > UINTPTR_MAX - base || stream_vtable != base + layout->stream_vtable ||
            !legacy_read_offset(read_memory, ctx, stream, state_offset, width, &state, sizeof(state)) || state != 6 ||
            !legacy_read_offset(read_memory, ctx, stream, rate_offset, width, &value, sizeof(value)) ||
            !isfinite(value) || value < 0.0 || value > 1000.0 ||
            !legacy_read_offset(read_memory, ctx, stream, state_offset, width, &state_again, sizeof(state_again)) || state_again != 6 ||
            !legacy_pointer_offset(read_memory, ctx, publisher, stream_offset, width, &stream_again) || stream_again != stream ||
            !legacy_pointer_offset(read_memory, ctx, base, layout->engine_singleton, width, &root_again) || root_again != root ||
            !legacy_pointer_offset(read_memory, ctx, root, manager_offset, width, &manager_again) || manager_again != manager ||
            !legacy_pointer_offset(read_memory, ctx, manager, vector_offset, width, &begin_again) || begin_again != begin ||
            !legacy_pointer_offset(read_memory, ctx, manager, vector_offset + width, width, &end_again) || end_again != end ||
            !legacy_read_pointer(read_memory, ctx, slot, width, &publisher_again) || publisher_again != publisher)
            continue;
        *kbps = value;
        return TT_SAMPLE_ACTIVE;
    }
    return TT_SAMPLE_WAITING;
}

TTSampleState tt_sample_with_layout(TTMemoryReader read_memory, void *ctx,
                                    uintptr_t engine_base, const TTLayout *layout, double *kbps) {
    uintptr_t root, gateway, channels, channels_end;
    if (!read_memory || !kbps || !layout) return TT_SAMPLE_WAITING;
    if (layout->kind == TT_LAYOUT_LEGACY64 || layout->kind == TT_LAYOUT_LEGACY32)
        return sample_legacy(read_memory, ctx, engine_base, layout, kbps);
    if (layout->kind != TT_LAYOUT_MODERN64 || !pointer_ok(engine_base) ||
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
