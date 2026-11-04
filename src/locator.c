#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <strsafe.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>
#include "locator.h"

typedef struct {
    TTEdition edition;
    wchar_t paths[2][TT_PATH_CAP];
    unsigned count;
    unsigned visited;
    ULONGLONG deadline;
    BOOL incomplete;
} Candidates;

static BOOL name_contains(const wchar_t *value, const wchar_t *needle) {
    size_t length = wcslen(needle);
    for (const wchar_t *p = value; *p; ++p)
        if (_wcsnicmp(p, needle, length) == 0) return TRUE;
    return FALSE;
}

static BOOL is_tt_name(const wchar_t *name) {
    return name_contains(name, L"TT语音") || name_contains(name, L"TTVoice");
}

BOOL tt_location_validate_for(TTEdition edition, const wchar_t *input, wchar_t *normalized) {
    if (edition < TT_EDITION_GAME || edition >= TT_EDITION_COUNT || !input || !*input || !normalized) return FALSE;
    wchar_t full[TT_PATH_CAP];
    DWORD n = GetFullPathNameW(input, TT_PATH_CAP, full, NULL);
    if (!n || n >= TT_PATH_CAP) return FALSE;
    const wchar_t *name = wcsrchr(full, L'\\');
    if (!name || _wcsicmp(name + 1, tt_edition_executable(edition)) != 0) return FALSE;
    DWORD attributes = GetFileAttributesW(full);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    DWORD binary_type = 0;
    if (!GetBinaryTypeW(full, &binary_type) ||
        (binary_type != SCS_64BIT_BINARY && binary_type != SCS_32BIT_BINARY)) return FALSE;
    return SUCCEEDED(StringCchCopyW(normalized, TT_PATH_CAP, full));
}

static BOOL config_paths(TTEdition edition, wchar_t *directory, wchar_t *file) {
    wchar_t local[TT_PATH_CAP];
#ifdef TT_LOCATOR_TEST
    DWORD n = GetEnvironmentVariableW(L"TT_LOCATOR_TEST_DIR", local, TT_PATH_CAP);
    if (!n || n >= TT_PATH_CAP)
#endif
    {
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local)))
            return FALSE;
    }
    return SUCCEEDED(StringCchPrintfW(directory, TT_PATH_CAP, L"%ls\\KekemiTTVoiceEnhancer", local)) &&
           SUCCEEDED(StringCchPrintfW(file, TT_PATH_CAP, L"%ls\\%ls", directory,
                     edition == TT_EDITION_ENTERTAINMENT ? L"tt-entertainment-path.txt" : L"tt-path.txt"));
}

BOOL tt_location_load_for(TTEdition edition, wchar_t *path) {
    wchar_t directory[TT_PATH_CAP], file[TT_PATH_CAP], text[TT_PATH_CAP];
    if (!path || !config_paths(edition, directory, file)) return FALSE;
    HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER size;
    DWORD got = 0;
    wchar_t bom = 0;
    BOOL ok = GetFileSizeEx(handle, &size) && size.QuadPart >= 4 &&
              size.QuadPart <= (LONGLONG)(sizeof(text) - sizeof(wchar_t) + sizeof(wchar_t)) &&
              (size.QuadPart % sizeof(wchar_t)) == 0 &&
              ReadFile(handle, &bom, sizeof(bom), &got, NULL) && got == sizeof(bom) && bom == 0xfeff;
    DWORD bytes = ok ? (DWORD)(size.QuadPart - sizeof(wchar_t)) : 0;
    if (ok) ok = ReadFile(handle, text, bytes, &got, NULL) && got == bytes;
    CloseHandle(handle);
    if (!ok || bytes >= sizeof(text)) return FALSE;
    text[bytes / sizeof(wchar_t)] = L'\0';
    return tt_location_validate_for(edition, text, path);
}

