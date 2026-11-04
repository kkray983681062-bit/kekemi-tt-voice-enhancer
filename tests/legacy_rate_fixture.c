#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *allocate(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

int main(void) {
    const int width = sizeof(uintptr_t);
    const uintptr_t singleton = width == 4 ? 0x19da068 : 0x2266ba8;
    const uintptr_t pub_vtable = width == 4 ? 0x157e160 : 0x1c81808;
    const uintptr_t stream_vtable = width == 4 ? 0x157e70c : 0x1c81e88;
    const uintptr_t manager_offset = width == 4 ? 0x30 : 0x60;
    const uintptr_t vector_offset = width == 4 ? 0x118 : 0x1e0;
    const uintptr_t channel_offset = width == 4 ? 0x8c : 0xe8;
    const uintptr_t stream_offset = width == 4 ? 0x90 : 0xf0;
    const uintptr_t state_offset = width == 4 ? 0x584 : 0x674;
    const uintptr_t rate_offset = width == 4 ? 0x968 : 0xb68;
    uintptr_t base = (uintptr_t)allocate(singleton + 4096);
    uintptr_t root = (uintptr_t)allocate(4096), manager = (uintptr_t)allocate(4096);
    uintptr_t slots = (uintptr_t)allocate(4096), publisher = (uintptr_t)allocate(4096);
    uintptr_t stream = (uintptr_t)allocate(4096);
    if (!base || !root || !manager || !slots || !publisher || !stream) return 2;
    *(uintptr_t *)(base + singleton) = root;
    *(uintptr_t *)(root + manager_offset) = manager;
    *(uintptr_t *)(manager + vector_offset) = slots;
    *(uintptr_t *)(manager + vector_offset + width) = slots + width * 2;
    *(uintptr_t *)slots = publisher;
    *(uintptr_t *)publisher = base + pub_vtable;
    *(int *)(publisher + channel_offset) = 0;
    *(uintptr_t *)(publisher + stream_offset) = stream;
    *(uintptr_t *)stream = base + stream_vtable;
    *(int *)(stream + state_offset) = getenv("TT_FIXTURE_INACTIVE") ? 0 : 6;
    *(double *)(stream + rate_offset) = 137.25;
    const char *ready_path = getenv("TT_FIXTURE_READY");
    if (!ready_path) ready_path = "Z:\\tmp\\tt-legacy-rate-ready.log";
    FILE *file = fopen(ready_path, "w");
    if (!file) return 3;
    fprintf(file, "%lu %llx %d\n", (unsigned long)GetCurrentProcessId(),
            (unsigned long long)base, width);
    fclose(file);
    Sleep(60000);
    return 0;
}
