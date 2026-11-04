/* Reproduce the public 3.25 x64 entry conventions while keeping the test logic in C. */
__declspec(dllexport) __attribute__((naked)) void zego_express_get_audio_config(void) {
    __asm__(".byte 0x4c,0x8b,0xc2,0x33,0xd2\n\tjmp mock_get_by_channel");
}

__declspec(dllexport) __attribute__((naked)) void zego_express_set_audio_config(void) {
    __asm__(
        ".byte 0x48,0x83,0xec,0x38,0xf2,0x0f,0x10,0x02\n\t"
        "xorl %r8d,%r8d\n\t"
        "movl 8(%rdx),%eax\n\t"
        "leaq 0x20(%rsp),%rdx\n\t"
        "movsd %xmm0,0x20(%rsp)\n\t"
        "movl %eax,0x28(%rsp)\n\t"
        "call mock_set_by_channel\n\t"
        "addq $0x38,%rsp\n\t"
        "ret");
}
