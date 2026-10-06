#include "common.h"
#include "db.h"
#include "realm.h"
#include "sources.h"
#include "win_scan.h"
#include "bridge.h"
#include "chart.h"
#include "sqlite3.h"

static HWND g_panel = NULL;
static HWND g_tabs = NULL;
static HWND g_list = NULL;
static Db  *g_panel_db = NULL;
static int  g_cur_tab = 0;
static HWND g_btn_edit = NULL, g_btn_reset = NULL, g_btn_scan = NULL, g_btn_refresh = NULL;
static HWND g_chart = NULL;
static HWND g_radios[3] = { NULL, NULL, NULL };
static HWND g_lbl_from = NULL, g_dtp_from = NULL, g_lbl_to = NULL, g_dtp_to = NULL;
static HWND g_btn_chart_reset = NULL;
static bool g_dtp_guard = false;
static int  g_dpi = 96;
static HFONT g_ui_font = NULL;

static void panel_apply_fonts(void) {
    HFONT nf = CreateFontW(-TP_SCALE(13, g_dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Microsoft YaHei UI");
    if (!nf) return;
    HFONT old = g_ui_font;
    g_ui_font = nf;
    HWND hs[] = { g_tabs, g_list, g_btn_edit, g_btn_reset, g_btn_scan, g_btn_refresh,
                  g_radios[0], g_radios[1], g_radios[2],
                  g_lbl_from, g_dtp_from, g_lbl_to, g_dtp_to, g_btn_chart_reset };
    for (int i = 0; i < (int)(sizeof(hs) / sizeof(hs[0])); i++)
        if (hs[i]) SendMessageW(hs[i], WM_SETFONT, (WPARAM)nf, MAKELPARAM(TRUE, 0));
    if (old) DeleteObject(old);
}

/* click-to-sort state for the first three tabs (会话记录/按工具/每日修为) */
static int  g_sort_col[3]  = { 5, 2, 0 };
static bool g_sort_desc[3] = { true, true, true };

#define ID_BTN_EDIT  201
#define ID_BTN_RESET 202
#define ID_BTN_SCAN  203
#define ID_BTN_REFRESH 204
#define ID_RADIO_DAY   205
#define ID_RADIO_WEEK  206
#define ID_RADIO_MONTH 207
#define ID_BTN_CHART_RESET 208
#define ID_DTP_FROM    209
#define ID_DTP_TO      210
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
        HWND st = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"手动指定该数据源位置（留空 = 恢复自动探测）",
                                  WS_CHILD | WS_VISIBLE,
                                  TP_SCALE(12, g_dpi), TP_SCALE(12, g_dpi),
                                  TP_SCALE(520, g_dpi), TP_SCALE(20, g_dpi), h, NULL, NULL, NULL);
        g_dlg_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_dlg_current,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     TP_SCALE(12, g_dpi), TP_SCALE(40, g_dpi),
                                     TP_SCALE(520, g_dpi), TP_SCALE(26, g_dpi),
                                     h, (HMENU)ID_DLG_EDIT, NULL, NULL);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                  TP_SCALE(340, g_dpi), TP_SCALE(80, g_dpi),
                                  TP_SCALE(90, g_dpi), TP_SCALE(30, g_dpi),
                                  h, (HMENU)ID_DLG_OK, NULL, NULL);
        HWND ca = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                                  TP_SCALE(440, g_dpi), TP_SCALE(80, g_dpi),
                                  TP_SCALE(90, g_dpi), TP_SCALE(30, g_dpi),
                                  h, (HMENU)ID_DLG_CANCEL, NULL, NULL);
        if (g_ui_font) {
            HWND hs[] = { st, g_dlg_edit, ok, ca };
            for (int i = 0; i < 4; i++)
                if (hs[i]) SendMessageW(hs[i], WM_SETFONT, (WPARAM)g_ui_font, MAKELPARAM(TRUE, 0));
        }
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
    int w = TP_SCALE(560, g_dpi), h = TP_SCALE(160, g_dpi);
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
    L"会话记录", L"按工具", L"每日修为", L"数据源", L"来源覆盖", L"统计图"
};
#define TAB_COUNT 6
#define TAB_CHART (TAB_COUNT - 1)

