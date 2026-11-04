#define WIN32_LEAN_AND_MEAN
#include "controller.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef enum { ABI_UNKNOWN, ABI_MODERN64, ABI_LEGACY64, ABI_LEGACY32 } EngineABI;

typedef struct {
    HANDLE process;
    DWORD pid;
    uintptr_t engine_base;
    DWORD engine_size;
    uintptr_t get_audio_config;
    uintptr_t set_audio_config;
    EngineABI abi;
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
    IMAGE_NT_HEADERS64 nt64;
    IMAGE_EXPORT_DIRECTORY exports;
    if (!read_rva(target, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0 || (DWORD)dos.e_lfanew > 0x100000 ||
        !read_rva(target, (DWORD)dos.e_lfanew, &nt64, sizeof(nt64)) ||
        nt64.Signature != IMAGE_NT_SIGNATURE) return FALSE;

    IMAGE_DATA_DIRECTORY directory;
    WORD machine = nt64.FileHeader.Machine;
    if (machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt64.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        nt64.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
        directory = nt64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else if (machine == IMAGE_FILE_MACHINE_I386) {
        IMAGE_NT_HEADERS32 nt32;
        if (!read_rva(target, (DWORD)dos.e_lfanew, &nt32, sizeof(nt32)) ||
            nt32.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt32.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return FALSE;
        directory = nt32.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        return FALSE;
    }

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

    /* SDK 3.21 and 3.25 export the same names with different calling conventions. */
    static const unsigned char get_modern64[] = {0x4c,0x8b,0xc2,0x33,0xd2};
    static const unsigned char get_legacy64[] = {0x48,0x8b,0xd1,0x33,0xc9};
    static const unsigned char set_modern64[] = {0x48,0x83,0xec,0x38,0xf2,0x0f,0x10,0x02};
    static const unsigned char set_legacy64[] = {0x48,0x83,0xec,0x38,0xf2,0x0f,0x10,0x01};
    static const unsigned char get_legacy32[] = {0x55,0x8b,0xec,0xff,0x75,0x08,0x6a,0x00};
    static const unsigned char set_legacy32[] = {0x55,0x8b,0xec,0xf3,0x0f,0x7e,0x45,0x08,0x8b,0x45,0x10};
    unsigned char get_code[12], set_code[12];
    if (!read_rva(target, get_rva, get_code, sizeof(get_code)) ||
        !read_rva(target, set_rva, set_code, sizeof(set_code))) return FALSE;
    target->abi = ABI_UNKNOWN;
    if (machine == IMAGE_FILE_MACHINE_AMD64 && (get_code[5] == 0xe9 || get_code[5] == 0xeb)) {
        if (memcmp(get_code, get_modern64, sizeof(get_modern64)) == 0 &&
            memcmp(set_code, set_modern64, sizeof(set_modern64)) == 0)
            target->abi = ABI_MODERN64;
        else if (memcmp(get_code, get_legacy64, sizeof(get_legacy64)) == 0 &&
                 memcmp(set_code, set_legacy64, sizeof(set_legacy64)) == 0)
            target->abi = ABI_LEGACY64;
    } else if (machine == IMAGE_FILE_MACHINE_I386 &&
               memcmp(get_code, get_legacy32, sizeof(get_legacy32)) == 0 &&
               memcmp(set_code, set_legacy32, sizeof(set_legacy32)) == 0) {
        target->abi = ABI_LEGACY32;
    }
    if (target->abi == ABI_UNKNOWN) return FALSE;
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
                          BOOL set, DWORD *system_error) {
    unsigned char stub[40];
    SIZE_T stub_length = 0;
    uint64_t data64 = (uint64_t)remote->data, function64 = (uint64_t)function;
    if (target->abi == ABI_MODERN64) {
        const unsigned char code[] = {
            0x48,0x83,0xec,0x28, 0x48,0x31,0xc9,
            0x48,0xba,0,0,0,0,0,0,0,0, 0x48,0xb8,0,0,0,0,0,0,0,0,
            0xff,0xd0, 0x48,0x83,0xc4,0x28,0xc3
        };
        stub_length = sizeof(code); memcpy(stub, code, stub_length);
        memcpy(stub + 9, &data64, sizeof(data64));
        memcpy(stub + 19, &function64, sizeof(function64));
    } else if (target->abi == ABI_LEGACY64) {
        const unsigned char code[] = {
            0x48,0x83,0xec,0x28, 0x48,0xb9,0,0,0,0,0,0,0,0,
            0x48,0xb8,0,0,0,0,0,0,0,0, 0xff,0xd0, 0x48,0x83,0xc4,0x28,0xc3
        };
        stub_length = sizeof(code); memcpy(stub, code, stub_length);
        memcpy(stub + 6, &data64, sizeof(data64));
        memcpy(stub + 16, &function64, sizeof(function64));
    } else if (target->abi == ABI_LEGACY32 && remote->data <= UINT32_MAX &&
               remote->code <= UINT32_MAX && function <= UINT32_MAX) {
        uint32_t data32 = (uint32_t)remote->data, function32 = (uint32_t)function;
        if (set) {
            /* The 32-bit setter receives the 12-byte configuration by value. */
            const unsigned char code[] = {
                0xb8,0,0,0,0, 0xff,0x70,0x08, 0xff,0x70,0x04, 0xff,0x30,
                0xb8,0,0,0,0, 0xff,0xd0, 0x83,0xc4,0x0c, 0xc2,0x04,0x00
            };
            stub_length = sizeof(code); memcpy(stub, code, stub_length);
            memcpy(stub + 1, &data32, sizeof(data32));
            memcpy(stub + 14, &function32, sizeof(function32));
        } else {
            const unsigned char code[] = {
                0x68,0,0,0,0, 0xb8,0,0,0,0, 0xff,0xd0, 0x83,0xc4,0x04, 0xc2,0x04,0x00
            };
            stub_length = sizeof(code); memcpy(stub, code, stub_length);
            memcpy(stub + 1, &data32, sizeof(data32));
            memcpy(stub + 6, &function32, sizeof(function32));
        }
    } else {
        *system_error = ERROR_INVALID_ADDRESS;
        return TT_REMOTE_FAILED;
    }
    DWORD old_protect = 0;
    if (!VirtualProtectEx(target->process, (LPVOID)remote->code, 4096, PAGE_READWRITE, &old_protect)) {
        *system_error = GetLastError();
        return TT_REMOTE_FAILED;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(target->process, (LPVOID)remote->code, stub, stub_length, &written) ||
        written != stub_length ||
        !VirtualProtectEx(target->process, (LPVOID)remote->code, 4096, PAGE_EXECUTE_READ, &old_protect) ||
        !FlushInstructionCache(target->process, (LPCVOID)remote->code, stub_length)) {
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
    TTCode code = remote_call(target, remote, target->get_audio_config, FALSE, system_error);
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
                result.code = remote_call(&target, &remote, target.set_audio_config, TRUE, &result.system_error);
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
