#define WIN32_LEAN_AND_MEAN
#include "controller.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincrypt.h>

typedef enum { ABI_UNKNOWN, ABI_MODERN64, ABI_LEGACY64, ABI_LEGACY32 } EngineABI;

typedef struct {
    HANDLE process;
    DWORD pid;
    uintptr_t engine_base;
    DWORD engine_size;
    uintptr_t get_audio_config;
    uintptr_t set_audio_config;
    EngineABI abi;
    wchar_t engine_path[MAX_PATH];
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
        case TT_CHANNEL_NOT_EFFECTIVE: return "CHANNEL_NOT_EFFECTIVE";
        case TT_RESTORE_FAILED: return "RESTORE_FAILED";
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
    memcpy(target->engine_path, found.engine_path, sizeof(target->engine_path));
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

/* These scalar layouts are restricted to fully fingerprinted game/entertainment DLLs.
   They describe capture and encoder settings, never PCM or conversation data. */
typedef struct {
    int mode, engine_mode, capture_channels, pending_channels, encoder_channels;
} TTChannelSnapshot;

typedef struct {
    unsigned char sha256[32];
    DWORD image_size;
    uintptr_t capture_rva, mode_write_rva, encoder_write_rva, audio_root;
    uintptr_t device_vtable, engine_vtable, encoder_vtable, sessions_offset;
} TTChannelLayout;

static const TTChannelLayout channel_layouts[] = {
    {{0x7a,0xb8,0x98,0x26,0xd7,0x4a,0xcd,0x0f,0xc7,0x07,0x44,0xee,0x90,0xd7,0x1e,0x61,
      0xf2,0xd2,0x20,0xbd,0xa0,0x39,0x50,0x90,0x96,0xd9,0x9d,0xe9,0x9e,0x14,0x9a,0xf3},
     0x2289000,0xb97b10,0xbead0f,0xc5d47e,0x20a4380,0x1947f60,0x1960750,0x1956760,0x226f08},
    {{0x08,0x7e,0x1a,0x9e,0xd2,0x95,0x49,0x23,0x4a,0xe8,0xe5,0xf7,0x87,0x17,0x49,0x3e,
      0x1f,0x29,0xef,0x2e,0x7a,0xf3,0x9a,0x15,0xa4,0x51,0xa4,0x18,0xb1,0xbc,0x02,0x03},
     0x336f000,0xc42920,0xc9c01f,0xd23bae,0x2115d80,0x1bbae10,0x1bd6260,0x1bcc300,0x2284e0}
};

static const TTChannelLayout *stereo_layout(const Target *target) {
    for (unsigned i = 0; i < sizeof(channel_layouts) / sizeof(channel_layouts[0]); ++i)
        if (target->engine_size == channel_layouts[i].image_size) return &channel_layouts[i];
    return NULL;
}

static BOOL stereo_profile(const Target *target) {
    const TTChannelLayout *layout = stereo_layout(target);
    if (!layout) return FALSE;
    HANDLE file = CreateFileW(target->engine_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    BOOL ok = CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    unsigned char buffer[65536], digest[32]; DWORD count;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &count, NULL)) { ok = FALSE; break; }
        if (!count) break;
        ok = CryptHashData(hash, buffer, count, 0);
    }
    count = sizeof(digest);
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &count, 0) &&
        count == sizeof(digest) && memcmp(digest, layout->sha256, sizeof(digest)) == 0;
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    if (!ok) return FALSE;
    static const unsigned char capture_code[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48};
    static const unsigned char state_code[] = {0x89,0x9f,0x84,0x07,0x00,0x00};
    static const unsigned char encoder_code[] = {0x89,0xb3,0x44,0x01,0x00,0x00};
    unsigned char loaded[12];
    return read_rva(target, (DWORD)layout->capture_rva, loaded, sizeof(capture_code)) &&
        memcmp(loaded, capture_code, sizeof(capture_code)) == 0 &&
        read_rva(target, (DWORD)layout->mode_write_rva, loaded, sizeof(state_code)) &&
        memcmp(loaded, state_code, sizeof(state_code)) == 0 &&
        read_rva(target, (DWORD)layout->encoder_write_rva, loaded, sizeof(encoder_code)) &&
        memcmp(loaded, encoder_code, sizeof(encoder_code)) == 0;
}

