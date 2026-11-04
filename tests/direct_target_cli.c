/* Wine cannot enumerate a 32-bit process from a 64-bit process. This fixture
   supplies the mock process's own module address and exercises the real remote calls. */
#include "../src/controller.c"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 5 || argc > 6) return 2;
    Target target;
    memset(&target, 0, sizeof(target));
    target.pid = strtoul(argv[1], NULL, 10);
    target.engine_base = (uintptr_t)_strtoui64(argv[2], NULL, 16);
    target.engine_size = strtoul(argv[3], NULL, 10);
    const DWORD rights = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                         PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE;
    target.process = OpenProcess(rights, FALSE, target.pid);
    if (!target.process || !resolve_exports(&target)) { puts("INTERFACE_UNSUPPORTED"); return 1; }
    DWORD error = 0;
    RemoteCall remote;
    TTCode code = remote_prepare(&target, &remote, &error);
    AudioConfig config;
    if (code == TT_OK) code = remote_get(&target, &remote, &config, &error);
    BOOL set = strcmp(argv[4], "--set") == 0;
    if (code == TT_OK && set) {
        if (argc != 6) return 2;
        config.bitrate = atoi(argv[5]);
        SIZE_T written = 0;
        if (!WriteProcessMemory(target.process, (LPVOID)remote.data, &config, sizeof(config), &written) ||
            written != sizeof(config)) code = TT_REMOTE_FAILED;
        else code = remote_call(&target, &remote, target.set_audio_config, TRUE, &error);
        if (code == TT_OK) code = remote_get(&target, &remote, &config, &error);
    }
    if (code == TT_OK)
        printf("%s %d %d %d\n", set ? "APPLIED" : "READ", config.bitrate, config.channel, config.codec);
    else printf("%s %lu\n", tt_code_name(code), (unsigned long)error);
    remote_free(&target, &remote);
    CloseHandle(target.process);
    return code == TT_OK ? 0 : 1;
}
