#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char *ready_path = getenv("TT_DUAL_READY");
    const char *helper = getenv("TT_DUAL_HELPER");
    HMODULE engine = helper ? NULL : LoadLibraryW(L"ZegoExpressEngine.dll");
    if (ready_path) {
        FILE *ready = fopen(ready_path, "w");
        if (ready) { fprintf(ready, "%s\n", helper || engine ? "ready" : "load_failed"); fclose(ready); }
    }
    if (!helper && !engine) return 1;
    Sleep(240000);
    return 0;
}
