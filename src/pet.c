#include "common.h"
#include "anim.h"
#include "db.h"
#include "realm.h"
#include "win_scan.h"
#include "bridge.h"
#include "pricing.h"
#include <math.h>

extern int scan_report_run(void);
extern void panel_open(HWND parent, Db *db);

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

#define PET_BASE_W 300
#define PET_BASE_H 340
#define PET_TIMER_ID 1

#define WM_TRAYICON (WM_APP + 1)
#define WM_PET_PULSE (WM_APP + 10)      /* wParam: unused, lParam: strength*100 */
#define WM_PET_BREAK (WM_APP + 11)
#define WM_PET_STATS (WM_APP + 12)

#define IDM_TOPMOST       1001
#define IDM_CLICKTHROUGH  1002
#define IDM_RESET_SCALE   1003
#define IDM_PANEL         1004
#define IDM_SCAN_NOW      1005
#define IDI_TOKENPET      101
#define IDM_AUTOSTART     1006
#define IDM_RESCAN_ALL    1007
#define IDM_PREVIEW_ANIM  1008
#define IDM_EXIT          1099

static HWND     g_hwnd = NULL;
static HWND     g_panel = NULL;
static HINSTANCE g_inst = NULL;
static NOTIFYICONDATAW g_nid;
static PetAnim  g_anim;
static int      g_win_w = PET_BASE_W, g_win_h = PET_BASE_H;
static bool     g_dragging = false;
static POINT    g_drag_cursor;
static RECT     g_drag_rect;
static HBITMAP  g_dib = NULL;
static void    *g_dib_bits = NULL;
static int      g_dib_w = 0, g_dib_h = 0;
static HDC      g_mem_dc = NULL;
static ULONGLONG g_last_tick = 0;
static HICON    g_icon = NULL;
static bool     g_icon_owned = false;
static Db      *g_db = NULL;
static RealmTable g_realm;
static int64_t  g_xp = 0;
static int64_t  g_today = 0;
static wchar_t  g_realm_name[64] = L"凡人";
static int      g_last_realm_idx = -1;
static int      g_last_sub_idx = -1;
static ULONGLONG g_last_stats = 0;

static void pet_render(void);
static void pet_apply_clickthrough(void);
static void pet_apply_topmost(void);
static void pet_show_menu(POINT pt);

/* ---------- render ---------- */

static bool ensure_dib(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g_dib && g_dib_w == w && g_dib_h == h) return true;
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; g_dib_bits = NULL; }
    if (g_mem_dc) { DeleteDC(g_mem_dc); g_mem_dc = NULL; }

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &g_dib_bits, NULL, 0);
    if (!g_dib) return false;
    g_dib_w = w;
    g_dib_h = h;
    g_mem_dc = CreateCompatibleDC(NULL);
    SelectObject(g_mem_dc, g_dib);
    return true;
}

