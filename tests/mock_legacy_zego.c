#include <windows.h>

typedef struct { int bitrate, channel, codec; } AudioConfig;
static AudioConfig current = {64, 1, 3};

int legacy_get_by_channel(int channel, AudioConfig *out) {
    if (channel != 0 || !out) return 10010;
    *out = current;
    return 0;
}

int legacy_set_by_channel(AudioConfig config, int channel) {
    if (channel != 0) return 10010;
    current = config;
    return 0;
}

/* The wrappers use the actual 3.21 export calling conventions and initial instructions. */
#if defined(__x86_64__)
__declspec(dllexport) __attribute__((naked)) void zego_express_get_audio_config(void) {
    __asm__(".byte 0x48,0x8b,0xd1,0x33,0xc9\n\tjmp legacy_get_by_channel");
}

__declspec(dllexport) __attribute__((naked)) void zego_express_set_audio_config(void) {
    __asm__(
        ".byte 0x48,0x83,0xec,0x38,0xf2,0x0f,0x10,0x01\n\t"
        "xorl %edx,%edx\n\t"
        "movl 8(%rcx),%eax\n\t"
        "leaq 0x20(%rsp),%rcx\n\t"
        "movsd %xmm0,0x20(%rsp)\n\t"
        "movl %eax,0x28(%rsp)\n\t"
        "call legacy_set_by_channel\n\t"
        "addq $0x38,%rsp\n\t"
        "ret");
}
#else
__declspec(dllexport) __attribute__((naked)) void zego_express_get_audio_config(void) {
    __asm__(
        ".byte 0x55,0x8b,0xec,0xff,0x75,0x08,0x6a,0x00\n\t"
        "call _legacy_get_by_channel\n\t"
        "addl $8,%esp\n\t"
        "popl %ebp\n\t"
        "ret");
}

__declspec(dllexport) __attribute__((naked)) void zego_express_set_audio_config(void) {
    __asm__(
        ".byte 0x55,0x8b,0xec,0xf3,0x0f,0x7e,0x45,0x08,0x8b,0x45,0x10,0x6a,0x00\n\t"
        "subl $12,%esp\n\t"
        "movl %esp,%ecx\n\t"
        "movq %xmm0,(%ecx)\n\t"
        "movl %eax,8(%ecx)\n\t"
        "call _legacy_set_by_channel\n\t"
        "addl $16,%esp\n\t"
        "popl %ebp\n\t"
        "ret");
}
#endif

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance; (void)reason; (void)reserved;
    return TRUE;
}
