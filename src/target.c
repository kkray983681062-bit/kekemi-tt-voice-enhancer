#define WIN32_LEAN_AND_MEAN
#include "target.h"
#include <tlhelp32.h>
#include <string.h>
#include <wchar.h>

const wchar_t *tt_edition_executable(TTEdition edition) {
#ifdef TT_FAKE_TARGET
    return edition == TT_EDITION_ENTERTAINMENT ? L"TTEntertainmentTest.exe" : L"TTVoiceTest.exe";
#else
    return edition == TT_EDITION_ENTERTAINMENT ? L"TT语音娱乐版.exe" : L"TTVoice.exe";
#endif
}

static HANDLE module_snapshot(DWORD pid) {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 4; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(30);
    }
    return snapshot;
}

BOOL tt_same_process(HANDLE process, const TTTarget *target) {
    FILETIME created, exited, kernel, user;
    return process && target && GetProcessTimes(process, &created, &exited, &kernel, &user) &&
           CompareFileTime(&created, &target->created) == 0;
}

TTTargetCode tt_find_target(TTEdition edition, TTTarget *target, DWORD *system_error) {
    if (!target || edition < TT_EDITION_GAME || edition >= TT_EDITION_COUNT) return TT_TARGET_SYSTEM;
    memset(target, 0, sizeof(*target));
    if (system_error) *system_error = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (system_error) *system_error = GetLastError();
        return TT_TARGET_SYSTEM;
    }
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry)); entry.dwSize = sizeof(entry);
    unsigned processes = 0, engines = 0;
    BOOL inaccessible = FALSE;
    DWORD last_error = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, tt_edition_executable(edition)) != 0) continue;
        ++processes;
        HANDLE modules = module_snapshot(entry.th32ProcessID);
        if (modules == INVALID_HANDLE_VALUE) {
            last_error = GetLastError();
            /* A process that already exited cannot be the selected audio engine. */
            if (last_error != ERROR_INVALID_PARAMETER && last_error != ERROR_NOT_FOUND)
                inaccessible = TRUE;
            continue;
        }
        MODULEENTRY32W module;
        memset(&module, 0, sizeof(module)); module.dwSize = sizeof(module);
        BOOL found = FALSE;
        if (Module32FirstW(modules, &module)) do {
            if (_wcsicmp(module.szModule, L"ZegoExpressEngine.dll") == 0) { found = TRUE; break; }
        } while (Module32NextW(modules, &module));
        CloseHandle(modules);
        if (!found) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) { inaccessible = TRUE; last_error = GetLastError(); continue; }
        FILETIME created, exited, kernel, user;
        BOOL have_time = GetProcessTimes(process, &created, &exited, &kernel, &user);
        if (!have_time) last_error = GetLastError();
        CloseHandle(process);
        if (!have_time) { inaccessible = TRUE; continue; }
        ++engines;
        if (engines == 1) {
            target->pid = entry.th32ProcessID;
            target->engine_base = (uintptr_t)module.modBaseAddr;
            target->engine_size = module.modBaseSize;
            target->created = created;
            wcsncpy(target->engine_path, module.szExePath, MAX_PATH - 1);
            target->engine_path[MAX_PATH - 1] = 0;
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (engines > 1) return TT_TARGET_MULTIPLE;
    if (inaccessible) {
        if (system_error) *system_error = last_error;
        return TT_TARGET_ACCESS;
    }
    if (engines == 1) return TT_TARGET_FOUND;
    return processes ? TT_TARGET_NO_ENGINE : TT_TARGET_NOT_RUNNING;
}