static BOOL channel_snapshot(const Target *target, TTChannelSnapshot *state) {
    const TTChannelLayout *layout = stereo_layout(target);
    uintptr_t audio, device, engine, sessions, encoder, codec, table, again;
    int configured_channels;
    if (!layout || !read_bytes(target->process, target->engine_base + layout->audio_root, &audio, 8) || !audio ||
        !read_bytes(target->process, audio + 0x60, &device, 8) || !device ||
        !read_bytes(target->process, device, &table, 8) || table != target->engine_base + layout->device_vtable ||
        !read_bytes(target->process, device + 0x748, &engine, 8) || !engine ||
        !read_bytes(target->process, engine, &table, 8) || table != target->engine_base + layout->engine_vtable ||
        !read_bytes(target->process, device + layout->sessions_offset, &sessions, 8) || !sessions ||
        !read_bytes(target->process, sessions + 0xd8, &encoder, 8) || !encoder ||
        !read_bytes(target->process, encoder, &table, 8) || table != target->engine_base + layout->encoder_vtable ||
        !read_bytes(target->process, encoder + 0x118, &codec, 8) || !codec ||
        !read_bytes(target->process, codec + 8, &table, 8) || table < target->engine_base ||
        table - target->engine_base >= target->engine_size ||
        !read_bytes(target->process, device + 0x784, &state->mode, 4) ||
        !read_bytes(target->process, engine + 0x2cc, &state->engine_mode, 4) ||
        !read_bytes(target->process, engine + 0x3474, &state->capture_channels, 4) ||
        !read_bytes(target->process, engine + 0x3d34, &state->pending_channels, 4) ||
        !read_bytes(target->process, encoder + 0x144, &configured_channels, 4) ||
        !read_bytes(target->process, codec + 0x64, &state->encoder_channels, 4) ||
        configured_channels != state->encoder_channels ||
        !read_bytes(target->process, target->engine_base + layout->audio_root, &again, 8) || again != audio ||
        !read_bytes(target->process, audio + 0x60, &again, 8) || again != device ||
        !read_bytes(target->process, device + 0x748, &again, 8) || again != engine ||
        !read_bytes(target->process, device + layout->sessions_offset, &again, 8) || again != sessions ||
        !read_bytes(target->process, sessions + 0xd8, &again, 8) || again != encoder ||
        !read_bytes(target->process, encoder + 0x118, &again, 8) || again != codec) return FALSE;
    return state->mode >= 0 && state->mode <= 2 && state->engine_mode >= 0 && state->engine_mode <= 2 &&
        state->capture_channels >= 1 && state->capture_channels <= 2 &&
        state->pending_channels >= 1 && state->pending_channels <= 2 &&
        state->encoder_channels >= 1 && state->encoder_channels <= 2;
}

static TTCode remote_capture_mode(const Target *target, RemoteCall *remote, int mode, DWORD *error) {
    const TTChannelLayout *layout = stereo_layout(target);
    if (!layout) return TT_ENGINE_UNSUPPORTED;
    unsigned char shim[] = {0xba,0,0,0,0,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    uint64_t function = target->engine_base + layout->capture_rva;
    memcpy(shim + 1, &mode, 4); memcpy(shim + 7, &function, 8);
    DWORD old; SIZE_T written;
    if (!VirtualProtectEx(target->process, (LPVOID)remote->code, 4096, PAGE_READWRITE, &old) ||
        !WriteProcessMemory(target->process, (LPVOID)(remote->code + 128), shim, sizeof(shim), &written) ||
        written != sizeof(shim) ||
        !FlushInstructionCache(target->process, (LPCVOID)(remote->code + 128), sizeof(shim))) {
        *error = GetLastError(); return TT_REMOTE_FAILED;
    }
    return remote_call(target, remote, remote->code + 128, FALSE, error);
}

static TTCode remote_config(const Target *target, RemoteCall *remote, const AudioConfig *config, DWORD *error) {
    SIZE_T written;
    if (!WriteProcessMemory(target->process, (LPVOID)remote->data, config, sizeof(*config), &written) ||
        written != sizeof(*config)) { *error = GetLastError(); return TT_REMOTE_FAILED; }
    return remote_call(target, remote, target->set_audio_config, TRUE, error);
}

static BOOL wait_channel_snapshot(const Target *target, const TTChannelSnapshot *expected, TTChannelSnapshot *observed) {
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        if (channel_snapshot(target, observed) && memcmp(expected, observed, sizeof(*expected)) == 0) return TRUE;
        Sleep(50);
    }
    return FALSE;
}

