#include <stdio.h>
#include "../src/telemetry.c"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Expected paths for the 3.25 x64, 3.21 x64 and 3.21 x86 DLLs.\n");
        return 2;
    }
    const TTLayoutKind expected[] = {TT_LAYOUT_MODERN64, TT_LAYOUT_LEGACY64, TT_LAYOUT_LEGACY32};
    for (int i = 0; i < 3; ++i) {
        wchar_t path[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, path, MAX_PATH)) return 2;
        int index = file_profile(path);
        if (index < 0) {
            fprintf(stderr, "FAIL: supplied DLL %d is not recognized\n", i + 1);
            return 1;
        }
        if (profiles[index].layout.kind != expected[i]) {
            fprintf(stderr, "FAIL: supplied DLL %d selects the wrong layout\n", i + 1);
            return 1;
        }
        if (i == 0) {
            HMODULE module = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
            if (!module) { fputs("FAIL: cannot map the 3.25 DLL\n", stderr); return 1; }
            const uintptr_t lookup_rva = 0xbeec20;
            unsigned char instruction[7];
            int32_t displacement = 0;
            BOOL ok = read_remote(GetCurrentProcess(), (uintptr_t)module + lookup_rva,
                                  instruction, sizeof(instruction)) &&
                      instruction[0] == 0x48 && instruction[1] == 0x8b && instruction[2] == 0x05;
            if (ok) memcpy(&displacement, instruction + 3, sizeof(displacement));
            uintptr_t actual_singleton = (uintptr_t)((int64_t)lookup_rva + 7 + displacement);
            FreeLibrary(module);
            if (!ok || actual_singleton != profiles[index].layout.engine_singleton) {
                fputs("FAIL: 3.25 DLL singleton address disagrees with its lookup instruction\n", stderr);
                return 1;
            }
        }
    }
    puts("PASS: all supplied DLL fingerprints select the matching architecture and layout");
    return 0;
}