BOOL tt_location_save_for(TTEdition edition, const wchar_t *path) {
    wchar_t normalized[TT_PATH_CAP], directory[TT_PATH_CAP], file[TT_PATH_CAP], temporary[TT_PATH_CAP];
    if (!tt_location_validate_for(edition, path, normalized) || !config_paths(edition, directory, file)) return FALSE;
    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    if (FAILED(StringCchPrintfW(temporary, TT_PATH_CAP, L"%ls.%lu.tmp", file,
                                (unsigned long)GetCurrentProcessId()))) return FALSE;
    HANDLE handle = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    wchar_t bom = 0xfeff;
    DWORD written = 0, bytes = (DWORD)(wcslen(normalized) * sizeof(wchar_t));
    BOOL ok = WriteFile(handle, &bom, sizeof(bom), &written, NULL) && written == sizeof(bom) &&
              WriteFile(handle, normalized, bytes, &written, NULL) && written == bytes &&
              FlushFileBuffers(handle);
    CloseHandle(handle);
    if (ok) ok = MoveFileExW(temporary, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temporary);
    return ok;
}

/* When a path is requested, count distinct installation paths, not Electron helper processes. */
static int running_tt(TTEdition edition, wchar_t *path, DWORD *system_error) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (system_error) *system_error = GetLastError();
        return -1;
    }
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry)); entry.dwSize = sizeof(entry);
    unsigned count = 0, distinct = 0;
    wchar_t selected[TT_PATH_CAP] = {0};
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, tt_edition_executable(edition)) != 0) continue;
        ++count;
        if (!path) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process) {
            wchar_t raw[TT_PATH_CAP], normalized[TT_PATH_CAP];
            DWORD size = TT_PATH_CAP;
            if (QueryFullProcessImageNameW(process, 0, raw, &size) &&
                tt_location_validate_for(edition, raw, normalized)) {
                if (!distinct) { StringCchCopyW(selected, TT_PATH_CAP, normalized); distinct = 1; }
                else if (_wcsicmp(selected, normalized) != 0) distinct = 2;
            }
            CloseHandle(process);
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (path && distinct == 1) StringCchCopyW(path, TT_PATH_CAP, selected);
    return path ? (int)distinct : (int)count;
}

static void add_file(Candidates *set, const wchar_t *path) {
    if (set->count >= 2) return;
    wchar_t normalized[TT_PATH_CAP];
    if (!tt_location_validate_for(set->edition, path, normalized)) return;
    for (unsigned i = 0; i < set->count; ++i)
        if (_wcsicmp(set->paths[i], normalized) == 0) return;
    if (SUCCEEDED(StringCchCopyW(set->paths[set->count], TT_PATH_CAP, normalized))) ++set->count;
}

static void add_directory(Candidates *set, const wchar_t *directory) {
    wchar_t path[TT_PATH_CAP];
    const wchar_t *name = tt_edition_executable(set->edition);
    if (SUCCEEDED(StringCchPrintfW(path, TT_PATH_CAP, L"%ls\\%ls", directory, name)))
        add_file(set, path);
    if (SUCCEEDED(StringCchPrintfW(path, TT_PATH_CAP, L"%ls\\%ls\\%ls", directory,
                                   set->edition == TT_EDITION_GAME ? L"TTVoice" : L"TT语音娱乐版", name)))
        add_file(set, path);
}

/* Registry hints may be directories or quoted DisplayIcon/UninstallString values. */
static void add_hint(Candidates *set, const wchar_t *hint) {
    if (!hint || !*hint || set->count >= 2) return;
    wchar_t expanded[TT_PATH_CAP], path[TT_PATH_CAP];
    DWORD count = ExpandEnvironmentStringsW(hint, expanded, TT_PATH_CAP);
    if (!count || count > TT_PATH_CAP) return;
    const wchar_t *start = expanded;
    while (iswspace(*start)) ++start;
    if (*start == L'"') {
        ++start;
        const wchar_t *end = wcschr(start, L'"');
        if (!end || (size_t)(end - start) >= TT_PATH_CAP) return;
        wmemcpy(path, start, end - start); path[end - start] = 0;
    } else {
        if (FAILED(StringCchCopyW(path, TT_PATH_CAP, start))) return;
        wchar_t *comma = wcschr(path, L',');
        if (comma) *comma = 0;
        size_t n = wcslen(path);
        while (n && iswspace(path[n - 1])) path[--n] = 0;
    }
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        add_directory(set, path);
    } else {
        add_file(set, path);
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) { *slash = 0; add_directory(set, path); }
    }
}

