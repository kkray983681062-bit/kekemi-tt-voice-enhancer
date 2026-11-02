#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/telemetry_core.h"

typedef struct { uintptr_t address; unsigned char value[8]; size_t length; } Cell;
static Cell cells[64];
static size_t cell_count;
static const uintptr_t base = (uintptr_t)0x700000000ULL;
enum { ROOT=0x100000, GATE=0x200000, CHANNELS=0x300000, CHANNEL=0x400000,
       NATIVE=0x500000, CONTEXT=0x600000, MANAGER=0x700000,
       PARENTS=0x800000, PARENT=0x900000, STREAM=0xa00000 };

static void put(uintptr_t address, const void *value, size_t length) {
    for (size_t i = 0; i < cell_count; ++i) {
        if (cells[i].address == address) {
            memcpy(cells[i].value, value, length); cells[i].length = length; return;
        }
    }
    cells[cell_count].address = address;
    memcpy(cells[cell_count].value, value, length);
    cells[cell_count++].length = length;
}
static void pointer(uintptr_t address, uintptr_t value) { put(address, &value, sizeof(value)); }
static void integer(uintptr_t address, int value) { put(address, &value, sizeof(value)); }
static void number(uintptr_t address, double value) { put(address, &value, sizeof(value)); }
static int read_fake(void *context, uintptr_t address, void *out, size_t length) {
    (void)context;
    for (size_t i = 0; i < cell_count; ++i) {
        if (cells[i].address == address && cells[i].length == length) {
            memcpy(out, cells[i].value, length); return 1;
        }
    }
    return 0;
}
static int require(int condition, const char *message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message); return 0;
}

int main(void) {
    pointer(base + 0x20a4bd8, ROOT); pointer(ROOT + 0x20, GATE);
    pointer(GATE + 0xd0, CHANNELS); pointer(GATE + 0xd8, CHANNELS + 16);
    pointer(CHANNELS, CHANNEL); pointer(CHANNEL + 0x10, NATIVE);
    pointer(NATIVE, base + 0x19117e0); pointer(NATIVE + 0x1c0, CONTEXT);
    pointer(CONTEXT + 0x78, MANAGER); pointer(MANAGER + 0x68, CONTEXT);
    pointer(MANAGER + 0x1f8, PARENTS); pointer(MANAGER + 0x200, PARENTS + 16);
    pointer(PARENTS, PARENT); pointer(PARENT, base + 0x18ce5f8);
    integer(PARENT + 0xf8, 0); pointer(PARENT + 0x100, STREAM);
    pointer(STREAM, base + 0x18ced08); pointer(STREAM + 0x408, CONTEXT);
    integer(STREAM + 0x6d4, 6); number(STREAM + 0xc00, 201.21);

    double rate = -1;
    if (!require(tt_sample_core(read_fake, NULL, base, &rate) == TT_SAMPLE_ACTIVE &&
                 fabs(rate - 201.21) < 0.001, "active stream uses measured kbps, including above configured limit")) return 1;
    number(STREAM + 0xc00, 39.5);
    if (!require(tt_sample_core(read_fake, NULL, base, &rate) == TT_SAMPLE_ACTIVE &&
                 fabs(rate - 39.5) < 0.001, "changed measured value is returned")) return 1;
    integer(STREAM + 0x6d4, 0);
    if (!require(tt_sample_core(read_fake, NULL, base, &rate) == TT_SAMPLE_WAITING, "inactive publisher is not shown as live data")) return 1;
    integer(STREAM + 0x6d4, 6); integer(PARENT + 0xf8, 1);
    if (!require(tt_sample_core(read_fake, NULL, base, &rate) == TT_SAMPLE_WAITING, "a different channel is not mistaken for the main channel")) return 1;
    integer(PARENT + 0xf8, 0); pointer(STREAM, base + 0x18ced09);
    if (!require(tt_sample_core(read_fake, NULL, base, &rate) == TT_SAMPLE_WAITING, "unexpected stream layout is rejected")) return 1;
    pointer(STREAM, base + 0x18ced08); number(STREAM + 0xc00, NAN);
    if (!require(tt_sample_core(read_fake, NULL, base, &rate) == TT_SAMPLE_WAITING, "invalid numerical readings are rejected")) return 1;
    number(STREAM + 0xc00, 30); pointer(MANAGER + 0x200, PARENTS + 4096);
    if (!require(tt_sample_core(read_fake, NULL, base, &rate) == TT_SAMPLE_WAITING, "invalid publisher list bounds are rejected")) return 1;

    memset(cells, 0, sizeof(cells)); cell_count = 0;
    const TTLayout entertainment = {0x21165d8, 0x1b82b48, 0x1b3a5d8, 0x1b3ace8};
    pointer(base + entertainment.engine_singleton, ROOT); pointer(ROOT + 0x20, GATE);
    pointer(GATE + 0xd0, CHANNELS); pointer(GATE + 0xd8, CHANNELS + 16);
    pointer(CHANNELS, CHANNEL); pointer(CHANNEL + 0x10, NATIVE);
    pointer(NATIVE, base + entertainment.native_vtable); pointer(NATIVE + 0x1c0, CONTEXT);
    pointer(CONTEXT + 0x78, MANAGER); pointer(MANAGER + 0x68, CONTEXT);
    pointer(MANAGER + 0x1f8, PARENTS); pointer(MANAGER + 0x200, PARENTS + 16);
    pointer(PARENTS, PARENT); pointer(PARENT, base + entertainment.publisher_vtable);
    integer(PARENT + 0xf8, 0); pointer(PARENT + 0x100, STREAM);
    pointer(STREAM, base + entertainment.stream_vtable); pointer(STREAM + 0x408, CONTEXT);
    integer(STREAM + 0x6d4, 6); number(STREAM + 0xc00, 72.73);
    if (!require(tt_sample_with_layout(read_fake, NULL, base, &entertainment, &rate) == TT_SAMPLE_ACTIVE &&
                 fabs(rate - 72.73) < 0.001, "entertainment profile uses its own verified layout")) return 1;
    if (!require(tt_sample_core(read_fake, NULL, base, &rate) == TT_SAMPLE_WAITING,
                 "the game profile must not interpret the entertainment layout")) return 1;
    puts("PASS: measured values, main-channel selection, inactivity and unsafe layouts");
    return 0;
}
