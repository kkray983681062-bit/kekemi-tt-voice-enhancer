#define WIN32_LEAN_AND_MEAN
#define STRSAFE_NO_DEPRECATE
#include <windows.h>
#include <commdlg.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include "controller.h"
#include "telemetry.h"
#include "locator.h"
#include "../resources/resource.h"

#define IDC_READ 1001
#define IDC_RATE 1002
#define IDC_APPLY 1003
#define IDC_HOLD 1004
#define IDC_CURRENT 1005
#define IDC_STATUS 1006
#define IDC_MINIMIZE 1007
#define IDC_CLOSE 1008
#define IDC_LOCATION 1009
#define IDC_CHOOSE_TT 1010
#define IDC_START_TT 1011
#define IDC_GAME 1012
#define IDC_ENTERTAINMENT 1013
#define IDT_HOLD 1
#define IDT_METRIC 2
#define WM_WORK_DONE (WM_APP + 1)
#define WM_METRIC_DONE (WM_APP + 2)
#define WM_START_METRIC (WM_APP + 3)
#define WM_LOCATION_DONE (WM_APP + 4)
#define WM_START_LOCATION (WM_APP + 5)
#define TT_ERROR_ELEVATION_REQUIRED 740u
#define CHART_SAMPLES 31
#define WINDOW_WIDTH 568
#define WINDOW_HEIGHT 657

typedef struct {
    HWND window;
    BOOL apply;
    BOOL automatic;
    TTEdition edition;
    int desired;
    TTResult result;
} Work;

typedef struct {
    wchar_t path[TT_PATH_CAP];
    wchar_t input[16];
    wchar_t status[256];
    BOOL status_error;
    BOOL hold_checked;
    BOOL holding;
    BOOL has_config;
    int held_rate;
    unsigned auto_failures;
    unsigned location_generation;
    ULONGLONG location_retry_after;
    ULONGLONG next_hold;
    AudioConfig config;
} EditionState;

static HWND g_window, g_rate, g_current, g_status, g_read, g_apply, g_hold;
static HWND g_live, g_extrema, g_metric_state;
static HWND g_location, g_game, g_entertainment;
static HFONT g_font, g_title_font, g_big_font, g_small_font;
static HICON g_header_icon;
static HBRUSH g_background, g_edit_background;
static BOOL g_busy = FALSE;
static BOOL g_metric_busy = FALSE, g_closing = FALSE, g_has_live = FALSE;
static BOOL g_location_busy = FALSE;
static TTEdition g_edition = TT_EDITION_GAME;
static EditionState g_states[TT_EDITION_COUNT];
static unsigned g_metric_generation = 0;
static COLORREF g_status_color;
static TTMetric g_latest_metric;
static ULONGLONG g_latest_tick;
static double g_samples[CHART_SAMPLES];
static BOOL g_sample_valid[CHART_SAMPLES];

static void start_location(void);
static void finish_close_if_ready(HWND window);

static void status_for(TTEdition edition, const wchar_t *text, BOOL error) {
    EditionState *state = &g_states[edition];
    StringCchCopyW(state->status, sizeof(state->status) / sizeof(state->status[0]), text);
    state->status_error = error;
    if (edition != g_edition) return;
    g_status_color = error ? RGB(255, 151, 151) : RGB(171, 184, 211);
    SetWindowTextW(g_status, text);
    InvalidateRect(g_status, NULL, TRUE);
}

static void status_text(const wchar_t *text, BOOL error) { status_for(g_edition, text, error); }

static void show_config_for(TTEdition edition, const AudioConfig *config) {
    EditionState *state = &g_states[edition];
    state->config = *config;
    state->has_config = TRUE;
    if (edition != g_edition) return;
    wchar_t text[100];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"当前配置：%d kbps", config->bitrate);
    SetWindowTextW(g_current, text);
}