static void pet_render(void) {
    float scale = g_settings.scale;
    int W = (int)(PET_BASE_W * scale + 0.5f);
    int H = (int)(PET_BASE_H * scale + 0.5f);
    if (W < 60) W = 60;
    if (H < 60) H = 60;
    if (!ensure_dib(W, H)) return;

    memset(g_dib_bits, 0, (size_t)g_dib_w * g_dib_h * 4);
    anim_draw_surface(g_dib_bits, g_dib_w, g_dib_h, scale, &g_anim);

    if (W != g_win_w || H != g_win_h) {
        RECT r;
        GetWindowRect(g_hwnd, &r);
        g_win_w = W;
        g_win_h = H;
        SetWindowPos(g_hwnd, NULL, r.left, r.top, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HDC screen = GetDC(NULL);
    POINT pt_dst = { 0, 0 };
    POINT pt_src = { 0, 0 };
    SIZE sz = { g_dib_w, g_dib_h };
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    pt_dst.x = wr.left;
    pt_dst.y = wr.top;
    UpdateLayeredWindow(g_hwnd, screen, &pt_dst, &sz, g_mem_dc, &pt_src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

/* ---------- interactions ---------- */

static void save_pos(void) {
    RECT r;
    GetWindowRect(g_hwnd, &r);
    g_settings.x = r.left;
    g_settings.y = r.top;
    g_settings.has_pos = true;
    tp_settings_save();
}

static void pet_zoom(float factor, POINT anchor) {
    float old = g_settings.scale;
    float ns = old * factor;
    if (ns < 0.5f) ns = 0.5f;
    if (ns > 2.5f) ns = 2.5f;
    if (fabsf(ns - old) < 0.001f) return;
    RECT r;
    GetWindowRect(g_hwnd, &r);
    float k = ns / old;
    int nw = (int)((r.right - r.left) * k + 0.5f);
    int nh = (int)((r.bottom - r.top) * k + 0.5f);
    int nx = anchor.x - (int)((anchor.x - r.left) * k);
    int ny = anchor.y - (int)((anchor.y - r.top) * k);
    g_settings.scale = ns;
    g_win_w = nw;
    g_win_h = nh;
    SetWindowPos(g_hwnd, NULL, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
    pet_render();
    tp_settings_save();
}

static void pet_apply_topmost(void) {
    SetWindowPos(g_hwnd, g_settings.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void pet_apply_clickthrough(void) {
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (g_settings.click_through) ex |= WS_EX_TRANSPARENT;
    else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
}

static void pet_menu_command(int id) {
    switch (id) {
    case IDM_TOPMOST:
        g_settings.topmost = !g_settings.topmost;
        pet_apply_topmost();
        tp_settings_save();
        break;
    case IDM_CLICKTHROUGH:
        g_settings.click_through = !g_settings.click_through;
        pet_apply_clickthrough();
        tp_settings_save();
        break;
    case IDM_RESET_SCALE: {
        RECT r;
        GetWindowRect(g_hwnd, &r);
        POINT a = { r.left, r.top };
        g_settings.scale = 1.0f;
        g_win_w = PET_BASE_W;
        g_win_h = PET_BASE_H;
        SetWindowPos(g_hwnd, NULL, r.left, r.top, PET_BASE_W, PET_BASE_H, SWP_NOZORDER | SWP_NOACTIVATE);
        (void)a;
        pet_render();
        tp_settings_save();
        break;
    }
    case IDM_AUTOSTART: {
        g_settings.autostart = !g_settings.autostart;
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            if (g_settings.autostart) {
                wchar_t exe[TP_PATH_MAX];
                GetModuleFileNameW(NULL, exe, TP_PATH_MAX);
                wchar_t val[TP_PATH_MAX + 8];
                _snwprintf(val, TP_PATH_MAX + 7, L"\"%s\" --autostart", exe);
                val[TP_PATH_MAX + 7] = 0;
                RegSetValueExW(k, L"TokenPet", 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
            } else {
                RegDeleteValueW(k, L"TokenPet");
            }
            RegCloseKey(k);
        }
        tp_settings_save();
        break;
    }
    case IDM_PANEL:
        if (g_db) panel_open(g_hwnd, g_db);
        break;
    case IDM_SCAN_NOW:
        win_scan_trigger();
        break;
    case IDM_PREVIEW_ANIM:
        anim_next_action();
        break;
    case IDM_RESCAN_ALL:
        if (MessageBoxW(g_hwnd, L"将清空本地统计并从所有数据源重新扫描，确认？", L"Token-Pet",
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            win_scan_reset_all();
            bridge_stop();
            if (g_settings.collector_enabled) bridge_start(g_db, g_settings.wsl_distro, true);
            g_xp = 0;
            anim_trigger_breakthrough(&g_anim);
        }
        break;
    case IDM_EXIT:
        DestroyWindow(g_hwnd);
        break;
    }
}

static void pet_show_menu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_PANEL, L"修炼日志(&L)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_settings.topmost ? MF_CHECKED : 0), IDM_TOPMOST, L"窗口置顶");
    AppendMenuW(m, MF_STRING | (g_settings.click_through ? MF_CHECKED : 0), IDM_CLICKTHROUGH, L"点击穿透（仅托盘菜单可关）");
    AppendMenuW(m, MF_STRING | (g_settings.autostart ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机自启");
    AppendMenuW(m, MF_STRING, IDM_RESET_SCALE, L"恢复 100% 缩放");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SCAN_NOW, L"立即扫描数据");
    AppendMenuW(m, MF_STRING, IDM_PREVIEW_ANIM, L"预览动作");
    AppendMenuW(m, MF_STRING, IDM_RESCAN_ALL, L"重新扫描全部（清空重建）");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

/* ---------- window proc ---------- */

static LRESULT CALLBACK pet_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(h, PET_TIMER_ID, 33, NULL);
        g_last_tick = GetTickCount64();
        return 0;

    case WM_TIMER: {
        ULONGLONG now = GetTickCount64();
        float dt = (float)(now - g_last_tick) / 1000.0f;
        g_last_tick = now;
        if (dt > 0.25f) dt = 0.25f;
        anim_update(&g_anim, dt);

        if (g_db) {
            int64_t drained = win_scan_take_pending_gain();
            if (drained != 0) {
                g_xp += drained;
                if (drained < 0 || drained > 10000000)
                    tp_log("gain %+lld -> xp=%lld", (long long)drained, (long long)g_xp);
                if (drained > 0 && drained < 300000 && g_startup_cycles_pending == 0) {
                    anim_add_popup(&g_anim, drained);
                }
                if (drained > 0) anim_pulse_gain(&g_anim, drained > 200000 ? 0.9f : 0.4f);
            }
        }
        if (g_db && now - g_last_stats > 1000) {
            g_last_stats = now;
            g_today = db_today_xp(g_db);
            int ri = 0, si = -1;
            realm_compute(&g_realm, g_xp, g_realm_name, 64, NULL, NULL, &ri, &si);
            if (ri > g_last_realm_idx || (ri == g_last_realm_idx && si > g_last_sub_idx)) {
                anim_trigger_breakthrough(&g_anim);
                char *rn = tp_wide_to_utf8(g_realm_name);
                tp_log("breakthrough -> %s", rn);
                free(rn);
            }
            g_last_realm_idx = ri;
            g_last_sub_idx = si;
            wcscpy(g_anim.realm, g_realm_name);
            g_anim.total_xp = g_xp;
            g_anim.today_xp = g_today;

            char big[64];
            anim_format_big(big, sizeof(big), g_xp);
            char *rn = tp_wide_to_utf8(g_realm_name);
            char tipA[300];
            tp_snprintf(tipA, sizeof(tipA), "Token-Pet | %s | 总修为 %s", rn, big);
            free(rn);
            wchar_t *tipW = tp_utf8_to_wide(tipA);
            wcsncpy(g_nid.szTip, tipW, 127);
            g_nid.szTip[127] = 0;
            free(tipW);
        }
        pet_render();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (g_settings.click_through) return 0;
        g_dragging = true;
        GetCursorPos(&g_drag_cursor);
        GetWindowRect(h, &g_drag_rect);
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_dragging) {
            POINT p;
            GetCursorPos(&p);
            int nx = g_drag_rect.left + (p.x - g_drag_cursor.x);
            int ny = g_drag_rect.top + (p.y - g_drag_cursor.y);
            SetWindowPos(h, NULL, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            save_pos();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        pet_menu_command(IDM_PANEL);
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT p;
        GetCursorPos(&p);
        float factor = powf(1.12f, (float)delta / 120.0f);
        pet_zoom(factor, p);
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT p;
        GetCursorPos(&p);
        pet_show_menu(p);
        return 0;
    }

    case WM_COMMAND:
        pet_menu_command(LOWORD(wp));
        return 0;

    case WM_PET_PULSE:
        anim_pulse_gain(&g_anim, (float)lp / 100.0f);
        return 0;
    case WM_PET_BREAK:
        anim_trigger_breakthrough(&g_anim);
        return 0;

    case WM_TRAYICON: {
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            POINT p;
            GetCursorPos(&p);
            pet_show_menu(p);
        } else if (lp == WM_LBUTTONDBLCLK) {
            pet_menu_command(IDM_PANEL);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(h, PET_TIMER_ID);
        g_nid.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        bridge_stop();
        if (g_db) db_close(g_db);
        g_db = NULL;
        save_pos();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------- main ---------- */

static void enable_console(void) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)show;
    /* DPI awareness */
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetCtxFn)(DPI_AWARENESS_CONTEXT);
        SetCtxFn fn = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    bool autostart = cmdline && wcsstr(cmdline, L"--autostart") != NULL;

    /* CLI scan report mode */
    if (cmdline && wcsstr(cmdline, L"--scan-report") != NULL) {
        enable_console();
        tp_settings_load();
        scan_report_run();
        FreeConsole();
        return 0;
    }

    g_inst = inst;

    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_DATE_CLASSES;
        InitCommonControlsEx(&icc);
    }

    /* single instance */
    HANDLE mtx = CreateMutexW(NULL, TRUE, TP_MUTEX_NAME);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND old = FindWindowW(TP_APP_CLASS, NULL);
        if (old) {
            ShowWindow(old, SW_SHOW);
            SetForegroundWindow(old);
        }
        return 0;
    }

    anim_gfx_startup();
    tp_settings_load();

    /* icon: embedded resource first, procedural fallback */
    g_icon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_TOKENPET));
    if (g_icon) {
        g_icon_owned = false;
    } else {
        g_icon = anim_make_icon(32);
        g_icon_owned = true;
    }
    if (!g_icon) {
        g_icon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
        g_icon_owned = false;
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = pet_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    wc.lpszClassName = TP_APP_CLASS;
    wc.hIcon = g_icon;
    wc.hIconSm = g_icon;
    RegisterClassExW(&wc);

    int W = (int)(PET_BASE_W * g_settings.scale + 0.5f);
    int H = (int)(PET_BASE_H * g_settings.scale + 0.5f);
    int px = g_settings.x, py = g_settings.y;
    if (!g_settings.has_pos || autostart) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        px = wa.right - W - 24;
        py = wa.bottom - H - 24;
    }
    g_win_w = W;
    g_win_h = H;

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (g_settings.topmost) ex |= WS_EX_TOPMOST;

    g_hwnd = CreateWindowExW(ex, TP_APP_CLASS, TP_APP_NAME, WS_POPUP,
                             px, py, W, H, NULL, NULL, inst, NULL);
    if (!g_hwnd) {
        anim_gfx_shutdown();
        return 1;
    }

    anim_init(&g_anim);

    /* sprite pack (optional): assets/character next to exe or one level up */
    {
        wchar_t exe_dir[TP_PATH_MAX], a1[TP_PATH_MAX], a2[TP_PATH_MAX], m1[TP_PATH_MAX];
        tp_exe_dir(exe_dir, TP_PATH_MAX);
        tp_path_join(a1, TP_PATH_MAX, exe_dir, L"assets\\character");
        tp_path_join(a2, TP_PATH_MAX, exe_dir, L"..\\assets\\character");
        tp_path_join(m1, TP_PATH_MAX, a1, L"manifest.json");
        char *m1u = tp_wide_to_utf8(m1);
        bool ok = tp_file_exists_utf8(m1u);
        free(m1u);
        anim_load_sprites(ok ? a1 : a2);
    }

    /* data layer */
    {
        wchar_t exe_dir[TP_PATH_MAX], cfg_dir[TP_PATH_MAX], data_dir[TP_PATH_MAX], db_path[TP_PATH_MAX];
        tp_exe_dir(exe_dir, TP_PATH_MAX);
        tp_path_join(cfg_dir, TP_PATH_MAX, exe_dir, L"config");
        tp_path_join(data_dir, TP_PATH_MAX, exe_dir, L"data");
        char *u1 = tp_wide_to_utf8(cfg_dir);
        char *u2 = tp_wide_to_utf8(data_dir);
        tp_mkdir_utf8(u1);
        tp_mkdir_utf8(u2);
        free(u1);
        free(u2);
        tp_path_join(db_path, TP_PATH_MAX, data_dir, L"pet.db");

        g_db = db_open(db_path);
        if (g_db) {
            pricing_load(cfg_dir);
            db_lock(g_db);
            pricing_register(db_raw(g_db));
            db_unlock(g_db);
            realm_load(&g_realm, cfg_dir);
            g_xp = db_total_xp(g_db);
            g_today = db_today_xp(g_db);
            realm_compute(&g_realm, g_xp, g_realm_name, 64, NULL, NULL, NULL, NULL);
            wcscpy(g_anim.realm, g_realm_name);
            g_anim.total_xp = g_xp;
            g_anim.today_xp = g_today;

            win_scan_init(g_db, cfg_dir);
            bool fresh = db_count_items(g_db) == 0;
            if (g_settings.collector_enabled) bridge_start(g_db, g_settings.wsl_distro, fresh);
            win_scan_start_thread();
            tp_log("data layer ready: xp=%lld items=%lld fresh=%d", (long long)g_xp, (long long)db_count_items(g_db), fresh ? 1 : 0);
        } else {
            tp_log("db open failed");
        }
    }

    pet_apply_clickthrough();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    /* tray */
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_icon;
    wcsncpy(g_nid.szTip, L"Token-Pet 修仙小人", 127);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    tp_log("token-pet started (scale=%.2f pos=%d,%d)", g_settings.scale, px, py);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_icon && g_icon_owned) DestroyIcon(g_icon);
    if (g_dib) DeleteObject(g_dib);
    if (g_mem_dc) DeleteDC(g_mem_dc);
    if (mtx) CloseHandle(mtx);
    anim_gfx_shutdown();
    return 0;
}