static BOOL registry_string(HKEY key, const wchar_t *name, wchar_t *out) {
    DWORD type = 0, size = TT_PATH_CAP * sizeof(wchar_t);
    LONG status = RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t))
        return FALSE;
    out[TT_PATH_CAP - 1] = 0;
    return TRUE;
}

static void find_registry(Candidates *set) {
    static const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    static const REGSAM views[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    for (unsigned i = 0; i < 2 && set->count < 2; ++i) {
        for (unsigned j = 0; j < 2 && set->count < 2; ++j) {
            HKEY key = NULL;
            wchar_t app_key[TT_PATH_CAP];
            if (FAILED(StringCchPrintfW(app_key, TT_PATH_CAP,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls", tt_edition_executable(set->edition)))) continue;
            if (RegOpenKeyExW(roots[i], app_key,
                              0, KEY_READ | views[j], &key) == ERROR_SUCCESS) {
                wchar_t value[TT_PATH_CAP];
                if (registry_string(key, NULL, value)) add_hint(set, value);
                if (registry_string(key, L"Path", value)) add_hint(set, value);
                RegCloseKey(key);
            }
            if (RegOpenKeyExW(roots[i], L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                              0, KEY_READ | views[j], &key) != ERROR_SUCCESS) continue;
            for (DWORD index = 0; set->count < 2; ++index) {
                wchar_t subname[512];
                DWORD length = (DWORD)(sizeof(subname) / sizeof(subname[0]));
                LONG code = RegEnumKeyExW(key, index, subname, &length, NULL, NULL, NULL, NULL);
                if (code == ERROR_NO_MORE_ITEMS) break;
                if (code != ERROR_SUCCESS) continue;
                HKEY child = NULL;
                if (RegOpenKeyExW(key, subname, 0, KEY_READ | views[j], &child) != ERROR_SUCCESS) continue;
                wchar_t name[TT_PATH_CAP], value[TT_PATH_CAP];
                if (registry_string(child, L"DisplayName", name) && is_tt_name(name)) {
                    if (registry_string(child, L"InstallLocation", value)) add_hint(set, value);
                    if (registry_string(child, L"DisplayIcon", value)) add_hint(set, value);
                    if (registry_string(child, L"UninstallString", value)) add_hint(set, value);
                }
                RegCloseKey(child);
            }
            RegCloseKey(key);
        }
    }
}

static void add_shortcut(Candidates *set, const wchar_t *path) {
    IShellLinkW *link = NULL;
    IPersistFile *persist = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr) || !link) return;
    hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&persist);
    if (SUCCEEDED(hr) && persist && SUCCEEDED(IPersistFile_Load(persist, path, STGM_READ))) {
        wchar_t target[TT_PATH_CAP] = {0};
        WIN32_FIND_DATAW data;
        memset(&data, 0, sizeof(data));
        if (SUCCEEDED(IShellLinkW_GetPath(link, target, TT_PATH_CAP, &data, SLGP_RAWPATH)) && target[0])
            add_hint(set, target);
    }
    if (persist) IPersistFile_Release(persist);
    IShellLinkW_Release(link);
}

static void scan_shortcuts(Candidates *set, const wchar_t *directory, unsigned depth) {
    if (set->count >= 2 || depth > 3) return;
    if (++set->visited > 4000 || GetTickCount64() > set->deadline) { set->incomplete = TRUE; return; }
    wchar_t pattern[TT_PATH_CAP], full[TT_PATH_CAP];
    if (FAILED(StringCchPrintfW(pattern, TT_PATH_CAP, L"%ls\\*", directory))) return;
    WIN32_FIND_DATAW item;
    HANDLE find = FindFirstFileW(pattern, &item);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(item.cFileName, L".") == 0 || wcscmp(item.cFileName, L"..") == 0 ||
            (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (FAILED(StringCchPrintfW(full, TT_PATH_CAP, L"%ls\\%ls", directory, item.cFileName))) continue;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_shortcuts(set, full, depth + 1);
        } else {
            const wchar_t *ext = wcsrchr(item.cFileName, L'.');
            if (ext && _wcsicmp(ext, L".lnk") == 0 && is_tt_name(item.cFileName)) add_shortcut(set, full);
        }
    } while (set->count < 2 && !set->incomplete && FindNextFileW(find, &item));
    FindClose(find);
}