static void stop_holding(TTEdition edition, BOOL uncheck) {
    EditionState *state = &g_states[edition];
    state->holding = FALSE;
    state->held_rate = 0;
    state->auto_failures = 0;
    state->next_hold = 0;
    if (uncheck) {
        state->hold_checked = FALSE;
        if (edition == g_edition) SendMessageW(g_hold, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    if (!g_states[TT_EDITION_GAME].holding && !g_states[TT_EDITION_ENTERTAINMENT].holding)
        KillTimer(g_window, IDT_HOLD);
}

static void show_error(TTEdition edition, const TTResult *result, int desired) {
    wchar_t text[256];
    switch (result->code) {
        case TT_OUT_OF_RANGE:
            status_for(edition, L"请输入 6–192 之间的整数码率。", TRUE); return;
        case TT_NO_TARGET:
            status_for(edition, L"没有找到 TT 语音，请先打开 TT。", TRUE); return;
        case TT_MULTIPLE_TARGETS:
            status_for(edition, L"当前版本有多个音频实例，请仅保留一个后重试。", TRUE); return;
        case TT_NO_ENGINE:
            status_for(edition, L"尚未检测到音频引擎，请先进入 TT 语音房间。", TRUE); return;
        case TT_ACCESS_DENIED:
            status_for(edition, L"无法访问 TT。请让工具与 TT 使用相同权限，必要时以管理员身份启动工具。", TRUE); return;
        case TT_ENGINE_UNSUPPORTED:
            status_for(edition, L"无法识别当前 TT 的音频接口，TT 更新后可能需要适配。", TRUE); return;
        case TT_ENGINE_NOT_READY:
            status_for(edition, L"音频配置尚未就绪，请进入 TT 语音房间后重试。", TRUE); return;
        case TT_SDK_ERROR:
            if (result->system_error == 1003002) {
                status_for(edition, L"TT 音频引擎不接受该码率（1003002）。请降低码率，最高 192 kbps。", TRUE);
                return;
            }
            if (result->system_error == 1000001) {
                status_for(edition, L"TT 音频引擎尚未初始化（1000001）。请先进入语音房间，再重试。", TRUE);
                return;
            }
            if (result->system_error == 1000002) {
                status_for(edition, L"TT 尚未登录语音房间（1000002）。请先进入房间，再重试。", TRUE);
                return;
            }
            swprintf(text, sizeof(text) / sizeof(text[0]),
                     L"TT 音频引擎返回错误码 %ld，请检查 TT 当前状态后重试。",
                     (long)(int32_t)result->system_error);
            status_for(edition, text, TRUE); return;
        case TT_REMOTE_TIMEOUT:
            status_for(edition, L"TT 响应超时，请勿重复操作；重启 TT 后再试。", TRUE); return;
        case TT_READBACK_MISMATCH:
            show_config_for(edition, &result->config);
            swprintf(text, sizeof(text) / sizeof(text[0]),
                     L"请求 %d kbps，但 TT 回读为 %d kbps；设置未确认生效。",
                     desired, result->config.bitrate);
            status_for(edition, text, TRUE); return;
        case TT_REMOTE_FAILED:
            swprintf(text, sizeof(text) / sizeof(text[0]),
                     L"与 TT 的音频接口通信失败（代码 %lu）。",
                     (unsigned long)result->system_error);
            status_for(edition, text, TRUE); return;
        default:
            swprintf(text, sizeof(text) / sizeof(text[0]),
                     L"操作失败（系统代码 %lu）。", (unsigned long)result->system_error);
            status_for(edition, text, TRUE); return;
    }
}

static DWORD WINAPI worker_main(LPVOID argument) {
    Work *work = (Work *)argument;
    work->result = work->apply ? tt_apply_bitrate_for(work->edition, work->desired) :
                                  tt_read_config_for(work->edition);
    if (!PostMessageW(work->window, WM_WORK_DONE, 0, (LPARAM)work)) free(work);
    return 0;
}

static void start_work(TTEdition edition, BOOL apply, int desired, BOOL automatic) {
    if (g_busy) return;
    Work *work = (Work *)calloc(1, sizeof(*work));
    if (!work) {
        status_for(edition, L"内存不足，无法开始操作。", TRUE);
        return;
    }
    work->window = g_window;
    work->edition = edition;
    work->apply = apply;
    work->automatic = automatic;
    work->desired = desired;
    g_busy = TRUE;
    EnableWindow(g_read, FALSE); EnableWindow(g_apply, FALSE);
    EnableWindow(g_game, FALSE); EnableWindow(g_entertainment, FALSE);
    if (!automatic) status_for(edition, apply ? L"正在设置并回读，请稍候…" : L"正在读取，请稍候…", FALSE);
    HANDLE thread = CreateThread(NULL, 0, worker_main, work, 0, NULL);
    if (!thread) {
        free(work);
        g_busy = FALSE;
        EnableWindow(g_read, TRUE); EnableWindow(g_apply, TRUE);
        EnableWindow(g_game, TRUE); EnableWindow(g_entertainment, TRUE);
        status_for(edition, L"无法启动读取任务，请重试。", TRUE);
        return;
    }
    CloseHandle(thread);
}

static HWND make_control(DWORD extended, const wchar_t *kind, const wchar_t *text,
                         DWORD style, int x, int y, int width, int height, int id) {
    HWND control = CreateWindowExW(extended, kind, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, g_window, (HMENU)(INT_PTR)id,
                                   GetModuleHandleW(NULL), NULL);
    if (control) SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
    return control;
}

static void finish_work(Work *work) {
    TTEdition edition = work->edition;
    EditionState *state = &g_states[edition];
    g_busy = FALSE;
    EnableWindow(g_read, TRUE); EnableWindow(g_apply, TRUE);
    EnableWindow(g_game, TRUE); EnableWindow(g_entertainment, TRUE);
    if (work->automatic) state->next_hold = GetTickCount64() + 10000;
    if (work->result.code == TT_OK) {
        show_config_for(edition, &work->result.config);
        if (work->apply) {
            if (!work->automatic) {
                stop_holding(edition, FALSE);
                if (state->hold_checked) {
                    state->holding = TRUE;
                    state->held_rate = work->desired;
                    state->next_hold = GetTickCount64() + 10000;
                    if (!SetTimer(g_window, IDT_HOLD, 1000, NULL)) {
                        stop_holding(edition, TRUE);
                        status_for(edition, L"码率已设置，但无法启动自动保持。", TRUE);
                        free(work);
                        return;
                    }
                }
            }
            state->auto_failures = 0;
            wchar_t text[160];
            swprintf(text, sizeof(text) / sizeof(text[0]),
                     state->holding ? L"已设为 %d kbps；自动保持已开启。" : L"已设为 %d kbps。",
                     work->desired);
            status_for(edition, text, FALSE);
        } else {
            status_for(edition, L"已读取 TT 当前音频配置。", FALSE);
        }
    } else if (work->automatic) {
        BOOL waiting = work->result.code == TT_NO_TARGET || work->result.code == TT_NO_ENGINE;
        if (waiting) {
            status_for(edition, L"正在等待 TT 音频引擎，自动保持仍开启。", FALSE);
        } else if (++state->auto_failures < 3 && work->result.code == TT_SDK_ERROR) {
            show_error(edition, &work->result, work->desired);
        } else {
            stop_holding(edition, TRUE);
            show_error(edition, &work->result, work->desired);
        }
    } else {
        if (work->apply) stop_holding(edition, TRUE);
        show_error(edition, &work->result, work->desired);
    }
    free(work);
}

typedef struct {
    HWND window;
    TTEdition edition;
    unsigned generation;
    TTMetric value;
} MetricWork;

static void update_extrema(void) {
    double low = 0.0, high = 0.0;
    BOOL any = FALSE;
    for (int i = 0; i < CHART_SAMPLES; ++i) {
        if (!g_sample_valid[i]) continue;
        if (!any || g_samples[i] < low) low = g_samples[i];
        if (!any || g_samples[i] > high) high = g_samples[i];
        any = TRUE;
    }
    wchar_t text[128];
    if (any)
        swprintf(text, sizeof(text) / sizeof(text[0]), L"谷值 %.2f     峰值 %.2f", low, high);
    else
        wcscpy(text, L"谷值 --     峰值 --");
    SetWindowTextW(g_extrema, text);
}

static void update_metric(TTMetric value) {
    if (!g_states[g_edition].path[0] && !g_location_busy && !g_closing &&
        value.code != TT_METRIC_NO_TT && value.code != TT_METRIC_MULTIPLE_TT &&
        value.code != TT_METRIC_ACCESS && GetTickCount64() >= g_states[g_edition].location_retry_after)
        start_location();
    g_latest_metric = value;
    g_latest_tick = GetTickCount64();
    g_has_live = value.code == TT_METRIC_ACTIVE;
    if (g_has_live) {
        wchar_t text[40];
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%.2f", value.kbps);
        SetWindowTextW(g_live, text);
        SetWindowTextW(g_metric_state, L"推流状态：正在发送");
        return;
    }
    SetWindowTextW(g_live, L"--");
    switch (value.code) {
        case TT_METRIC_NO_TT:
            SetWindowTextW(g_metric_state, L"推流状态：请先打开 TT 语音"); break;
        case TT_METRIC_MULTIPLE_TT:
            SetWindowTextW(g_metric_state, L"推流状态：检测到多个 TT 进程"); break;
        case TT_METRIC_ACCESS:
            SetWindowTextW(g_metric_state, L"推流状态：无法读取，请检查运行权限"); break;
        case TT_METRIC_UNSUPPORTED:
            SetWindowTextW(g_metric_state, L"推流状态：当前引擎版本暂不支持实时曲线"); break;
        default:
            SetWindowTextW(g_metric_state, L"推流状态：等待进入房间并开始发送"); break;
    }
}

static DWORD WINAPI metric_worker(LPVOID argument) {
    MetricWork *work = (MetricWork *)argument;
    work->value = tt_read_telemetry_for(work->edition);
    if (!PostMessageW(work->window, WM_METRIC_DONE, 0, (LPARAM)work)) free(work);
    return 0;
}

static void start_metric(void) {
    if (g_metric_busy || g_closing) return;
    MetricWork *work = (MetricWork *)calloc(1, sizeof(*work));
    if (!work) return;
    work->window = g_window;
    work->edition = g_edition;
    work->generation = g_metric_generation;
    g_metric_busy = TRUE;
    HANDLE thread = CreateThread(NULL, 0, metric_worker, work, 0, NULL);
    if (!thread) {
        g_metric_busy = FALSE;
        free(work);
        return;
    }
    CloseHandle(thread);
}

static void chart_tick(void) {
    memmove(g_samples, g_samples + 1, (CHART_SAMPLES - 1) * sizeof(g_samples[0]));
    memmove(g_sample_valid, g_sample_valid + 1, (CHART_SAMPLES - 1) * sizeof(g_sample_valid[0]));
    BOOL fresh = g_has_live && GetTickCount64() - g_latest_tick <= 2500;
    g_sample_valid[CHART_SAMPLES - 1] = fresh;
    if (fresh) {
        g_samples[CHART_SAMPLES - 1] = g_latest_metric.kbps;
    } else if (g_has_live) {
        g_has_live = FALSE;
        SetWindowTextW(g_live, L"--");
        SetWindowTextW(g_metric_state, L"推流状态：等待实时数据");
    }
    update_extrema();
    RECT area = {20, 165, 549, 332};
    InvalidateRect(g_window, &area, FALSE);
    start_metric();
}

static void paint_chart(HDC dc) {
    const int left = 20, right = 548, top = 166, bottom = 330;
    RECT graph = {left, top, right + 1, bottom + 1};
    HBRUSH fill = CreateSolidBrush(RGB(13, 16, 29));
    FillRect(dc, &graph, fill);
    DeleteObject(fill);

    SetBkMode(dc, TRANSPARENT);
    HPEN grid = CreatePen(PS_DOT, 1, RGB(40, 45, 66));
    HGDIOBJ old_pen = SelectObject(dc, grid);
    for (int i = 1; i < 6; ++i) {
        int x = left + (right - left) * i / 6;
        MoveToEx(dc, x, top, NULL); LineTo(dc, x, bottom);
    }
    for (int i = 1; i < 4; ++i) {
        int y = top + (bottom - top) * i / 4;
        MoveToEx(dc, left, y, NULL); LineTo(dc, right, y);
    }
    SelectObject(dc, old_pen);
    DeleteObject(grid);

    double maximum = 0.0;
    for (int i = 0; i < CHART_SAMPLES; ++i)
        if (g_sample_valid[i] && g_samples[i] > maximum) maximum = g_samples[i];
    double scale = ceil(maximum / 32.0) * 32.0;
    if (scale < 64.0) scale = 64.0;

    HPEN line = CreatePen(PS_SOLID, 2, RGB(155, 166, 255));
    old_pen = SelectObject(dc, line);
    BOOL preceding = FALSE;
    int last_x = 0, last_y = 0;
    for (int i = 0; i < CHART_SAMPLES; ++i) {
        if (!g_sample_valid[i]) { preceding = FALSE; continue; }
        int x = left + (right - left) * i / (CHART_SAMPLES - 1);
        int y = bottom - (int)((bottom - top - 5) * g_samples[i] / scale);
        if (!preceding) MoveToEx(dc, x, y, NULL);
        else LineTo(dc, x, y);
        preceding = TRUE;
        last_x = x; last_y = y;
    }
    if (g_sample_valid[CHART_SAMPLES - 1]) {
        HBRUSH point = CreateSolidBrush(RGB(144, 157, 255));
        HGDIOBJ old_brush = SelectObject(dc, point);
        Ellipse(dc, last_x - 4, last_y - 4, last_x + 5, last_y + 5);
        SelectObject(dc, old_brush);
        DeleteObject(point);
    }
    SelectObject(dc, old_pen);
    DeleteObject(line);
}

static void draw_button(const DRAWITEMSTRUCT *item) {
    BOOL title_button = item->CtlID == IDC_MINIMIZE || item->CtlID == IDC_CLOSE;
    BOOL enabled = !(item->itemState & ODS_DISABLED);
    BOOL version_selected = (item->CtlID == IDC_GAME && g_edition == TT_EDITION_GAME) ||
                            (item->CtlID == IDC_ENTERTAINMENT && g_edition == TT_EDITION_ENTERTAINMENT);
    COLORREF fill = title_button ? RGB(10, 12, 21) :
                    (item->CtlID == IDC_APPLY || version_selected) ? RGB(92, 102, 194) : RGB(31, 37, 57);
    if (item->itemState & ODS_SELECTED)
        fill = title_button ? RGB(40, 44, 61) : RGB(66, 75, 134);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN border = CreatePen(PS_SOLID, 1, title_button ? fill : RGB(66, 74, 105));
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, border);
    RoundRect(item->hDC, item->rcItem.left, item->rcItem.top,
              item->rcItem.right, item->rcItem.bottom, 9, 9);
    SelectObject(item->hDC, old_pen);
    SelectObject(item->hDC, old_brush);
    DeleteObject(border);
    DeleteObject(brush);
    wchar_t text[80];
    GetWindowTextW(item->hwndItem, text, (int)(sizeof(text) / sizeof(text[0])));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, enabled ? RGB(236, 239, 250) : RGB(113, 121, 142));
    HGDIOBJ old_font = SelectObject(item->hDC, title_button ? g_title_font : g_font);
    RECT rect = item->rcItem;
    DrawTextW(item->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item->hDC, old_font);
    if ((item->itemState & ODS_FOCUS) && !title_button) {
        InflateRect(&rect, -4, -4);
        DrawFocusRect(item->hDC, &rect);
    }
}

