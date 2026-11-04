#include "../src/controller.c"
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv) {
    if (argc != 3) return 2;
    HMODULE module = LoadLibraryExW(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) { printf("FAIL: unable to map image (%lu)\n", (unsigned long)GetLastError()); return 1; }
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((unsigned char *)module + dos->e_lfanew);
    Target target;
    memset(&target, 0, sizeof(target));
    target.process = GetCurrentProcess();
    target.engine_base = (uintptr_t)module;
    target.engine_size = nt->OptionalHeader.SizeOfImage;
    EngineABI expected = wcscmp(argv[2], L"modern64") == 0 ? ABI_MODERN64 :
                         wcscmp(argv[2], L"legacy64") == 0 ? ABI_LEGACY64 : ABI_LEGACY32;
    BOOL ok = resolve_exports(&target) && target.abi == expected;
    FreeLibrary(module);
    if (!ok) { puts("FAIL: real DLL export ABI not recognized"); return 1; }
    puts("PASS: real DLL export ABI recognized");
    return 0;
}
