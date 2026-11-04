#include <windows.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    HMODULE engine = LoadLibraryW(L"ZegoExpressEngine.dll");
    FILE *ready = fopen("Z:\\tmp\\tt-bitrate-fixture-ready.log", "w");
    if (ready) {
        if (engine) {
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)engine;
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((unsigned char *)engine + dos->e_lfanew);
            fprintf(ready, "ready %lu %llx %lu\n", (unsigned long)GetCurrentProcessId(),
                    (unsigned long long)(uintptr_t)engine, (unsigned long)nt->OptionalHeader.SizeOfImage);
        } else {
            fputs("load_failed\n", ready);
        }
        fclose(ready);
    }
    if (!engine) return 1;
    Sleep(240000);
    return 0;
}
