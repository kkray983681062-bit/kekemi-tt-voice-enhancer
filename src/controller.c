#define WIN32_LEAN_AND_MEAN
#include "controller.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    HANDLE process;
    DWORD pid;
    uintptr_t engine_base;
    DWORD engine_size;
    uintptr_t get_audio_config;
    uintptr_t set_audio_config;
} Target;

typedef struct {
    uintptr_t data;
    uintptr_t code;
    BOOL retain_remote_memory;
} RemoteCall;

static TTResult result_of(TTCode code, DWORD error) {
    TTResult result;
    memset(&result, 0, sizeof(result));
    result.code = code;
    result.system_error = error;
    return result;
}

const char *tt_code_name(TTCode code) {
    switch (code) {
        case TT_OK: return "OK";
        case TT_OUT_OF_RANGE: return "OUT_OF_RANGE";
        case TT_NO_TARGET: return "NO_TARGET";
        case TT_MULTIPLE_TARGETS: return "MULTIPLE_TARGETS";
        case TT_NO_ENGINE: return "NO_ENGINE";
        case TT_ACCESS_DENIED: return "ACCESS_DENIED";
        case TT_ENGINE_UNSUPPORTED: return "ENGINE_UNSUPPORTED";
        case TT_ENGINE_NOT_READY: return "ENGINE_NOT_READY";
        case TT_SDK_ERROR: return "SDK_ERROR";
        case TT_REMOTE_FAILED: return "REMOTE_FAILED";
        case TT_REMOTE_TIMEOUT: return "REMOTE_TIMEOUT";
        case TT_READBACK_MISMATCH: return "READBACK_MISMATCH";
        default: return "SYSTEM_ERROR";
    }
}

static TTResult find_target(TTEdition edition, Target *target) {
    TTTarget found;
    DWORD error = 0;
    TTTargetCode code = tt_find_target(edition, &found, &error);
    if (code == TT_TARGET_NOT_RUNNING) return result_of(TT_NO_TARGET, error);
    if (code == TT_TARGET_NO_ENGINE) return result_of(TT_NO_ENGINE, error);
    if (code == TT_TARGET_MULTIPLE) return result_of(TT_MULTIPLE_TARGETS, error);
    if (code == TT_TARGET_ACCESS) return result_of(TT_ACCESS_DENIED, error);
    if (code != TT_TARGET_FOUND) return result_of(TT_SYSTEM_ERROR, error);
    const DWORD rights = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                         PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE;
    HANDLE process = OpenProcess(rights, FALSE, found.pid);
    if (!process) {
        error = GetLastError();
        return result_of(error == ERROR_ACCESS_DENIED ? TT_ACCESS_DENIED : TT_SYSTEM_ERROR, error);
    }
    if (!tt_same_process(process, &found)) {
        CloseHandle(process);
        return result_of(TT_NO_TARGET, 0);
    }
    memset(target, 0, sizeof(*target));
    target->process = process;
    target->pid = found.pid;
    target->engine_base = found.engine_base;
    target->engine_size = found.engine_size;
    TTResult result = result_of(TT_OK, 0);
    result.pid = found.pid;
    return result;
}

static BOOL read_bytes(HANDLE process, uintptr_t address, void *out, SIZE_T length) {
    SIZE_T read_count = 0;
    return ReadProcessMemory(process, (LPCVOID)address, out, length, &read_count) &&
           read_count == length;
}

static BOOL read_rva(const Target *target, DWORD rva, void *out, SIZE_T length) {
    if ((uint64_t)rva + length > target->engine_size) return FALSE;
    return read_bytes(target->process, target->engine_base + rva, out, length);
}

