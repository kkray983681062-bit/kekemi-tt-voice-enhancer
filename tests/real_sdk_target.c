#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef int (__cdecl *InitEngine)(void *, unsigned int, const char *, bool, int);

int main(void) {
    HMODULE library = LoadLibraryW(L"ZegoExpressEngine.dll");
    int init_result = -9999;
    if (library) {
        FARPROC function = GetProcAddress(library, "zego_express_engine_init");
        if (function) {
            InitEngine init;
            memcpy(&init, &function, sizeof(init));
            init_result = init(NULL, 1,
                "0000000000000000000000000000000000000000000000000000000000000000",
                true, 0);
        }
    }
    FILE *file = fopen("Z:\\tmp\\tt-real-sdk-ready.log", "w");
    if (file) {
        fprintf(file, "loaded=%d init=%d\n", library != NULL, init_result);
        fclose(file);
    }
    if (!library) return 1;
    Sleep(240000);
    return 0;
}
