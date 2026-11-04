#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/telemetry_core.h"

typedef struct { uintptr_t address; unsigned char bytes[8]; size_t length; } Cell;
static Cell cells[32];
static size_t count;

static void put(uintptr_t address, const void *value, size_t length) {
    for (size_t i = 0; i < count; ++i) {
        if (cells[i].address == address) {
            memcpy(cells[i].bytes, value, length);
            cells[i].length = length;
            return;
        }
    }
    cells[count].address = address;
    memcpy(cells[count].bytes, value, length);
    cells[count++].length = length;
}

static void ptr(uintptr_t address, uintptr_t value, int width) {
    if (width == 4) {
        uint32_t narrowed = (uint32_t)value;
        put(address, &narrowed, sizeof(narrowed));
    } else {
        uint64_t wide = (uint64_t)value;
        put(address, &wide, sizeof(wide));
    }
}

static void integer(uintptr_t address, int value) { put(address, &value, sizeof(value)); }
static void number(uintptr_t address, double value) { put(address, &value, sizeof(value)); }

static int read_fake(void *context, uintptr_t address, void *out, size_t length) {
    (void)context;
    for (size_t i = 0; i < count; ++i) {
        if (cells[i].address == address && cells[i].length == length) {
            memcpy(out, cells[i].bytes, length);
            return 1;
        }
    }
    return 0;
}

static int check(int condition, const char *message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

static int test_legacy(int width) {
    const uintptr_t base = width == 4 ? 0x10000000ULL : 0x700000000ULL;
    const uintptr_t root = 0x100000, manager = 0x200000, slots = 0x300000;
    const uintptr_t publisher = 0x400000, stream = 0x500000;
    const uintptr_t singleton = width == 4 ? 0x19da068 : 0x2266ba8;
    const uintptr_t publisher_vtable = width == 4 ? 0x157e160 : 0x1c81808;
    const uintptr_t stream_vtable = width == 4 ? 0x157e70c : 0x1c81e88;
    const uintptr_t manager_offset = width == 4 ? 0x30 : 0x60;
    const uintptr_t vector_offset = width == 4 ? 0x118 : 0x1e0;
    const uintptr_t channel_offset = width == 4 ? 0x8c : 0xe8;
    const uintptr_t stream_offset = width == 4 ? 0x90 : 0xf0;
    const uintptr_t state_offset = width == 4 ? 0x584 : 0x674;
    const uintptr_t rate_offset = width == 4 ? 0x968 : 0xb68;
    TTLayout layout = {singleton, 0, publisher_vtable, stream_vtable,
                       width == 4 ? TT_LAYOUT_LEGACY32 : TT_LAYOUT_LEGACY64};

    memset(cells, 0, sizeof(cells));
    count = 0;
    ptr(base + singleton, root, width);
    ptr(root + manager_offset, manager, width);
    ptr(manager + vector_offset, slots, width);
    ptr(manager + vector_offset + width, slots + (uintptr_t)width * 2, width);
    ptr(slots, publisher, width);
    ptr(publisher, base + publisher_vtable, width);
    integer(publisher + channel_offset, 0);
    ptr(publisher + stream_offset, stream, width);
    ptr(stream, base + stream_vtable, width);
    integer(stream + state_offset, 6);
    number(stream + rate_offset, 137.25);

    double rate = -1;
    if (!check(tt_sample_with_layout(read_fake, NULL, base, &layout, &rate) == TT_SAMPLE_ACTIVE &&
               fabs(rate - 137.25) < 0.001, width == 4 ? "32-bit legacy real-rate path" : "64-bit legacy real-rate path")) return 0;
    integer(stream + state_offset, 0);
    if (!check(tt_sample_with_layout(read_fake, NULL, base, &layout, &rate) == TT_SAMPLE_WAITING,
               "inactive stream must not be shown as live")) return 0;
    integer(stream + state_offset, 6);
    integer(publisher + channel_offset, 1);
    if (!check(tt_sample_with_layout(read_fake, NULL, base, &layout, &rate) == TT_SAMPLE_WAITING,
               "another publishing channel must not be mistaken for the main channel")) return 0;
    integer(publisher + channel_offset, 0);
    ptr(stream, base + stream_vtable + 4, width);
    if (!check(tt_sample_with_layout(read_fake, NULL, base, &layout, &rate) == TT_SAMPLE_WAITING,
               "unexpected stream layout must be rejected")) return 0;
    return 1;
}

int main(void) {
    if (!test_legacy(8) || !test_legacy(4)) return 1;
    puts("PASS: legacy 32-bit and 64-bit real-rate paths and guards");
    return 0;
}
