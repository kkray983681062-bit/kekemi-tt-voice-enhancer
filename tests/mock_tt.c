#include <windows.h>
#include <stdio.h>

int main(void) {
    HMODULE engine = LoadLibraryW(L"ZegoExpressEngine.dll");
    FILE *ready = fopen("Z:\\tmp\\tt-bitrate-fixture-ready.log", "w");
    if (ready) {
        fprintf(ready, "%s\n", engine ? "ready" : "load_failed");
        fclose(ready);
    }
    if (!engine) return 1;
    Sleep(240000);
    return 0;
}