struct ColDef { const wchar_t *name; int width; };
static const struct ColDef COLS[TAB_COUNT][7] = {
    { {L"工具", 80}, {L"标题", 300}, {L"模型", 160}, {L"目录", 160}, {L"最后活动", 130}, {L"修为", 110}, {L"花费(¥)", 140} },
    { {L"工具", 100}, {L"条目数", 100}, {L"修为", 160}, {L"花费(¥)", 150} },
    { {L"日期", 120}, {L"工具", 100}, {L"修为", 140}, {L"花费(¥)", 150} },
    { {L"工具", 90}, {L"位置", 140}, {L"路径", 420}, {L"状态", 80}, {L"启用", 60} },
    { {L"工具", 90}, {L"来源类型", 130}, {L"条目数", 90}, {L"修为", 160}, {L"花费(¥)", 150} },
    { {L"", 1} }
};
static const int COL_COUNT[TAB_COUNT] = { 7, 4, 4, 5, 5, 0 };

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
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    while (ListView_DeleteColumn(g_list, 0)) {}
    for (int i = 0; i < COL_COUNT[g_cur_tab]; i++) {
        LVCOLUMNW c;
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = (LPWSTR)COLS[g_cur_tab][i].name;
        c.cx = TP_SCALE(COLS[g_cur_tab][i].width, g_dpi);
        c.iSubItem = i;
        ListView_InsertColumn(g_list, i, &c);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);
}

static bool col_default_desc(int tab, int col) {
    if (tab == 0) return col >= 4;                     /* 最后活动/修为/花费 */
    if (tab == 1) return col >= 1;                     /* 条目数/修为/花费 */
    if (tab == 2) return col == 0 || col >= 2;         /* 日期/修为/花费 */
    return false;
}

static void update_sort_arrows(void) {
    if (!g_list) return;
    HWND hdr = ListView_GetHeader(g_list);
    if (!hdr) return;
    int n = Header_GetItemCount(hdr);
    for (int i = 0; i < n; i++) {
        HDITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = HDI_FORMAT;
        if (!Header_GetItem(hdr, i, &it)) continue;
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (g_cur_tab < 3 && i == g_sort_col[g_cur_tab])
            it.fmt |= g_sort_desc[g_cur_tab] ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(hdr, i, &it);
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
    int64_t tq = tp_now_ms();
    int rows = 0;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    db_lock(db);
    sqlite3 *raw = (sqlite3 *)db_raw(db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            rows++;
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
                } else if (kind == 4) {
                    swprintf(cells[i], 512, L"¥%.2f", sqlite3_column_double(st, i));
                }
            }
            add_row(cols, cells);
        }
        sqlite3_finalize(st);
    }
    db_unlock(db);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);
    int64_t dt = tp_now_ms() - tq;
    if (dt > 200) tp_log("panel: query+fill %lld ms rows=%d", (long long)dt, rows);
}

