#include <stdio.h>
#include "../src/telemetry.c"

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    int index = file_profile(argv[1]);
    if (index < 0) { puts("FAIL: file fingerprint not recognized"); return 1; }
    HMODULE module = LoadLibraryExW(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) { printf("FAIL: unable to map image (%lu)\n", (unsigned long)GetLastError()); return 1; }
    BOOL matches = loaded_code_matches(GetCurrentProcess(), (uintptr_t)module, &profiles[index]);
    FreeLibrary(module);
    if (!matches) { puts("FAIL: mapped image does not match its verified code markers"); return 1; }
    puts("PASS: real DLL fingerprint and relocated code markers match");
    return 0;
}