TTResult tt_apply_channels_for(TTEdition edition, int channels) {
    if (channels != 1 && channels != 2) return result_of(TT_OUT_OF_RANGE, 0);
    if (edition < TT_EDITION_GAME || edition >= TT_EDITION_COUNT) return result_of(TT_ENGINE_UNSUPPORTED, 0);
    Target target;
    TTResult result = find_target(edition, &target);
    if (result.code != TT_OK) return result;
    RemoteCall remote = {0};
    AudioConfig original, requested;
    TTChannelSnapshot before, after;
    if (!resolve_exports(&target) || target.abi != ABI_MODERN64 || !stereo_profile(&target)) {
        result.code = TT_ENGINE_UNSUPPORTED; goto done_channels;
    }
    if (!channel_snapshot(&target, &before)) { result.code = TT_ENGINE_NOT_READY; goto done_channels; }
    result.code = remote_prepare(&target, &remote, &result.system_error);
    if (result.code != TT_OK) goto done_channels;
    result.code = remote_get(&target, &remote, &original, &result.system_error);
    if (result.code != TT_OK) goto done_channels;
    result.config = original;
    if (original.bitrate > TT_MAX_KBPS) { result.code = TT_OUT_OF_RANGE; goto done_channels; }
    requested = original; requested.channel = channels;
    result.code = remote_capture_mode(&target, &remote, channels == 2 ? 1 : 0, &result.system_error);
    if (result.code == TT_REMOTE_TIMEOUT) goto done_channels;
    if (result.code == TT_OK) result.code = remote_config(&target, &remote, &requested, &result.system_error);
    if (result.code == TT_REMOTE_TIMEOUT) goto done_channels;
    if (result.code == TT_OK) result.code = remote_get(&target, &remote, &result.config, &result.system_error);
    if (result.code == TT_REMOTE_TIMEOUT) goto done_channels;
    if (result.code == TT_OK && memcmp(&requested, &result.config, sizeof(requested))) result.code = TT_READBACK_MISMATCH;
    if (result.code == TT_OK) {
        TTChannelSnapshot expected = {channels == 2 ? 1 : 0, channels == 2 ? 1 : 0, channels, channels, channels};
        if (wait_channel_snapshot(&target, &expected, &after)) {
            result.capture_channels = after.capture_channels;
            result.encoder_channels = after.encoder_channels;
            goto done_channels;
        }
        result.code = TT_CHANNEL_NOT_EFFECTIVE;
    }
    /* Roll back both settings, including an independently chosen original capture mode. */
    {
        TTCode primary = result.code;
        DWORD primary_error = result.system_error, error = 0;
        TTCode restore_config = remote_config(&target, &remote, &original, &error);
        if (restore_config == TT_REMOTE_TIMEOUT) { result.code = TT_REMOTE_TIMEOUT; result.system_error = error; goto done_channels; }
        TTCode restore_capture = remote_capture_mode(&target, &remote, before.mode, &error);
        if (restore_capture == TT_REMOTE_TIMEOUT) { result.code = TT_REMOTE_TIMEOUT; result.system_error = error; goto done_channels; }
        TTCode readback = remote_get(&target, &remote, &result.config, &error);
        if (readback == TT_REMOTE_TIMEOUT) { result.code = TT_REMOTE_TIMEOUT; result.system_error = error; goto done_channels; }
        if (restore_config != TT_OK || restore_capture != TT_OK || readback != TT_OK ||
            memcmp(&original, &result.config, sizeof(original)) || !wait_channel_snapshot(&target, &before, &after)) {
            result.code = TT_RESTORE_FAILED; result.system_error = error;
        } else { result.code = primary; result.system_error = primary_error; }
    }
done_channels:
    remote_free(&target, &remote);
    CloseHandle(target.process);
    return result;
}