static void panel_fill(HWND h) {
    (void)h;
    int64_t t0 = tp_now_ms();
    int keep_top = g_list ? ListView_GetTopIndex(g_list) : -1;
    int keep_sel = g_list ? ListView_GetNextItem(g_list, -1, LVNI_SELECTED) : -1;
    setup_columns();
    update_sort_arrows();
    if (!g_panel_db) return;
    switch (g_cur_tab) {
    case 0: {
        int kinds[] = { 0, 0, 0, 0, 0, 1, 4 };
        char sql[1200];
        snprintf(sql, sizeof(sql),
            "SELECT a.tool, COALESCE(m.title,''), COALESCE(m.model,''), COALESCE(m.dir,''),"
            " COALESCE(datetime(MAX(a.last_ts)/1000,'unixepoch','localtime'),''),"
            " COALESCE(SUM(a.tin+a.tout+a.tr+a.cr+a.cw),0),"
            " COALESCE(SUM(cost_cny(COALESCE(m.model,''), a.tool, a.tin, a.tout, a.tr, a.cr, a.cw)),0)"
            " FROM items_agg a LEFT JOIN meta m ON m.tool=a.tool AND m.sid=a.sid"
            " GROUP BY a.tool, a.sid ORDER BY %d %s, 5 DESC",
            g_sort_col[0] + 1, g_sort_desc[0] ? "DESC" : "ASC");
        exec_rows(g_panel_db, sql, 7, kinds, 7);
        break;
    }
    case 1: {
        int kinds[] = { 0, 1, 1, 4 };
        char sql[900];
        snprintf(sql, sizeof(sql),
            "SELECT a.tool, COALESCE(SUM(a.cnt),0), COALESCE(SUM(a.tin+a.tout+a.tr+a.cr+a.cw),0),"
            " COALESCE(SUM(cost_cny(COALESCE(m.model,''), a.tool, a.tin, a.tout, a.tr, a.cr, a.cw)),0)"
            " FROM items_agg a LEFT JOIN meta m ON m.tool=a.tool AND m.sid=a.sid"
            " GROUP BY a.tool ORDER BY %d %s, 1 ASC",
            g_sort_col[1] + 1, g_sort_desc[1] ? "DESC" : "ASC");
        exec_rows(g_panel_db, sql, 4, kinds, 4);
        break;
    }
    case 2: {
        int kinds[] = { 0, 0, 1, 4 };
        char sql[900];
        snprintf(sql, sizeof(sql),
            "SELECT a.day AS d, a.tool,"
            " COALESCE(SUM(a.tin+a.tout+a.tr+a.cr+a.cw),0),"
            " COALESCE(SUM(cost_cny(COALESCE(m.model,''), a.tool, a.tin, a.tout, a.tr, a.cr, a.cw)),0)"
            " FROM items_agg a LEFT JOIN meta m ON m.tool=a.tool AND m.sid=a.sid"
            " GROUP BY d, a.tool ORDER BY %d %s, 2 ASC",
            g_sort_col[2] + 1, g_sort_desc[2] ? "DESC" : "ASC");
        exec_rows(g_panel_db, sql, 4, kinds, 4);
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
        int kinds[] = { 0, 0, 1, 1, 4 };
        exec_rows(g_panel_db,
            "SELECT a.tool,"
            " CASE a.src WHEN 'ccr' THEN 'cc-switch补录'"
            "            WHEN 'ccp' THEN 'cc-switch代理'"
            "            WHEN 'oc' THEN 'opencode会话'"
            "            WHEN 'cx' THEN 'codex会话'"
            "            WHEN 'cl' THEN 'claude会话'"
            "            WHEN 'km' THEN 'kimi会话'"
            "            ELSE a.src END AS src_kind,"
            " COALESCE(SUM(a.cnt),0), COALESCE(SUM(a.tin+a.tout+a.tr+a.cr+a.cw),0),"
            " COALESCE(SUM(cost_cny(COALESCE(m.model,''), a.tool, a.tin, a.tout, a.tr, a.cr, a.cw)),0)"
            " FROM items_agg a LEFT JOIN meta m ON m.tool=a.tool AND m.sid=a.sid"
            " GROUP BY a.tool, src_kind ORDER BY 1, 4 DESC",
            5, kinds, 5);
        break;
    }
    case 5:
        break;
    }
    int64_t dt = tp_now_ms() - t0;
    if (dt > 200) tp_log("panel: tab %d fill %lld ms", g_cur_tab, (long long)dt);
    /* restore scroll / selection after manual refresh */
    int n = ListView_GetItemCount(g_list);
    if (keep_sel >= 0 && keep_sel < n) {
        ListView_SetItemState(g_list, keep_sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, keep_sel, FALSE);
    } else if (keep_top > 0 && keep_top < n) {
        ListView_EnsureVisible(g_list, keep_top, FALSE);
    }
}

static bool parse_day(const char *s, SYSTEMTIME *st) {
    int y = 0, m = 0, d = 0;
    if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return false;
    if (y < 1970 || y > 2100) return false;
    memset(st, 0, sizeof(*st));
    st->wYear = (WORD)y;
    st->wMonth = (WORD)m;
    st->wDay = (WORD)d;
    return true;
}

static void sync_chart_dates(void) {
    if (!g_chart || !g_dtp_from || !g_dtp_to) return;
    char sd[16] = "", ed[16] = "";
    chart_get_range(g_chart, sd, sizeof(sd), ed, sizeof(ed));
    SYSTEMTIME st;
    g_dtp_guard = true;
    if (parse_day(sd, &st)) DateTime_SetSystemtime(g_dtp_from, GDT_VALID, &st);
    if (parse_day(ed, &st)) DateTime_SetSystemtime(g_dtp_to, GDT_VALID, &st);
    g_dtp_guard = false;
}

static void update_tab_ui(void) {
    bool chart = (g_cur_tab == TAB_CHART);
    if (g_list) ShowWindow(g_list, chart ? SW_HIDE : SW_SHOW);
    if (g_btn_edit) ShowWindow(g_btn_edit, chart ? SW_HIDE : SW_SHOW);
    if (g_btn_reset) ShowWindow(g_btn_reset, chart ? SW_HIDE : SW_SHOW);
    if (g_chart) ShowWindow(g_chart, chart ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < 3; i++)
        if (g_radios[i]) ShowWindow(g_radios[i], chart ? SW_SHOW : SW_HIDE);
    HWND extras[] = { g_lbl_from, g_dtp_from, g_lbl_to, g_dtp_to, g_btn_chart_reset };
    for (int i = 0; i < 5; i++)
        if (extras[i]) ShowWindow(extras[i], chart ? SW_SHOW : SW_HIDE);
    if (chart && g_chart) {
        chart_reload(g_chart);
        sync_chart_dates();
    }
}

