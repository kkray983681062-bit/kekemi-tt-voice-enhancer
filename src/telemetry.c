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
} EngineProfile;

/* Each profile is accepted only after both the full DLL hash and loaded code match. */
static const EngineProfile profiles[] = {
    {{0x7a,0xb8,0x98,0x26,0xd7,0x4a,0xcd,0x0f,0xc7,0x07,0x44,0xee,0x90,0xd7,0x1e,0x61,
      0xf2,0xd2,0x20,0xbd,0xa0,0x39,0x50,0x90,0x96,0xd9,0x9d,0xe9,0x9e,0x14,0x9a,0xf3},
     {0x20a4bd8,0x19117e0,0x18ce5f8,0x18ced08},
     0x8b54b1,{0x48,0x8d,0x05,0x50,0x98,0x01,0x01},0x8b6711},
    {{0x08,0x7e,0x1a,0x9e,0xd2,0x95,0x49,0x23,0x4a,0xe8,0xe5,0xf7,0x87,0x17,0x49,0x3e,
      0x1f,0x29,0xef,0x2e,0x7a,0xf3,0x9a,0x15,0xa4,0x51,0xa4,0x18,0xb1,0xbc,0x02,0x03},
     {0x21165d8,0x1b82b48,0x1b3a5d8,0x1b3ace8},
     0x910851,{0x48,0x8d,0x05,0x90,0xa4,0x22,0x01},0x911ab1}
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
    static const unsigned char audio_quality[] = {0xf2,0x0f,0x10,0x83,0x00,0x0c,0x00,0x00};
    unsigned char bytes[sizeof(audio_quality)];
    return read_remote(process, base + profile->constructor_rva, bytes, sizeof(profile->constructor)) &&
           memcmp(bytes, profile->constructor, sizeof(profile->constructor)) == 0 &&
           read_remote(process, base + profile->quality_rva, bytes, sizeof(audio_quality)) &&
           memcmp(bytes, audio_quality, sizeof(audio_quality)) == 0;
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
    if (index < 0 || target.engine_size < profiles[index].layout.engine_singleton + sizeof(uintptr_t) ||
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