static void find_shortcuts(Candidates *set) {
    static const int folders[] = {CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS,
                                  CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY};
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(init)) return;
    set->visited = 0; set->deadline = GetTickCount64() + 2500;
    for (unsigned i = 0; i < sizeof(folders) / sizeof(folders[0]) && set->count < 2; ++i) {
        wchar_t dir[TT_PATH_CAP];
        if (SUCCEEDED(SHGetFolderPathW(NULL, folders[i], NULL, SHGFP_TYPE_CURRENT, dir)))
            scan_shortcuts(set, dir, 0);
        if (set->incomplete) break;
    }
    CoUninitialize();
}

static BOOL skip_directory(const wchar_t *name) {
    static const wchar_t *skipped[] = {L"Windows", L"$Recycle.Bin", L"System Volume Information",
                                       L"Recovery", L"Users", L"Documents and Settings", L"node_modules"};
    for (unsigned i = 0; i < sizeof(skipped) / sizeof(skipped[0]); ++i)
        if (_wcsicmp(name, skipped[i]) == 0) return TRUE;
    return FALSE;
}

static void scan_disk(Candidates *set, const wchar_t *directory, unsigned depth) {
    if (set->count >= 2 || depth > 3) return;
    if (++set->visited > 6000 || GetTickCount64() > set->deadline) { set->incomplete = TRUE; return; }
    wchar_t pattern[TT_PATH_CAP], full[TT_PATH_CAP];
    if (FAILED(StringCchPrintfW(pattern, TT_PATH_CAP, L"%ls\\*", directory))) return;
    WIN32_FIND_DATAW item;
    HANDLE find = FindFirstFileW(pattern, &item);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(item.cFileName, L".") == 0 || wcscmp(item.cFileName, L"..") == 0 ||
            (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (FAILED(StringCchPrintfW(full, TT_PATH_CAP, L"%ls\\%ls", directory, item.cFileName))) continue;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!skip_directory(item.cFileName)) scan_disk(set, full, depth + 1);
        } else if (_wcsicmp(item.cFileName, tt_edition_executable(set->edition)) == 0) {
            add_file(set, full);
        }
    } while (set->count < 2 && !set->incomplete && FindNextFileW(find, &item));
    FindClose(find);
}

static void find_known_paths(Candidates *set) {
    static const wchar_t *directories[] = {L"TT语音", L"TTVoice", L"TT语音娱乐版", L"Program Files\\TT语音娱乐版", L"Program Files\\TT语音",
                                            L"Program Files\\TTVoice", L"Program Files (x86)\\TT语音",
                                            L"Program Files (x86)\\TTVoice"};
    DWORD drives = GetLogicalDrives();
    for (unsigned i = 0; i < 26 && set->count < 2; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[] = L"A:\\";
        root[0] = (wchar_t)(L'A' + i);
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        for (unsigned j = 0; j < sizeof(directories) / sizeof(directories[0]); ++j) {
            wchar_t directory[TT_PATH_CAP];
            if (SUCCEEDED(StringCchPrintfW(directory, TT_PATH_CAP, L"%ls%ls", root, directories[j])))
                add_directory(set, directory);
        }
    }
    wchar_t local[TT_PATH_CAP], directory[TT_PATH_CAP];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local))) {
        static const wchar_t *local_dirs[] = {L"Programs\\TT语音娱乐版", L"TT语音娱乐版", L"Programs\\TT语音", L"Programs\\TTVoice", L"TT语音", L"TTVoice"};
        for (unsigned j = 0; j < sizeof(local_dirs) / sizeof(local_dirs[0]); ++j) {
            if (SUCCEEDED(StringCchPrintfW(directory, TT_PATH_CAP, L"%ls\\%ls", local, local_dirs[j])))
                add_directory(set, directory);
        }
    }
}

