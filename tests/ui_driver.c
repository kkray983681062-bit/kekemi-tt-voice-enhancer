#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define IDC_READ 1001
#define IDC_RATE 1002
#define IDC_APPLY 1003
#define IDC_HOLD 1004
#define IDC_CURRENT 1005
#define IDC_STATUS 1006
#define IDC_LOCATION 1009
#define IDC_CHOOSE_TT 1010
#define IDC_START_TT 1011
#define IDC_GAME 1012
#define IDC_ENTERTAINMENT 1013

static void print_text(HWND control) {
    wchar_t text[512];
    char utf8[2048];
    GetWindowTextW(control, text, (int)(sizeof(text) / sizeof(text[0])));
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, (int)sizeof(utf8), NULL, NULL);
    puts(utf8);
}

int main(int argc, char **argv) {
    HWND window = FindWindowW(L"TTBitrateControllerWindow", NULL);
    if (!window) return 2;
    if (argc == 3 && strcmp(argv[1], "edition") == 0) {
        int id = strcmp(argv[2], "game") == 0 ? IDC_GAME :
                 strcmp(argv[2], "entertainment") == 0 ? IDC_ENTERTAINMENT : 0;
        HWND target = id ? GetDlgItem(window, id) : NULL;
        if (!target) return 4;
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)target);
    } else if (argc == 2 && strcmp(argv[1], "status") == 0) {
        print_text(GetDlgItem(window, IDC_CURRENT));
        print_text(GetDlgItem(window, IDC_STATUS));
    } else if (argc == 2 && strcmp(argv[1], "location") == 0) {
        print_text(GetDlgItem(window, IDC_LOCATION));
        print_text(GetDlgItem(window, IDC_STATUS));
    } else if (argc == 2 && strcmp(argv[1], "choose") == 0) {
        PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_CHOOSE_TT, BN_CLICKED),
                     (LPARAM)GetDlgItem(window, IDC_CHOOSE_TT));
    } else if (argc == 2 && strcmp(argv[1], "start_tt") == 0) {
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_START_TT, BN_CLICKED),
                     (LPARAM)GetDlgItem(window, IDC_START_TT));
    } else if (argc == 2 && strcmp(argv[1], "close") == 0) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    } else if (argc == 2 && strcmp(argv[1], "read") == 0) {
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_READ, BN_CLICKED),
                     (LPARAM)GetDlgItem(window, IDC_READ));
    } else if (argc == 3 && strcmp(argv[1], "apply") == 0) {
        wchar_t value[16];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, value, (int)(sizeof(value) / sizeof(value[0])));
        SetWindowTextW(GetDlgItem(window, IDC_RATE), value);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_APPLY, BN_CLICKED),
                     (LPARAM)GetDlgItem(window, IDC_APPLY));
    } else if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        SendMessageW(GetDlgItem(window, IDC_HOLD), BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_HOLD, BN_CLICKED),
                     (LPARAM)GetDlgItem(window, IDC_HOLD));
    } else {
        return 3;
    }
    return 0;
}