static void set_font(HWND control, HFONT font) {
    if (control) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

typedef struct {
    HWND window;
    TTEdition edition;
    unsigned generation;
    TTLocation location;
} LocationWork;

static void finish_close_if_ready(HWND window) {
    if (g_closing && !g_metric_busy && !g_location_busy) DestroyWindow(window);
}

static void show_location(const wchar_t *path) {
    wchar_t text[TT_PATH_CAP + 16];
    StringCchPrintfW(text, sizeof(text) / sizeof(text[0]), L"TT：%ls", path);
    SetWindowTextW(g_location, text);
}

static DWORD WINAPI location_worker(LPVOID argument) {
    LocationWork *work = (LocationWork *)argument;
    work->location = tt_location_find_for(work->edition);
    if (!PostMessageW(work->window, WM_LOCATION_DONE, 0, (LPARAM)work)) free(work);
    return 0;
}

static void start_location(void) {
    if (g_location_busy || g_closing) return;
    LocationWork *work = (LocationWork *)calloc(1, sizeof(*work));
    if (!work) return;
    work->window = g_window;
    work->edition = g_edition;
    work->generation = g_states[g_edition].location_generation;
    g_location_busy = TRUE;
    g_states[g_edition].location_retry_after = GetTickCount64() + 10000;
    HANDLE thread = CreateThread(NULL, 0, location_worker, work, 0, NULL);
    if (!thread) {
        free(work);
        g_location_busy = FALSE;
        SetWindowTextW(g_location, L"TT：无法自动查找，请手动选择");
        return;
    }
    CloseHandle(thread);
}

static void choose_tt(void) {
    EditionState *state = &g_states[g_edition];
    wchar_t selected[TT_PATH_CAP] = {0}, normalized[TT_PATH_CAP];
    if (state->path[0]) StringCchCopyW(selected, TT_PATH_CAP, state->path);
    OPENFILENAMEW dialog;
    memset(&dialog, 0, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_window;
    dialog.lpstrFilter = g_edition == TT_EDITION_GAME ?
        L"开黑版程序 (TTVoice.exe)\0TTVoice.exe\0可执行文件 (*.exe)\0*.exe\0\0" :
        L"娱乐版程序 (TT语音娱乐版.exe)\0TT语音娱乐版.exe\0可执行文件 (*.exe)\0*.exe\0\0";
    dialog.lpstrFile = selected;
    dialog.nMaxFile = TT_PATH_CAP;
    dialog.lpstrTitle = g_edition == TT_EDITION_GAME ? L"选择开黑版 TTVoice.exe" : L"选择 TT语音娱乐版.exe";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError()) status_text(L"无法打开文件选择窗口，请重试。", TRUE);
        return;
    }
    if (!tt_location_validate_for(g_edition, selected, normalized)) {
        status_text(g_edition == TT_EDITION_GAME ? L"请选择开黑版 TTVoice.exe。" :
                    L"请选择 TT语音娱乐版.exe。", TRUE);
        return;
    }
    ++state->location_generation;
    StringCchCopyW(state->path, TT_PATH_CAP, normalized);
    show_location(state->path);
    if (tt_location_save_for(g_edition, state->path)) status_text(L"已选择并记住当前版本的位置。", FALSE);
    else status_text(L"已选择 TT，但无法保存位置；下次可能需要重新选择。", TRUE);
}