static void find_on_disks(Candidates *set) {
    find_known_paths(set);
    if (set->count) return;
    set->visited = 0; set->deadline = GetTickCount64() + 3000;
    DWORD drives = GetLogicalDrives();
    for (unsigned i = 0; i < 26 && set->count < 2 && !set->incomplete; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[] = L"A:\\";
        root[0] = (wchar_t)(L'A' + i);
        if (GetDriveTypeW(root) == DRIVE_FIXED) scan_disk(set, root, 0);
    }
}

static TTLocation from_candidates(const Candidates *set, TTLocationSource source) {
    TTLocation result;
    memset(&result, 0, sizeof(result));
    if (set->count > 1) result.code = TT_LOCATION_MULTIPLE;
    else if (set->incomplete) result.code = TT_LOCATION_INCOMPLETE;
    else if (set->count == 1) {
        result.code = TT_LOCATION_FOUND;
        result.source = source;
        StringCchCopyW(result.path, TT_PATH_CAP, set->paths[0]);
    }
    return result;
}

TTLocation tt_location_find_for(TTEdition edition) {
    TTLocation result;
    memset(&result, 0, sizeof(result));
    if (tt_location_load_for(edition, result.path)) {
        result.code = TT_LOCATION_FOUND; result.source = TT_LOCATION_SAVED; return result;
    }
    wchar_t running[TT_PATH_CAP] = {0};
    if (running_tt(edition, running, NULL) == 1 && running[0]) {
        result.code = TT_LOCATION_FOUND; result.source = TT_LOCATION_RUNNING;
        StringCchCopyW(result.path, TT_PATH_CAP, running);
        return result;
    }
    Candidates set;
    memset(&set, 0, sizeof(set)); set.edition = edition;
    find_registry(&set);
    result = from_candidates(&set, TT_LOCATION_REGISTRY);
    if (result.code != TT_LOCATION_NONE) return result;
#ifdef TT_LOCATOR_TEST
    if (GetEnvironmentVariableW(L"TT_LOCATOR_TEST_MODE", NULL, 0)) return result;
#endif
    memset(&set, 0, sizeof(set)); set.edition = edition;
    find_shortcuts(&set);
    result = from_candidates(&set, TT_LOCATION_SHORTCUT);
    if (result.code != TT_LOCATION_NONE) return result;
    memset(&set, 0, sizeof(set)); set.edition = edition;
    find_on_disks(&set);
    return from_candidates(&set, TT_LOCATION_DISK);
}

TTLaunch tt_location_start_for(TTEdition edition, const wchar_t *path) {
    TTLaunch result = {TT_LAUNCH_FAILED, 0, 0};
    wchar_t executable[TT_PATH_CAP], directory[TT_PATH_CAP], command[TT_PATH_CAP + 4];
    int count = running_tt(edition, NULL, &result.system_error);
    if (count < 0) return result;
    if (count > 0) { result.code = TT_LAUNCH_ALREADY_RUNNING; return result; }
    if (!tt_location_validate_for(edition, path, executable)) { result.system_error = ERROR_FILE_NOT_FOUND; return result; }
    if (FAILED(StringCchCopyW(directory, TT_PATH_CAP, executable)) ||
        FAILED(StringCchPrintfW(command, TT_PATH_CAP + 4, L"\"%ls\"", executable))) {
        result.system_error = ERROR_INSUFFICIENT_BUFFER; return result;
    }
    wchar_t *slash = wcsrchr(directory, L'\\');
    if (!slash) { result.system_error = ERROR_PATH_NOT_FOUND; return result; }
    *slash = 0;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup)); startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process)) {
        result.system_error = GetLastError(); return result;
    }
    result.code = TT_LAUNCH_STARTED;
    result.pid = process.dwProcessId;
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return result;
}

BOOL tt_location_validate(const wchar_t *input, wchar_t *normalized) {
    return tt_location_validate_for(TT_EDITION_GAME, input, normalized);
}
BOOL tt_location_load(wchar_t *path) { return tt_location_load_for(TT_EDITION_GAME, path); }
BOOL tt_location_save(const wchar_t *path) { return tt_location_save_for(TT_EDITION_GAME, path); }
TTLocation tt_location_find(void) { return tt_location_find_for(TT_EDITION_GAME); }
TTLaunch tt_location_start(const wchar_t *path) { return tt_location_start_for(TT_EDITION_GAME, path); }
