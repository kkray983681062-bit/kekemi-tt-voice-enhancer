#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <stdint.h>
#include <string.h>
#include "telemetry.h"
#include "telemetry_core.h"

typedef struct {
    unsigned char sha256[32];
    TTLayout layout;
    uintptr_t constructor_rva;
    unsigned char constructor[7];
    uintptr_t quality_rva;
    unsigned char quality[8];
} EngineProfile;

/* Each profile is accepted only after both the full DLL hash and loaded code match. */
static const EngineProfile profiles[] = {
    {{0x7a,0xb8,0x98,0x26,0xd7,0x4a,0xcd,0x0f,0xc7,0x07,0x44,0xee,0x90,0xd7,0x1e,0x61,
      0xf2,0xd2,0x20,0xbd,0xa0,0x39,0x50,0x90,0x96,0xd9,0x9d,0xe9,0x9e,0x14,0x9a,0xf3},
     {0x20a4bd8,0x19117e0,0x18ce5f8,0x18ced08,TT_LAYOUT_MODERN64},
     0x8b54b1,{0x48,0x8d,0x05,0x50,0x98,0x01,0x01},0x8b6711,
     {0xf2,0x0f,0x10,0x83,0x00,0x0c,0x00,0x00}},
    {{0x08,0x7e,0x1a,0x9e,0xd2,0x95,0x49,0x23,0x4a,0xe8,0xe5,0xf7,0x87,0x17,0x49,0x3e,
      0x1f,0x29,0xef,0x2e,0x7a,0xf3,0x9a,0x15,0xa4,0x51,0xa4,0x18,0xb1,0xbc,0x02,0x03},
     {0x21165d8,0x1b82b48,0x1b3a5d8,0x1b3ace8,TT_LAYOUT_MODERN64},
     0x910851,{0x48,0x8d,0x05,0x90,0xa4,0x22,0x01},0x911ab1,
     {0xf2,0x0f,0x10,0x83,0x00,0x0c,0x00,0x00}},
    {{0x6c,0x01,0x30,0xda,0xa7,0x7f,0x6a,0x17,0x38,0x54,0x31,0x81,0x71,0x4c,0x54,0x1d,
      0x03,0x83,0x02,0xd8,0xed,0xf5,0x87,0xfe,0x00,0x5e,0x54,0x15,0x7d,0x70,0x3e,0xe1},
     {0x2116658,0x1b82b48,0x1b3a5d8,0x1b3ace8,TT_LAYOUT_MODERN64},
     0x910851,{0x48,0x8d,0x05,0x90,0xa4,0x22,0x01},0x911ab1,
     {0xf2,0x0f,0x10,0x83,0x00,0x0c,0x00,0x00}},
    {{0xe4,0x73,0x5a,0xeb,0xbe,0xab,0xec,0x25,0xbe,0xd9,0xe3,0x84,0xc1,0xcf,0x98,0x92,
      0x6e,0x6e,0xb1,0x26,0xc4,0x0b,0x7e,0x2d,0x17,0xf3,0xab,0xa8,0x28,0x95,0x90,0xe4},
     {0x2266ba8,0,0x1c81808,0x1c81e88,TT_LAYOUT_LEGACY64},
     0x9672b0,{0x48,0x8d,0x05,0xd1,0xab,0x31,0x01},0x96a306,
     {0xf2,0x0f,0x10,0x87,0x68,0x0b,0x00,0x00}},
    {{0xfd,0x65,0x9d,0x74,0x6d,0x26,0x95,0xae,0xa1,0x15,0x0d,0x47,0x73,0x1c,0xe4,0xc5,
      0xaf,0x68,0x46,0xce,0xa5,0xe4,0x04,0x7f,0x10,0xbe,0xbc,0x2e,0x9a,0xc8,0xdb,0xb0},
     {0x19da068,0,0x157e160,0x157e70c,TT_LAYOUT_LEGACY32},
     0x619b55,{0xc7,0x03,0,0,0,0,0},0x61bda8,
     {0xf2,0x0f,0x10,0x86,0x68,0x09,0x00,0x00}}
};