static void start_tt(void) {
    EditionState *state = &g_states[g_edition];
    TTLaunch result = tt_location_start_for(g_edition, state->path);
    if (result.code == TT_LAUNCH_STARTED) {
        status_text(L"已启动当前版本，请进入语音房间并上麦。", FALSE);
    } else if (result.code == TT_LAUNCH_ALREADY_RUNNING) {
        status_text(L"当前版本已经打开，请进入语音房间并上麦。", FALSE);
    } else if (!state->path[0] && result.system_error == ERROR_FILE_NOT_FOUND) {
        status_text(L"还没找到当前版本，请先点击“选择 TT”。", TRUE);
    } else if (result.system_error == TT_ERROR_ELEVATION_REQUIRED) {
        status_text(L"TT 需要管理员权限，请以管理员身份运行工具或手动打开 TT。", TRUE);
    } else if (result.system_error == ERROR_FILE_NOT_FOUND || result.system_error == ERROR_PATH_NOT_FOUND) {
        status_text(L"TT 程序不存在或不是所选版本，请重新选择。", TRUE);
    } else {
        wchar_t text[120];
        swprintf(text, sizeof(text) / sizeof(text[0]), L"启动 TT 失败（系统代码 %lu）。",
                 (unsigned long)result.system_error);
        status_text(text, TRUE);
    }
}

