#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/telemetry_core.h"

static int read_process(void *context, uintptr_t address, void *out, size_t length) {
    SIZE_T got = 0;
    return ReadProcessMemory((HANDLE)context, (LPCVOID)address, out, length, &got) && got == length;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    DWORD pid = strtoul(argv[1], NULL, 10);
    uintptr_t base = (uintptr_t)_strtoui64(argv[2], NULL, 16);
    int width = atoi(argv[3]);
    if (width != 4 && width != 8) return 2;
    TTLayout layout = width == 4 ?
        (TTLayout){0x19da068, 0, 0x157e160, 0x157e70c, TT_LAYOUT_LEGACY32} :
        (TTLayout){0x2266ba8, 0, 0x1c81808, 0x1c81e88, TT_LAYOUT_LEGACY64};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) { printf("OPEN_FAILED %lu\n", (unsigned long)GetLastError()); return 1; }
    double rate = 0;
    TTSampleState state = tt_sample_with_layout(read_process, process, base, &layout, &rate);
    CloseHandle(process);
    if (state == TT_SAMPLE_ACTIVE) printf("ACTIVE %.2f\n", rate);
    else puts("WAITING");
    return 0;
}