typedef struct {
    DWORD pid;
    uintptr_t base;
    FILETIME created;
    int profile;
    BOOL initialized;
} VerifiedEngine;
static VerifiedEngine g_verified;

static TTMetric metric(TTMetricCode code) {
    TTMetric result = {code, 0.0};
    return result;
}

static int file_profile(const wchar_t *path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BOOL ok = CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    unsigned char buffer[65536], digest[32];
    DWORD count = 0;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &count, NULL)) { ok = FALSE; break; }
        if (!count) break;
        if (!CryptHashData(hash, buffer, count, 0)) { ok = FALSE; break; }
    }
    DWORD length = sizeof(digest);
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    if (!ok) return -1;
    for (unsigned i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
        if (memcmp(digest, profiles[i].sha256, sizeof(digest)) == 0) return (int)i;
    return -1;
}

static int read_remote(void *context, uintptr_t address, void *out, size_t length) {
    SIZE_T got = 0;
    return ReadProcessMemory((HANDLE)context, (LPCVOID)address, out, length, &got) && got == length;
}

static BOOL loaded_code_matches(HANDLE process, uintptr_t base, const EngineProfile *profile) {
    unsigned char bytes[8];
    if (profile->layout.kind == TT_LAYOUT_LEGACY32) {
        uint32_t loaded_vtable;
        if (base > UINT32_MAX || profile->layout.stream_vtable > UINT32_MAX - base ||
            !read_remote(process, base + profile->constructor_rva, bytes, 6) ||
            memcmp(bytes, profile->constructor, 2) != 0) return FALSE;
        memcpy(&loaded_vtable, bytes + 2, sizeof(loaded_vtable));
        if ((uintptr_t)loaded_vtable != base + profile->layout.stream_vtable) return FALSE;
    } else if (!read_remote(process, base + profile->constructor_rva, bytes, sizeof(profile->constructor)) ||
               memcmp(bytes, profile->constructor, sizeof(profile->constructor)) != 0) {
        return FALSE;
    }
    return read_remote(process, base + profile->quality_rva, bytes, sizeof(profile->quality)) &&
           memcmp(bytes, profile->quality, sizeof(profile->quality)) == 0;
}

TTMetric tt_read_telemetry_for(TTEdition edition) {
    TTTarget target;
    DWORD error = 0;
    TTTargetCode code = tt_find_target(edition, &target, &error);
    if (code == TT_TARGET_NOT_RUNNING) return metric(TT_METRIC_NO_TT);
    if (code == TT_TARGET_NO_ENGINE) return metric(TT_METRIC_WAITING);
    if (code == TT_TARGET_MULTIPLE) return metric(TT_METRIC_MULTIPLE_TT);
    if (code != TT_TARGET_FOUND) return metric(TT_METRIC_ACCESS);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, target.pid);
    if (!process) return metric(TT_METRIC_ACCESS);
    if (!tt_same_process(process, &target)) { CloseHandle(process); return metric(TT_METRIC_WAITING); }
    if (!g_verified.initialized || g_verified.pid != target.pid || g_verified.base != target.engine_base ||
        CompareFileTime(&target.created, &g_verified.created) != 0) {
        g_verified.pid = target.pid; g_verified.base = target.engine_base; g_verified.created = target.created;
        g_verified.profile = file_profile(target.engine_path);
        g_verified.initialized = TRUE;
    }
    int index = g_verified.profile;
    if (index < 0 || target.engine_size < profiles[index].layout.engine_singleton +
                                         (profiles[index].layout.kind == TT_LAYOUT_LEGACY32 ? 4u : 8u) ||
        !loaded_code_matches(process, target.engine_base, &profiles[index])) {
        CloseHandle(process); return metric(TT_METRIC_UNSUPPORTED);
    }
    TTMetric result = metric(TT_METRIC_WAITING);
    if (tt_sample_with_layout(read_remote, process, target.engine_base, &profiles[index].layout,
                              &result.kbps) == TT_SAMPLE_ACTIVE)
        result.code = TT_METRIC_ACTIVE;
    CloseHandle(process);
    return result;
}

TTMetric tt_read_telemetry(void) { return tt_read_telemetry_for(TT_EDITION_GAME); }