static void select_edition(TTEdition edition) {
    if (g_busy || edition == g_edition || edition < TT_EDITION_GAME || edition >= TT_EDITION_COUNT) return;
    EditionState *previous = &g_states[g_edition];
    GetWindowTextW(g_rate, previous->input, (int)(sizeof(previous->input) / sizeof(previous->input[0])));
    previous->hold_checked = SendMessageW(g_hold, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edition = edition;
    EditionState *state = &g_states[edition];
    ++g_metric_generation;
    InvalidateRect(g_game, NULL, TRUE); InvalidateRect(g_entertainment, NULL, TRUE);
    SetWindowTextW(g_rate, state->input);
    SendMessageW(g_hold, BM_SETCHECK, state->hold_checked ? BST_CHECKED : BST_UNCHECKED, 0);
    if (state->has_config) show_config_for(edition, &state->config);
    else SetWindowTextW(g_current, L"当前配置：尚未读取");
    wchar_t saved_status[256];
    StringCchCopyW(saved_status, sizeof(saved_status) / sizeof(saved_status[0]), state->status);
    status_for(edition, saved_status, state->status_error);
    if (state->path[0]) show_location(state->path);
    else {
        SetWindowTextW(g_location, L"TT：正在自动查找…");
        start_location();
    }
    memset(g_samples, 0, sizeof(g_samples));
    memset(g_sample_valid, 0, sizeof(g_sample_valid));
    g_has_live = FALSE; g_latest_tick = 0;
    SetWindowTextW(g_live, L"--");
    SetWindowTextW(g_metric_state, L"推流状态：正在检测");
    update_extrema();
    RECT area = {20, 165, 549, 332};
    InvalidateRect(g_window, &area, FALSE);
    PostMessageW(g_window, WM_START_METRIC, 0, 0);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            g_window = window;
            for (int i = 0; i < TT_EDITION_COUNT; ++i) {
                g_states[i].hold_checked = TRUE;
                StringCchCopyW(g_states[i].input, 16, L"192");
                StringCchCopyW(g_states[i].status, 256, L"等待操作。");
            }
            g_background = CreateSolidBrush(RGB(10, 12, 21));
            g_edit_background = CreateSolidBrush(RGB(27, 32, 49));
            g_status_color = RGB(171, 184, 211);
            g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_title_font = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_big_font = CreateFontW(-44, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_small_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_header_icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON),
                                            IMAGE_ICON, 28, 28, LR_DEFAULTCOLOR);
            HWND avatar = make_control(0, L"STATIC", L"", SS_ICON | SS_CENTERIMAGE, 20, 10, 28, 28, 0);
            if (avatar && g_header_icon) SendMessageW(avatar, STM_SETICON, (WPARAM)g_header_icon, 0);
            HWND title = make_control(0, L"STATIC", L"克克咪 TT语音增强", 0, 56, 14, 374, 28, 0);
            set_font(title, g_title_font);
            make_control(0, L"BUTTON", L"—", WS_TABSTOP | BS_OWNERDRAW, 488, 8, 35, 32, IDC_MINIMIZE);
            make_control(0, L"BUTTON", L"×", WS_TABSTOP | BS_OWNERDRAW, 527, 8, 32, 32, IDC_CLOSE);
            HWND caption = make_control(0, L"STATIC", L"实时上行码率", 0, 22, 55, 200, 22, 0);
            set_font(caption, g_small_font);
            HWND edition_label = make_control(0, L"STATIC", L"版本", 0, 282, 59, 43, 20, 0);
            set_font(edition_label, g_small_font);
            g_game = make_control(0, L"BUTTON", L"开黑版", WS_TABSTOP | BS_OWNERDRAW, 326, 50, 105, 32, IDC_GAME);
            g_entertainment = make_control(0, L"BUTTON", L"娱乐版", WS_TABSTOP | BS_OWNERDRAW, 438, 50, 105, 32, IDC_ENTERTAINMENT);
            g_live = make_control(0, L"STATIC", L"--", SS_LEFT, 18, 74, 205, 60, 0);
            set_font(g_live, g_big_font);
            HWND unit = make_control(0, L"STATIC", L"Kbps", 0, 220, 105, 60, 20, 0);
            set_font(unit, g_small_font);
            g_extrema = make_control(0, L"STATIC", L"谷值 --     峰值 --", SS_RIGHT, 294, 105, 250, 22, 0);
            set_font(g_extrema, g_small_font);
            HWND graph_label = make_control(0, L"STATIC", L"最近 30 秒 · 实时音频发送", 0, 22, 140, 300, 22, 0);
            set_font(graph_label, g_small_font);
            HWND from = make_control(0, L"STATIC", L"30 秒前", 0, 20, 337, 100, 20, 0);
            HWND now = make_control(0, L"STATIC", L"现在", SS_RIGHT, 488, 337, 60, 20, 0);
            set_font(from, g_small_font); set_font(now, g_small_font);
            g_metric_state = make_control(0, L"STATIC", L"推流状态：正在检测", 0, 24, 365, 500, 24, 0);

            g_current = make_control(0, L"STATIC", L"当前配置：尚未读取", 0, 24, 406, 335, 25, IDC_CURRENT);
            g_read = make_control(0, L"BUTTON", L"读取当前码率", WS_TABSTOP | BS_OWNERDRAW,
                                  397, 399, 146, 36, IDC_READ);
            make_control(0, L"STATIC", L"目标码率", 0, 24, 454, 80, 25, 0);
            g_rate = make_control(0, L"EDIT", L"192",
                                  WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                                  110, 447, 95, 31, IDC_RATE);
            SendMessageW(g_rate, EM_SETLIMITTEXT, 3, 0);
            make_control(0, L"STATIC", L"kbps（6–192）", 0, 216, 454, 150, 25, 0);
            g_apply = make_control(0, L"BUTTON", L"应用码率", WS_TABSTOP | BS_OWNERDRAW,
                                   397, 445, 146, 36, IDC_APPLY);
            g_hold = make_control(0, L"BUTTON", L"应用后自动保持（两版独立；关闭工具后停止）",
                                  WS_TABSTOP | BS_AUTOCHECKBOX, 24, 492, 465, 27, IDC_HOLD);
            SendMessageW(g_hold, BM_SETCHECK, BST_CHECKED, 0);
            g_status = make_control(0, L"STATIC", L"等待操作。", SS_LEFT, 24, 528, 520, 46, IDC_STATUS);
            g_location = make_control(0, L"STATIC", L"TT：正在自动查找…", SS_PATHELLIPSIS | SS_NOPREFIX,
                                      24, 582, 318, 24, IDC_LOCATION);
            set_font(g_location, g_small_font);
            make_control(0, L"BUTTON", L"选择 TT", WS_TABSTOP | BS_OWNERDRAW,
                         347, 573, 93, 35, IDC_CHOOSE_TT);
            make_control(0, L"BUTTON", L"启动 TT", WS_TABSTOP | BS_OWNERDRAW,
                         448, 573, 95, 35, IDC_START_TT);
            HWND note = make_control(0, L"STATIC", L"曲线仅保留最近 30 秒；目标值与实际发送会有差异。",
                                     0, 24, 624, 520, 22, 0);
            set_font(note, g_small_font);
            SetTimer(window, IDT_METRIC, 1000, NULL);
            PostMessageW(window, WM_START_METRIC, 0, 0);
            PostMessageW(window, WM_START_LOCATION, 0, 0);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_GAME) {
                select_edition(TT_EDITION_GAME); return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_ENTERTAINMENT) {
                select_edition(TT_EDITION_ENTERTAINMENT); return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_MINIMIZE) {
                ShowWindow(window, SW_MINIMIZE); return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_CLOSE) {
                SendMessageW(window, WM_CLOSE, 0, 0); return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_CHOOSE_TT) {
                choose_tt(); return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_START_TT) {
                start_tt(); return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_READ) {
                start_work(g_edition, FALSE, 0, FALSE); return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_APPLY) {
                wchar_t input[16], *end = NULL;
                GetWindowTextW(g_rate, input, (int)(sizeof(input) / sizeof(input[0])));
                long rate = wcstol(input, &end, 10);
                if (!input[0] || !end || *end || rate < TT_MIN_KBPS || rate > TT_MAX_KBPS) {
                    TTResult result = {0};
                    result.code = TT_OUT_OF_RANGE;
                    show_error(g_edition, &result, 0);
                    return 0;
                }
                stop_holding(g_edition, FALSE);
                start_work(g_edition, TRUE, (int)rate, FALSE);
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDC_HOLD) {
                g_states[g_edition].hold_checked = SendMessageW(g_hold, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (!g_states[g_edition].hold_checked) {
                    stop_holding(g_edition, FALSE);
                    status_text(L"自动保持已关闭；TT 当前配置不会被恢复。", FALSE);
                } else if (!g_states[g_edition].holding) {
                    status_text(L"点击“应用码率”后开始自动保持。", FALSE);
                }
                return 0;
            }
            break;
        case WM_TIMER:
            if (wparam == IDT_HOLD && !g_busy && !g_closing) {
                ULONGLONG now = GetTickCount64();
                for (int i = 0; i < TT_EDITION_COUNT; ++i) {
                    EditionState *state = &g_states[i];
                    if (state->holding && now >= state->next_hold) {
                        state->next_hold = now + 10000;
                        start_work((TTEdition)i, TRUE, state->held_rate, TRUE);
                        break;
                    }
                }
            } else if (wparam == IDT_METRIC && !g_closing) chart_tick();
            return 0;
        case WM_START_METRIC:
            start_metric(); return 0;
        case WM_START_LOCATION:
            start_location(); return 0;
        case WM_METRIC_DONE: {
            MetricWork *work = (MetricWork *)lparam;
            g_metric_busy = FALSE;
            if (!g_closing && work->edition == g_edition && work->generation == g_metric_generation)
                update_metric(work->value);
            else if (!g_closing)
                PostMessageW(window, WM_START_METRIC, 0, 0);
            free(work);
            finish_close_if_ready(window);
            return 0;
        }
        case WM_LOCATION_DONE: {
            LocationWork *work = (LocationWork *)lparam;
            TTEdition edition = work->edition;
            EditionState *state = &g_states[edition];
            g_location_busy = FALSE;
            if (!g_closing && work->generation == state->location_generation) {
                wchar_t normalized[TT_PATH_CAP];
                if (work->location.code == TT_LOCATION_FOUND &&
                    tt_location_validate_for(edition, work->location.path, normalized)) {
                    StringCchCopyW(state->path, TT_PATH_CAP, normalized);
                    if (edition == g_edition) show_location(state->path);
                    if (work->location.source != TT_LOCATION_SAVED && !tt_location_save_for(edition, state->path))
                        status_for(edition, L"已找到 TT，但无法保存位置；下次可能需要重新选择。", TRUE);
                } else if (edition == g_edition) {
                    if (work->location.code == TT_LOCATION_MULTIPLE)
                        SetWindowTextW(g_location, L"TT：找到多个候选，请手动选择");
                    else if (work->location.code == TT_LOCATION_INCOMPLETE)
                        SetWindowTextW(g_location, L"TT：自动查找未完成，请手动选择");
                    else SetWindowTextW(g_location, L"TT：未找到，请手动选择");
                }
            }
            free(work);
            if (!g_closing && edition != g_edition && !g_states[g_edition].path[0]) start_location();
            finish_close_if_ready(window);
            return 0;
        }
        case WM_WORK_DONE:
            finish_work((Work *)lparam); return 0;
        case WM_DRAWITEM:
            draw_button((const DRAWITEMSTRUCT *)lparam); return TRUE;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkColor((HDC)wparam, RGB(10, 12, 21));
            SetTextColor((HDC)wparam, (HWND)lparam == g_status ? g_status_color :
                         (HWND)lparam == g_live ? RGB(249, 250, 255) : RGB(180, 193, 217));
            return (LRESULT)g_background;
        case WM_CTLCOLOREDIT:
            SetBkColor((HDC)wparam, RGB(27, 32, 49));
            SetTextColor((HDC)wparam, RGB(240, 243, 255));
            return (LRESULT)g_edit_background;
        case WM_NCHITTEST: {
            POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
            ScreenToClient(window, &point);
            if (point.y >= 0 && point.y < 44 && point.x >= 0 && point.x < 480) return HTCAPTION;
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            paint_chart(dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rect;
            GetClientRect(window, &rect);
            FillRect((HDC)wparam, &rect, g_background);
            return 1;
        }
        case WM_CLOSE:
            if (g_busy) {
                status_text(L"正在与 TT 通信，请稍候再关闭。", FALSE);
                return 0;
            }
            stop_holding(TT_EDITION_GAME, FALSE);
            stop_holding(TT_EDITION_ENTERTAINMENT, FALSE);
            g_closing = TRUE;
            KillTimer(window, IDT_METRIC);
            finish_close_if_ready(window);
            return 0;
        case WM_DESTROY:
            if (g_header_icon) DestroyIcon(g_header_icon);
            if (g_font) DeleteObject(g_font);
            if (g_title_font) DeleteObject(g_title_font);
            if (g_big_font) DeleteObject(g_big_font);
            if (g_small_font) DeleteObject(g_small_font);
            if (g_background) DeleteObject(g_background);
            if (g_edit_background) DeleteObject(g_edit_background);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line;
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TTBitrateControllerWindow";
    if (!RegisterClassW(&wc)) return 1;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - WINDOW_WIDTH) / 2;
    int y = work.top + ((work.bottom - work.top) - WINDOW_HEIGHT) / 2;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"克克咪 TT语音增强",
                                  WS_POPUP | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                  x, y, WINDOW_WIDTH, WINDOW_HEIGHT,
                                  NULL, NULL, instance, NULL);
    if (!window) return 1;
    HRGN region = CreateRoundRectRgn(0, 0, WINDOW_WIDTH + 1, WINDOW_HEIGHT + 1, 20, 20);
    if (region && !SetWindowRgn(window, region, TRUE)) DeleteObject(region);
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}