static void panel_layout(HWND h, int w, int hh) {
    (void)h;
    int m = TP_SCALE(8, g_dpi);
    MoveWindow(g_tabs, m, m, w - 2 * m, hh - 2 * m, TRUE);
    RECT rc;
    TabCtrl_GetItemRect(g_tabs, 0, &rc);
    int top = rc.bottom + TP_SCALE(8, g_dpi);
    int btn_h = TP_SCALE(32, g_dpi);
    int by = hh - btn_h - TP_SCALE(12, g_dpi);
    int x1 = TP_SCALE(14, g_dpi);
    int x2 = x1 + TP_SCALE(116, g_dpi);
    int x3 = x2 + TP_SCALE(116, g_dpi);
    int x4 = x3 + TP_SCALE(150, g_dpi);
    MoveWindow(g_btn_edit, x1, by, TP_SCALE(110, g_dpi), btn_h, TRUE);
    MoveWindow(g_btn_reset, x2, by, TP_SCALE(110, g_dpi), btn_h, TRUE);
    MoveWindow(g_btn_scan, x3, by, TP_SCALE(140, g_dpi), btn_h, TRUE);
    MoveWindow(g_btn_refresh, x4, by, TP_SCALE(90, g_dpi), btn_h, TRUE);
    int lx = TP_SCALE(14, g_dpi);
    int lw = w - TP_SCALE(28, g_dpi);
    int lh = hh - top - btn_h - TP_SCALE(20, g_dpi);
    if (g_list) MoveWindow(g_list, lx, top, lw, lh, TRUE);
    if (g_chart) MoveWindow(g_chart, lx, top, lw, lh, TRUE);
    int rx = TP_SCALE(500, g_dpi);
    int rstep = TP_SCALE(52, g_dpi);
    int rw = TP_SCALE(44, g_dpi);
    for (int i = 0; i < 3; i++)
        if (g_radios[i]) MoveWindow(g_radios[i], rx + i * rstep, by, rw, btn_h, TRUE);

    int cx = TP_SCALE(664, g_dpi);
    int lbl_w = TP_SCALE(26, g_dpi);
    int dw = TP_SCALE(128, g_dpi);
    int gap = TP_SCALE(6, g_dpi);
    int dy = by + TP_SCALE(5, g_dpi);
    int dh = btn_h - TP_SCALE(10, g_dpi);
    if (g_lbl_from) MoveWindow(g_lbl_from, cx, dy, lbl_w, dh, TRUE);
    cx += lbl_w + gap;
    if (g_dtp_from) MoveWindow(g_dtp_from, cx, dy, dw, dh, TRUE);
    cx += dw + TP_SCALE(10, g_dpi);
    if (g_lbl_to) MoveWindow(g_lbl_to, cx, dy, lbl_w, dh, TRUE);
    cx += lbl_w + gap;
    if (g_dtp_to) MoveWindow(g_dtp_to, cx, dy, dw, dh, TRUE);
    cx += dw + TP_SCALE(10, g_dpi);
    if (g_btn_chart_reset) MoveWindow(g_btn_chart_reset, cx, by, TP_SCALE(72, g_dpi), btn_h, TRUE);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_dpi = tp_dpi_for_window(h);
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
        g_btn_refresh = CreateWindowExW(0, L"BUTTON", L"刷新",
                                        WS_CHILD | WS_VISIBLE, 396, 400, 90, 32, h, (HMENU)ID_BTN_REFRESH, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

        g_chart = chart_create(h, g_panel_db);
        const wchar_t *rnames[3] = { L"日", L"周", L"月" };
        for (int i = 0; i < 3; i++) {
            g_radios[i] = CreateWindowExW(0, L"BUTTON", rnames[i],
                                          WS_CHILD | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0),
                                          0, 0, 10, 10, h, (HMENU)(INT_PTR)(ID_RADIO_DAY + i), NULL, NULL);
        }
        if (g_radios[0]) SendMessageW(g_radios[0], BM_SETCHECK, BST_CHECKED, 0);

        g_lbl_from = CreateWindowExW(0, L"STATIC", L"从", WS_CHILD,
                                     0, 0, 10, 10, h, NULL, NULL, NULL);
        g_dtp_from = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"",
                                     WS_CHILD | DTS_SHORTDATEFORMAT,
                                     0, 0, 10, 10, h, (HMENU)ID_DTP_FROM, NULL, NULL);
        g_lbl_to = CreateWindowExW(0, L"STATIC", L"到", WS_CHILD,
                                   0, 0, 10, 10, h, NULL, NULL, NULL);
        g_dtp_to = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"",
                                   WS_CHILD | DTS_SHORTDATEFORMAT,
                                   0, 0, 10, 10, h, (HMENU)ID_DTP_TO, NULL, NULL);
        g_btn_chart_reset = CreateWindowExW(0, L"BUTTON", L"重置", WS_CHILD,
                                            0, 0, 10, 10, h, (HMENU)ID_BTN_CHART_RESET, NULL, NULL);

        panel_apply_fonts();
        return 0;
    }
    case WM_SIZE:
        panel_layout(h, LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        if (g_dpi <= 0) g_dpi = 96;
        panel_apply_fonts();
        RECT rc;
        GetClientRect(h, &rc);
        panel_layout(h, rc.right, rc.bottom);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_BTN_EDIT) open_edit_dialog(h);
        else if (id == ID_BTN_REFRESH) {
            if (g_cur_tab == TAB_CHART) {
                if (g_chart) {
                    chart_reload(g_chart);
                    sync_chart_dates();
                }
            } else {
                panel_fill(h);
            }
        }
        else if (id >= ID_RADIO_DAY && id <= ID_RADIO_MONTH) {
            if (g_chart) chart_set_granularity(g_chart, id - ID_RADIO_DAY);
        }
        else if (id == ID_BTN_CHART_RESET) {
            if (g_chart) chart_clear_range(g_chart);
            sync_chart_dates();
        }
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
            update_tab_ui();
        } else if ((nh->hwndFrom == g_dtp_from || nh->hwndFrom == g_dtp_to) &&
                   nh->code == DTN_DATETIMECHANGE) {
            if (!g_dtp_guard && g_chart) {
                SYSTEMTIME s1, s2;
                char sd[16] = "", ed[16] = "";
                if (g_dtp_from && DateTime_GetSystemtime(g_dtp_from, &s1) == GDT_VALID)
                    snprintf(sd, sizeof(sd), "%04d-%02d-%02d", s1.wYear, s1.wMonth, s1.wDay);
                if (g_dtp_to && DateTime_GetSystemtime(g_dtp_to, &s2) == GDT_VALID)
                    snprintf(ed, sizeof(ed), "%04d-%02d-%02d", s2.wYear, s2.wMonth, s2.wDay);
                if (sd[0] && ed[0]) {
                    chart_set_range(g_chart, sd, ed);
                    tp_log("panel: chart range %s .. %s", sd, ed);
                }
            }
        } else if (nh->hwndFrom == g_list && nh->code == LVN_COLUMNCLICK && g_cur_tab < 3) {
            LPNMLISTVIEW nmlv = (LPNMLISTVIEW)lp;
            int col = nmlv->iSubItem;
            if (col >= 0 && col < COL_COUNT[g_cur_tab]) {
                if (g_sort_col[g_cur_tab] == col) {
                    g_sort_desc[g_cur_tab] = !g_sort_desc[g_cur_tab];
                } else {
                    g_sort_col[g_cur_tab] = col;
                    g_sort_desc[g_cur_tab] = col_default_desc(g_cur_tab, col);
                }
                panel_fill(h);
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_panel = NULL;
        g_tabs = NULL;
        g_list = NULL;
        g_chart = NULL;
        for (int i = 0; i < 3; i++) g_radios[i] = NULL;
        g_lbl_from = g_dtp_from = g_lbl_to = g_dtp_to = NULL;
        g_btn_chart_reset = NULL;
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
    g_dpi = tp_dpi_for_window(parent);
    if (g_panel && IsWindow(g_panel)) {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
        panel_fill(g_panel);
        update_tab_ui();
        return;
    }
    panel_class_init();
    int pw = TP_SCALE(1080, g_dpi), ph = TP_SCALE(640, g_dpi);
    RECT wa;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        int mw = wa.right - wa.left - TP_SCALE(40, g_dpi);
        int mh = wa.bottom - wa.top - TP_SCALE(40, g_dpi);
        if (pw > mw) pw = mw;
        if (ph > mh) ph = mh;
    }
    g_panel = CreateWindowExW(0, TP_PANEL_CLASS, L"Token-Pet · 修炼日志",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, pw, ph,
                              NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_panel) return;
    ShowWindow(g_panel, SW_SHOW);
    UpdateWindow(g_panel);
    panel_fill(g_panel);
    update_tab_ui();
}