static BOOL resolve_exports(Target *target) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_EXPORT_DIRECTORY exports;
    if (!read_rva(target, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0 || (DWORD)dos.e_lfanew > 0x100000 ||
        !read_rva(target, (DWORD)dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return FALSE;

    IMAGE_DATA_DIRECTORY directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || directory.Size < sizeof(exports) ||
        !read_rva(target, directory.VirtualAddress, &exports, sizeof(exports)) ||
        !exports.NumberOfNames || exports.NumberOfNames > 100000 ||
        !exports.NumberOfFunctions || exports.NumberOfFunctions > 100000) return FALSE;

    DWORD *names = (DWORD *)calloc(exports.NumberOfNames, sizeof(DWORD));
    WORD *ordinals = (WORD *)calloc(exports.NumberOfNames, sizeof(WORD));
    DWORD *functions = (DWORD *)calloc(exports.NumberOfFunctions, sizeof(DWORD));
    BOOL valid = names && ordinals && functions;
    if (valid) {
        valid = read_rva(target, exports.AddressOfNames, names, exports.NumberOfNames * sizeof(DWORD)) &&
                read_rva(target, exports.AddressOfNameOrdinals, ordinals, exports.NumberOfNames * sizeof(WORD)) &&
                read_rva(target, exports.AddressOfFunctions, functions, exports.NumberOfFunctions * sizeof(DWORD));
    }
    DWORD get_rva = 0, set_rva = 0;
    for (DWORD i = 0; valid && i < exports.NumberOfNames && (!get_rva || !set_rva); ++i) {
        if (names[i] >= target->engine_size || ordinals[i] >= exports.NumberOfFunctions) {
            valid = FALSE;
            break;
        }
        char name[128];
        SIZE_T available = target->engine_size - names[i];
        SIZE_T length = available < sizeof(name) ? available : sizeof(name);
        if (!read_rva(target, names[i], name, length) || !memchr(name, '\0', length)) {
            valid = FALSE;
            break;
        }
        if (strcmp(name, "zego_express_get_audio_config") == 0) get_rva = functions[ordinals[i]];
        if (strcmp(name, "zego_express_set_audio_config") == 0) set_rva = functions[ordinals[i]];
    }
    free(names);
    free(ordinals);
    free(functions);
    if (!valid || !get_rva || !set_rva ||
        get_rva >= target->engine_size || set_rva >= target->engine_size ||
        ((uint64_t)get_rva >= directory.VirtualAddress &&
         (uint64_t)get_rva < (uint64_t)directory.VirtualAddress + directory.Size) ||
        ((uint64_t)set_rva >= directory.VirtualAddress &&
         (uint64_t)set_rva < (uint64_t)directory.VirtualAddress + directory.Size)) return FALSE;
    target->get_audio_config = target->engine_base + get_rva;
    target->set_audio_config = target->engine_base + set_rva;
    return TRUE;
}

static void remote_free(const Target *target, RemoteCall *remote) {
    if (remote->retain_remote_memory) return;
    if (remote->code) VirtualFreeEx(target->process, (LPVOID)remote->code, 0, MEM_RELEASE);
    if (remote->data) VirtualFreeEx(target->process, (LPVOID)remote->data, 0, MEM_RELEASE);
    remote->code = remote->data = 0;
}

static TTCode remote_prepare(const Target *target, RemoteCall *remote, DWORD *system_error) {
    memset(remote, 0, sizeof(*remote));
    remote->data = (uintptr_t)VirtualAllocEx(target->process, NULL, 4096,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    remote->code = (uintptr_t)VirtualAllocEx(target->process, NULL, 4096,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote->data || !remote->code) {
        *system_error = GetLastError();
        remote_free(target, remote);
        return TT_REMOTE_FAILED;
    }
    return TT_OK;
}

static TTCode remote_call(const Target *target, RemoteCall *remote, uintptr_t function,
                          DWORD *system_error) {
    unsigned char stub[] = {
        0x48, 0x83, 0xec, 0x28,             /* stack alignment and shadow space */
        0x48, 0x31, 0xc9,                   /* engine index 0 */
        0x48, 0xba, 0,0,0,0,0,0,0,0,       /* rdx = configuration */
        0x48, 0xb8, 0,0,0,0,0,0,0,0,       /* rax = SDK export */
        0xff, 0xd0,                         /* call rax */
        0x48, 0x83, 0xc4, 0x28, 0xc3
    };
    memcpy(stub + 9, &remote->data, sizeof(uint64_t));
    memcpy(stub + 19, &function, sizeof(uint64_t));
    DWORD old_protect = 0;
    if (!VirtualProtectEx(target->process, (LPVOID)remote->code, 4096, PAGE_READWRITE, &old_protect)) {
        *system_error = GetLastError();
        return TT_REMOTE_FAILED;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(target->process, (LPVOID)remote->code, stub, sizeof(stub), &written) ||
        written != sizeof(stub) ||
        !VirtualProtectEx(target->process, (LPVOID)remote->code, 4096, PAGE_EXECUTE_READ, &old_protect) ||
        !FlushInstructionCache(target->process, (LPCVOID)remote->code, sizeof(stub))) {
        *system_error = GetLastError();
        return TT_REMOTE_FAILED;
    }
    HANDLE thread = CreateRemoteThread(target->process, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)remote->code, NULL, 0, NULL);
    if (!thread) {
        *system_error = GetLastError();
        return TT_REMOTE_FAILED;
    }
    DWORD wait = WaitForSingleObject(thread, 5000);
    DWORD exit_code = 0;
    if (wait != WAIT_OBJECT_0) {
        *system_error = wait == WAIT_FAILED ? GetLastError() : 0;
        remote->retain_remote_memory = TRUE;
        CloseHandle(thread);
        return wait == WAIT_TIMEOUT ? TT_REMOTE_TIMEOUT : TT_REMOTE_FAILED;
    }
    BOOL got_exit = GetExitCodeThread(thread, &exit_code);
    if (!got_exit) *system_error = GetLastError();
    CloseHandle(thread);
    if (!got_exit) return TT_REMOTE_FAILED;
    if (exit_code != 0) {
        *system_error = exit_code;
        if ((exit_code & 0xf0000000u) != 0xc0000000u) return TT_SDK_ERROR;
        return TT_REMOTE_FAILED;
    }
    return TT_OK;
}

static TTCode remote_get(const Target *target, RemoteCall *remote, AudioConfig *config,
                         DWORD *system_error) {
    AudioConfig empty = {0};
    SIZE_T count = 0;
    if (!WriteProcessMemory(target->process, (LPVOID)remote->data, &empty, sizeof(empty), &count) ||
        count != sizeof(empty)) {
        *system_error = GetLastError();
        return TT_REMOTE_FAILED;
    }
    TTCode code = remote_call(target, remote, target->get_audio_config, system_error);
    if (code != TT_OK) return code;
    if (!read_bytes(target->process, remote->data, config, sizeof(*config))) {
        *system_error = GetLastError();
        return TT_REMOTE_FAILED;
    }
    /* Reading a pre-existing configuration is separate from the allowed input range. */
    if (config->bitrate < TT_MIN_KBPS || config->bitrate > 320 ||
        (config->channel != 1 && config->channel != 2)) return TT_ENGINE_NOT_READY;
    return TT_OK;
}

static TTResult perform(TTEdition edition, BOOL set, int bitrate) {
    if (set && (bitrate < TT_MIN_KBPS || bitrate > TT_MAX_KBPS))
        return result_of(TT_OUT_OF_RANGE, 0);
    Target target;
    TTResult result = find_target(edition, &target);
    if (result.code != TT_OK) return result;
    if (!resolve_exports(&target)) {
        result.code = TT_ENGINE_UNSUPPORTED;
        CloseHandle(target.process);
        return result;
    }
    RemoteCall remote;
    result.code = remote_prepare(&target, &remote, &result.system_error);
    if (result.code == TT_OK) {
        result.code = remote_get(&target, &remote, &result.config, &result.system_error);
        if (result.code == TT_OK && set && result.config.bitrate != bitrate) {
            AudioConfig requested = result.config;
            requested.bitrate = bitrate;
            SIZE_T written = 0;
            if (!WriteProcessMemory(target.process, (LPVOID)remote.data, &requested,
                                    sizeof(requested), &written) || written != sizeof(requested)) {
                result.code = TT_REMOTE_FAILED;
                result.system_error = GetLastError();
            } else {
                result.code = remote_call(&target, &remote, target.set_audio_config, &result.system_error);
                if (result.code == TT_OK) {
                    result.code = remote_get(&target, &remote, &result.config, &result.system_error);
                    if (result.code == TT_OK && result.config.bitrate != bitrate)
                        result.code = TT_READBACK_MISMATCH;
                }
            }
        }
        remote_free(&target, &remote);
    }
    CloseHandle(target.process);
    return result;
}

TTResult tt_read_config_for(TTEdition edition) { return perform(edition, FALSE, 0); }
TTResult tt_apply_bitrate_for(TTEdition edition, int bitrate) { return perform(edition, TRUE, bitrate); }
TTResult tt_read_config(void) { return tt_read_config_for(TT_EDITION_GAME); }
TTResult tt_apply_bitrate(int bitrate) { return tt_apply_bitrate_for(TT_EDITION_GAME, bitrate); }
