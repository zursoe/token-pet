#include "common.h"
#include "db.h"
#include "realm.h"
#include "sources.h"
#include "win_scan.h"
#include "bridge.h"
#include "sqlite3.h"

#define PANEL_TIMER 7

static HWND g_panel = NULL;
static HWND g_tabs = NULL;
static HWND g_list = NULL;
static Db  *g_panel_db = NULL;
static int  g_cur_tab = 0;
static HWND g_btn_edit = NULL, g_btn_reset = NULL, g_btn_scan = NULL;

#define ID_BTN_EDIT  201
#define ID_BTN_RESET 202
#define ID_BTN_SCAN  203
#define ID_DLG_EDIT  301
#define ID_DLG_OK    302
#define ID_DLG_CANCEL 303

/* ---------- path edit dialog ---------- */

static HWND g_dlg = NULL, g_dlg_edit = NULL;
static char g_dlg_section[16] = "";
static char g_dlg_tool[32] = "";
static wchar_t g_dlg_current[800];

static LRESULT CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"手动指定该数据源位置（留空 = 恢复自动探测）",
                        WS_CHILD | WS_VISIBLE, 12, 12, 520, 20, h, NULL, NULL, NULL);
        g_dlg_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_dlg_current,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     12, 40, 520, 26, h, (HMENU)ID_DLG_EDIT, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        340, 80, 90, 30, h, (HMENU)ID_DLG_OK, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                        440, 80, 90, 30, h, (HMENU)ID_DLG_CANCEL, NULL, NULL);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_DLG_OK) {
            wchar_t buf[800];
            GetWindowTextW(g_dlg_edit, buf, 800);
            char *u = tp_wide_to_utf8(buf);
            sources_cfg_set(g_dlg_section, g_dlg_tool, u);
            free(u);
            if (strcmp(g_dlg_section, "win") == 0) {
                win_scan_trigger();
            } else {
                bridge_stop();
                if (g_settings.collector_enabled && g_panel_db)
                    bridge_start(g_panel_db, g_settings.wsl_distro, true);
            }
            DestroyWindow(h);
        } else if (id == ID_DLG_CANCEL) {
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_dlg = NULL;
        g_dlg_edit = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void open_edit_dialog(HWND parent) {
    if (g_cur_tab != 3) {
        MessageBoxW(parent, L"请先切换到「数据源」页签并选中一行。", L"Token-Pet", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0) {
        MessageBoxW(parent, L"请先在列表中选择一个数据源位置。", L"Token-Pet", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t toolW[64] = L"", labelW[64] = L"", pathW[800] = L"";
    ListView_GetItemText(g_list, sel, 0, toolW, 64);
    ListView_GetItemText(g_list, sel, 1, labelW, 64);
    ListView_GetItemText(g_list, sel, 2, pathW, 800);

    char *toolU = tp_wide_to_utf8(toolW);
    snprintf(g_dlg_tool, sizeof(g_dlg_tool), "%s", toolU ? toolU : "");
    free(toolU);
    snprintf(g_dlg_section, sizeof(g_dlg_section), "%s", (labelW[0] == L'W') ? "wsl" : "win");

    char *ov = sources_cfg_get(g_dlg_section, g_dlg_tool);
    if (ov) {
        wchar_t *w = tp_utf8_to_wide(ov);
        wcsncpy(g_dlg_current, w, 799);
        free(w);
        free(ov);
    } else {
        wcsncpy(g_dlg_current, pathW, 799);
    }
    g_dlg_current[799] = 0;

    static bool cls_done = false;
    if (!cls_done) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dlg_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"TokenPetPathDlg";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassExW(&wc);
        cls_done = true;
    }
    RECT pr, dr;
    GetWindowRect(parent, &pr);
    int w = 560, h = 160;
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    g_dlg = CreateWindowExW(WS_EX_TOOLWINDOW, L"TokenPetPathDlg", L"数据源位置",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                            x, y, w, h, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (g_dlg) {
        SetForegroundWindow(g_dlg);
        SetFocus(g_dlg_edit);
    }
}

static void reset_selected(HWND parent) {
    if (g_cur_tab != 3) return;
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0) return;
    wchar_t toolW[64] = L"", labelW[64] = L"";
    ListView_GetItemText(g_list, sel, 0, toolW, 64);
    ListView_GetItemText(g_list, sel, 1, labelW, 64);
    char *toolU = tp_wide_to_utf8(toolW);
    sources_cfg_set(labelW[0] == L'W' ? "wsl" : "win", toolU, NULL);
    free(toolU);
    if (labelW[0] == L'W') {
        bridge_stop();
        if (g_settings.collector_enabled && g_panel_db)
            bridge_start(g_panel_db, g_settings.wsl_distro, true);
    } else {
        win_scan_trigger();
    }
    MessageBoxW(parent, L"已恢复自动探测，稍后自动刷新。", L"Token-Pet", MB_OK | MB_ICONINFORMATION);
}

static const wchar_t *TAB_NAMES[] = {
    L"会话记录", L"按工具", L"每日修为", L"数据源", L"来源覆盖"
};
#define TAB_COUNT 5

struct ColDef { const wchar_t *name; int width; };
static const struct ColDef COLS[TAB_COUNT][7] = {
    { {L"工具", 80}, {L"标题", 300}, {L"模型", 160}, {L"目录", 160}, {L"最后活动", 130}, {L"修为", 110}, {L"灵石($)", 90} },
    { {L"工具", 100}, {L"条目数", 100}, {L"修为", 160}, {L"灵石($)", 120} },
    { {L"日期", 120}, {L"工具", 100}, {L"修为", 140}, {L"灵石($)", 120} },
    { {L"工具", 90}, {L"位置", 140}, {L"路径", 420}, {L"状态", 80}, {L"启用", 60} },
    { {L"工具", 90}, {L"来源类型", 130}, {L"条目数", 90}, {L"修为", 160}, {L"灵石($)", 110} }
};
static const int COL_COUNT[TAB_COUNT] = { 7, 4, 4, 5, 5 };

static void fmt_big(wchar_t *out, size_t cap, int64_t v) {
    char b[64];
    if (v < 10000) snprintf(b, sizeof(b), "%lld", (long long)v);
    else if (v < 100000000LL) snprintf(b, sizeof(b), "%.1f万", (double)v / 10000.0);
    else if (v < 1000000000000LL) snprintf(b, sizeof(b), "%.2f亿", (double)v / 100000000.0);
    else snprintf(b, sizeof(b), "%.2f万亿", (double)v / 1000000000000.0);
    wchar_t *w = tp_utf8_to_wide(b);
    wcsncpy(out, w, cap - 1);
    out[cap - 1] = 0;
    free(w);
}

static void wset(wchar_t *dst, size_t cap, const char *utf8) {
    wchar_t *w = tp_utf8_to_wide(utf8 ? utf8 : "");
    wcsncpy(dst, w, cap - 1);
    dst[cap - 1] = 0;
    free(w);
}

static void setup_columns(void) {
    ListView_DeleteAllItems(g_list);
    while (ListView_DeleteColumn(g_list, 0)) {}
    for (int i = 0; i < COL_COUNT[g_cur_tab]; i++) {
        LVCOLUMNW c;
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = (LPWSTR)COLS[g_cur_tab][i].name;
        c.cx = COLS[g_cur_tab][i].width;
        c.iSubItem = i;
        ListView_InsertColumn(g_list, i, &c);
    }
}

static void add_row(int cols, wchar_t cells[][512]) {
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = ListView_GetItemCount(g_list);
    it.pszText = cells[0];
    int idx = ListView_InsertItem(g_list, &it);
    for (int i = 1; i < cols; i++) {
        ListView_SetItemText(g_list, idx, i, cells[i]);
    }
}

static void exec_rows(Db *db, const char *sql, int cols, int *col_kinds, int n_cols_kinds) {
    db_lock(db);
    sqlite3 *raw = (sqlite3 *)db_raw(db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            wchar_t cells[8][512];
            memset(cells, 0, sizeof(cells));
            for (int i = 0; i < cols && i < 8; i++) {
                int kind = (i < n_cols_kinds) ? col_kinds[i] : 0;
                if (kind == 0) {
                    const char *s = (const char *)sqlite3_column_text(st, i);
                    wset(cells[i], 512, s);
                } else if (kind == 1) {
                    fmt_big(cells[i], 512, sqlite3_column_int64(st, i));
                } else if (kind == 2) {
                    swprintf(cells[i], 512, L"%.2f", sqlite3_column_double(st, i));
                } else if (kind == 3) {
                    wset(cells[i], 512, sqlite3_column_int64(st, i) ? "是" : "否");
                }
            }
            add_row(cols, cells);
        }
        sqlite3_finalize(st);
    }
    db_unlock(db);
}

static void panel_fill(HWND h) {
    (void)h;
    setup_columns();
    if (!g_panel_db) return;
    switch (g_cur_tab) {
    case 0: {
        int kinds[] = { 0, 0, 0, 0, 0, 1, 2 };
        exec_rows(g_panel_db,
            "SELECT i.tool, COALESCE(m.title,''), COALESCE(m.model,''), COALESCE(m.dir,''),"
            " COALESCE(datetime(MAX(i.ts)/1000,'unixepoch','localtime'),''),"
            " COALESCE(SUM(i.tin+i.tout+i.tr+i.cr+i.cw),0), COALESCE(SUM(i.cost),0)"
            " FROM items i LEFT JOIN meta m ON m.tool=i.tool AND m.sid=i.sid"
            " GROUP BY i.tool, i.sid ORDER BY 6 DESC LIMIT 500",
            7, kinds, 7);
        break;
    }
    case 1: {
        int kinds[] = { 0, 1, 1, 2 };
        exec_rows(g_panel_db,
            "SELECT tool, COUNT(*), COALESCE(SUM(tin+tout+tr+cr+cw),0), COALESCE(SUM(cost),0)"
            " FROM items GROUP BY tool ORDER BY 3 DESC",
            4, kinds, 4);
        break;
    }
    case 2: {
        int kinds[] = { 0, 0, 1, 2 };
        exec_rows(g_panel_db,
            "SELECT date(ts/1000,'unixepoch','localtime') AS d, tool,"
            " COALESCE(SUM(tin+tout+tr+cr+cw),0), COALESCE(SUM(cost),0)"
            " FROM items GROUP BY d, tool ORDER BY d DESC LIMIT 200",
            4, kinds, 4);
        break;
    }
    case 3: {
        int kinds[] = { 0, 0, 0, 0, 3 };
        exec_rows(g_panel_db,
            "SELECT tool, label, path, COALESCE(status,''), enabled FROM sources ORDER BY tool, label",
            5, kinds, 5);
        break;
    }
    case 4: {
        int kinds[] = { 0, 0, 1, 1, 2 };
        exec_rows(g_panel_db,
            "SELECT tool,"
            " CASE WHEN key LIKE 'ccr:%' THEN 'cc-switch补录'"
            "      WHEN key LIKE 'ccp:%' THEN 'cc-switch代理'"
            "      WHEN substr(key,1,2)='oc' THEN 'opencode会话'"
            "      WHEN substr(key,1,2)='cx' THEN 'codex会话'"
            "      WHEN substr(key,1,2)='cl' THEN 'claude会话'"
            "      WHEN substr(key,1,2)='km' THEN 'kimi会话'"
            "      ELSE substr(key,1,2) END AS src_kind,"
            " COUNT(*), COALESCE(SUM(tin+tout+tr+cr+cw),0), COALESCE(SUM(cost),0)"
            " FROM items GROUP BY tool, src_kind ORDER BY tool, 4 DESC",
            5, kinds, 5);
        break;
    }
    }
}

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | TCS_TABS,
                                 10, 10, 100, 100, h, NULL, NULL, NULL);
        for (int i = 0; i < TAB_COUNT; i++) {
            TCITEMW ti;
            memset(&ti, 0, sizeof(ti));
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)TAB_NAMES[i];
            TabCtrl_InsertItem(g_tabs, i, &ti);
        }
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 10, 10, 100, 100, h, (HMENU)1, NULL, NULL);
        g_btn_edit = CreateWindowExW(0, L"BUTTON", L"修改选中位置",
                                     WS_CHILD | WS_VISIBLE, 14, 400, 110, 32, h, (HMENU)ID_BTN_EDIT, NULL, NULL);
        g_btn_reset = CreateWindowExW(0, L"BUTTON", L"恢复自动探测",
                                      WS_CHILD | WS_VISIBLE, 130, 400, 110, 32, h, (HMENU)ID_BTN_RESET, NULL, NULL);
        g_btn_scan = CreateWindowExW(0, L"BUTTON", L"重新扫描全部",
                                     WS_CHILD | WS_VISIBLE, 246, 400, 140, 32, h, (HMENU)ID_BTN_SCAN, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        SetTimer(h, PANEL_TIMER, 5000, NULL);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), hh = HIWORD(lp);
        MoveWindow(g_tabs, 8, 8, w - 16, hh - 16, TRUE);
        RECT rc;
        TabCtrl_GetItemRect(g_tabs, 0, &rc);
        int top = rc.bottom + 8;
        int btn_h = 32;
        MoveWindow(g_btn_edit, 14, hh - btn_h - 12, 110, btn_h, TRUE);
        MoveWindow(g_btn_reset, 130, hh - btn_h - 12, 110, btn_h, TRUE);
        MoveWindow(g_btn_scan, 246, hh - btn_h - 12, 140, btn_h, TRUE);
        MoveWindow(g_list, 14, top, w - 28, hh - top - btn_h - 20, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_BTN_EDIT) open_edit_dialog(h);
        else if (id == ID_BTN_RESET) reset_selected(h);
        else if (id == ID_BTN_SCAN) {
            if (MessageBoxW(h, L"将清空本地统计并从所有数据源重新扫描，确认？", L"Token-Pet",
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                win_scan_reset_all();
                bridge_stop();
                if (g_settings.collector_enabled && g_panel_db)
                    bridge_start(g_panel_db, g_settings.wsl_distro, true);
            }
        }
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->hwndFrom == g_tabs && nh->code == TCN_SELCHANGE) {
            g_cur_tab = TabCtrl_GetCurSel(g_tabs);
            panel_fill(h);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == PANEL_TIMER) panel_fill(h);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, PANEL_TIMER);
        g_panel = NULL;
        g_tabs = NULL;
        g_list = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void panel_class_init(void) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = TP_PANEL_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    done = true;
}

void panel_open(HWND parent, Db *db) {
    g_panel_db = db;
    if (g_panel && IsWindow(g_panel)) {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
        panel_fill(g_panel);
        return;
    }
    panel_class_init();
    g_panel = CreateWindowExW(0, TP_PANEL_CLASS, L"Token-Pet · 修炼日志",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1080, 640,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_panel) return;
    ShowWindow(g_panel, SW_SHOW);
    UpdateWindow(g_panel);
    panel_fill(g_panel);
}
