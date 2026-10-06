#include "chart.h"
#include "sqlite3.h"
#include <math.h>

#define CHART_CLASS L"TokenPetChartClass"
#define MAX_BUCKETS 1200
#define MAX_TOOLS   8
#define MAX_NAME    48
#define MAX_LABEL   20
#define MAX_KEY     24

typedef struct {
    HWND hwnd;
    Db *db;
    int gran;                 /* 0 day, 1 week, 2 month */
    int n_buckets;
    char keys[MAX_BUCKETS][MAX_KEY];
    wchar_t labels[MAX_BUCKETS][MAX_LABEL];
    int64_t tok[MAX_BUCKETS][MAX_TOOLS];
    double cost[MAX_BUCKETS][MAX_TOOLS];
    int n_tools;
    char tool_names[MAX_TOOLS][MAX_NAME];
    wchar_t tool_wide[MAX_TOOLS][MAX_NAME];
    int hover;
    bool tracking;
    bool loaded;
    /* custom range (YYYY-MM-DD); empty = from earliest data / until today */
    bool has_start, has_end;
    char start_day[16], end_day[16];
    /* effective range of the last load, for the date pickers */
    char eff_start[16], eff_end[16];
} Chart;

static Chart g_ch;
static int g_dpi = 96;
static HFONT g_font = NULL;
static int g_font_dpi = 0;

/* ---------- helpers ---------- */

static int pen_w(void) {
    int w = TP_SCALE(1, g_dpi);
    return w < 1 ? 1 : w;
}

static HFONT get_font(void) {
    if (!g_font || g_font_dpi != g_dpi) {
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        g_font = CreateFontW(-TP_SCALE(13, g_dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei UI");
        if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_font_dpi = g_dpi;
    }
    return g_font;
}

static void fill_rect(HDC dc, int l, int t, int r, int b, COLORREF col) {
    if (r <= l || b <= t) return;
    HBRUSH br = CreateSolidBrush(col);
    RECT rc;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void draw_text(HDC dc, int x, int y, const wchar_t *s, COLORREF col) {
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, s, (int)wcslen(s));
}

static void fmt_tok(wchar_t *out, size_t cap, double v) {
    if (v < 10000) swprintf(out, cap, L"%.0f", v);
    else if (v < 1e8) swprintf(out, cap, L"%.1f万", v / 1e4);
    else if (v < 1e12) swprintf(out, cap, L"%.2f亿", v / 1e8);
    else swprintf(out, cap, L"%.2f万亿", v / 1e12);
}

static void fmt_cost(wchar_t *out, size_t cap, double v) {
    if (v < 10000) swprintf(out, cap, L"¥%.2f", v);
    else if (v < 1e8) swprintf(out, cap, L"¥%.2f万", v / 1e4);
    else swprintf(out, cap, L"¥%.2f亿", v / 1e8);
}

static COLORREF tool_color(const char *name, int idx) {
    if (strcmp(name, "codex") == 0) return RGB(0x4E, 0x79, 0xA7);
    if (strcmp(name, "claude") == 0) return RGB(0xF2, 0x8E, 0x2B);
    if (strcmp(name, "opencode") == 0) return RGB(0x59, 0xA1, 0x4F);
    if (strcmp(name, "kimi") == 0) return RGB(0xB0, 0x7A, 0xA1);
    static const COLORREF pal[4] = {
        RGB(0xE1, 0x57, 0x59), RGB(0x76, 0xB7, 0xB2),
        RGB(0xED, 0xC9, 0x48), RGB(0x9C, 0x75, 0x5F)
    };
    return pal[idx & 3];
}

static double nice_max(double m) {
    if (m <= 0) return 1;
    double p = pow(10.0, floor(log10(m)));
    double n = m / p;
    double nice;
    if (n <= 1) nice = 1;
    else if (n <= 2) nice = 2;
    else if (n <= 5) nice = 5;
    else nice = 10;
    return nice * p;
}

/* ---------- bucket generation ---------- */

static int64_t midnight_ms(int y, int m, int d) {
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    st.wYear = (WORD)y;
    st.wMonth = (WORD)m;
    st.wDay = (WORD)d;
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

static bool valid_day(const char *s) {
    if (!s || strlen(s) < 10) return false;
    int y = 0, m = 0, d = 0;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return false;
    return y >= 1970 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

static int64_t day_str_to_ms(const char *s) {
    int y = 1970, m = 1, d = 1;
    sscanf(s, "%d-%d-%d", &y, &m, &d);
    return midnight_ms(y, m, d);
}

static void ms_to_day_str(int64_t ms, char *out, size_t cap) {
    time_t t = (time_t)(ms / 1000);
    struct tm tmv;
    localtime_s(&tmv, &t);
    strftime(out, cap, "%Y-%m-%d", &tmv);
}

static char *agg_min_day(Chart *c) {
    if (!c->db) return NULL;
    char *out = NULL;
    db_lock(c->db);
    sqlite3 *raw = (sqlite3 *)db_raw(c->db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, "SELECT MIN(day) FROM items_agg", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *d = (const char *)sqlite3_column_text(st, 0);
            if (d && d[0]) out = xp_strdup(d);
        }
        sqlite3_finalize(st);
    }
    db_unlock(c->db);
    return out;
}

static void add_bucket(Chart *c, int64_t ms, const char *keyfmt, const char *labfmt) {
    if (c->n_buckets >= MAX_BUCKETS) return;
    time_t t = (time_t)(ms / 1000);
    struct tm tmv;
    localtime_s(&tmv, &t);
    char key[MAX_KEY] = "";
    char lab[MAX_LABEL] = "";
    strftime(key, sizeof(key), keyfmt, &tmv);
    strftime(lab, sizeof(lab), labfmt, &tmv);
    snprintf(c->keys[c->n_buckets], MAX_KEY, "%s", key);
    wchar_t *w = tp_utf8_to_wide(lab);
    wcsncpy(c->labels[c->n_buckets], w, MAX_LABEL - 1);
    c->labels[c->n_buckets][MAX_LABEL - 1] = 0;
    free(w);
    c->n_buckets++;
}

/* range defaults to earliest data .. today; custom bounds override */
static void build_buckets(Chart *c, const char **start_day, const char **end_day) {
    SYSTEMTIME now;
    GetLocalTime(&now);

    if (c->has_end && valid_day(c->end_day))
        snprintf(c->eff_end, sizeof(c->eff_end), "%s", c->end_day);
    else
        snprintf(c->eff_end, sizeof(c->eff_end), "%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);

    if (c->has_start && valid_day(c->start_day)) {
        snprintf(c->eff_start, sizeof(c->eff_start), "%s", c->start_day);
    } else {
        char *min = agg_min_day(c);
        if (min && valid_day(min)) snprintf(c->eff_start, sizeof(c->eff_start), "%s", min);
        else snprintf(c->eff_start, sizeof(c->eff_start), "%s", c->eff_end);
        free(min);
    }
    if (strcmp(c->eff_start, c->eff_end) > 0)
        snprintf(c->eff_start, sizeof(c->eff_start), "%s", c->eff_end);

    int64_t s_ms = day_str_to_ms(c->eff_start);
    int64_t e_ms = day_str_to_ms(c->eff_end) + 86400000LL;
    c->n_buckets = 0;

    if (c->gran == 0) {
        int64_t n = (e_ms - s_ms) / 86400000LL;
        if (n > MAX_BUCKETS) {
            s_ms = e_ms - (int64_t)MAX_BUCKETS * 86400000LL;
            ms_to_day_str(s_ms, c->eff_start, sizeof(c->eff_start));
        }
        for (int64_t t = s_ms; t < e_ms; t += 86400000LL)
            add_bucket(c, t, "%Y-%m-%d", "%m-%d");
    } else if (c->gran == 1) {
        time_t t0 = (time_t)(s_ms / 1000);
        struct tm tv;
        localtime_s(&tv, &t0);
        int wd = (tv.tm_wday + 6) % 7;                 /* days since Monday */
        int64_t monday = s_ms - (int64_t)wd * 86400000LL;
        for (int64_t t = monday; t < e_ms; t += 7LL * 86400000LL)
            add_bucket(c, t, "%Y-%m-%d", "%m-%d");
    } else {
        time_t t0 = (time_t)(s_ms / 1000);
        struct tm tv;
        localtime_s(&tv, &t0);
        int y = tv.tm_year + 1900, m = tv.tm_mon + 1;
        int64_t t = midnight_ms(y, m, 1);
        while (t < e_ms && c->n_buckets < MAX_BUCKETS) {
            time_t tt = (time_t)(t / 1000);
            struct tm tv2;
            localtime_s(&tv2, &tt);
            char key[MAX_KEY] = "";
            char lab[MAX_LABEL] = "";
            strftime(key, sizeof(key), "%Y-%m", &tv2);
            strftime(lab, sizeof(lab), "%y-%m", &tv2);
            snprintf(c->keys[c->n_buckets], MAX_KEY, "%s", key);
            wchar_t *w = tp_utf8_to_wide(lab);
            wcsncpy(c->labels[c->n_buckets], w, MAX_LABEL - 1);
            c->labels[c->n_buckets][MAX_LABEL - 1] = 0;
            free(w);
            c->n_buckets++;
            m++;
            if (m > 12) { m = 1; y++; }
            t = midnight_ms(y, m, 1);
        }
    }
    *start_day = c->eff_start;
    *end_day = c->eff_end;
}

static int find_bucket(Chart *c, const char *key) {
    if (!key) return -1;
    for (int i = 0; i < c->n_buckets; i++)
        if (strcmp(c->keys[i], key) == 0) return i;
    return -1;
}

static int find_tool(Chart *c, const char *name) {
    for (int i = 0; i < c->n_tools; i++)
        if (strcmp(c->tool_names[i], name) == 0) return i;
    if (c->n_tools < MAX_TOOLS - 1) {
        snprintf(c->tool_names[c->n_tools], MAX_NAME, "%s", name);
        wchar_t *w = tp_utf8_to_wide(c->tool_names[c->n_tools]);
        wcsncpy(c->tool_wide[c->n_tools], w, MAX_NAME - 1);
        c->tool_wide[c->n_tools][MAX_NAME - 1] = 0;
        free(w);
        return c->n_tools++;
    }
    if (c->n_tools == MAX_TOOLS - 1) {
        snprintf(c->tool_names[c->n_tools], MAX_NAME, "%s", "其他");
        wcsncpy(c->tool_wide[c->n_tools], L"其他", MAX_NAME - 1);
        c->tool_wide[c->n_tools][MAX_NAME - 1] = 0;
        c->n_tools++;
    }
    return MAX_TOOLS - 1;
}

static void load_data(Chart *c) {
    int64_t t_load0 = tp_now_ms();
    c->n_tools = 0;
    memset(c->tok, 0, sizeof(c->tok));
    memset(c->cost, 0, sizeof(c->cost));
    c->loaded = false;

    const char *sd = NULL, *ed = NULL;
    build_buckets(c, &sd, &ed);
    if (!c->db) { c->loaded = true; return; }

    const char *bexpr;
    if (c->gran == 0) {
        bexpr = "a.day";
    } else if (c->gran == 1) {
        bexpr = "date(a.day,'-'||((CAST(strftime('%w',a.day) AS INTEGER)+6)%7)||' days')";
    } else {
        bexpr = "substr(a.day,1,7)";
    }

    char sql[1200];
    snprintf(sql, sizeof(sql),
        "SELECT %s AS b, a.tool,"
        " COALESCE(SUM(a.tin+a.tout+a.tr+a.cr+a.cw),0),"
        " COALESCE(SUM(cost_cny(COALESCE(m.model,''), a.tool, a.tin, a.tout, a.tr, a.cr, a.cw)),0)"
        " FROM items_agg a LEFT JOIN meta m ON m.tool=a.tool AND m.sid=a.sid"
        " WHERE a.day >= '%s' AND a.day <= '%s' GROUP BY b, a.tool",
        bexpr, sd, ed);

    db_lock(c->db);
    sqlite3 *raw = (sqlite3 *)db_raw(c->db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *b = (const char *)sqlite3_column_text(st, 0);
            const char *tool = (const char *)sqlite3_column_text(st, 1);
            int bi = find_bucket(c, b);
            if (bi < 0) continue;
            int ti = find_tool(c, tool ? tool : "unknown");
            if (ti < 0) continue;
            c->tok[bi][ti] += sqlite3_column_int64(st, 2);
            c->cost[bi][ti] += sqlite3_column_double(st, 3);
        }
        sqlite3_finalize(st);
    }
    db_unlock(c->db);
    c->loaded = true;
    int64_t dt = tp_now_ms() - t_load0;
    if (dt > 200) tp_log("chart: gran %d load %lld ms buckets=%d", c->gran, (long long)dt, c->n_buckets);
}

/* ---------- drawing ---------- */

static void chart_layout(const RECT *rc, RECT *tok, RECT *cost) {
    int w = rc->right, h = rc->bottom;
    int lm = TP_SCALE(72, g_dpi), rm = TP_SCALE(14, g_dpi);
    int tm = TP_SCALE(42, g_dpi), bm = TP_SCALE(26, g_dpi), gap = TP_SCALE(10, g_dpi);
    int mid = tm + (h - tm - bm) / 2;
    tok->left = lm; tok->right = w - rm; tok->top = tm; tok->bottom = mid - gap;
    cost->left = lm; cost->right = w - rm; cost->top = mid + gap; cost->bottom = h - bm;
}

static double plot_max(const Chart *c, bool is_tok) {
    double m = 0;
    for (int i = 0; i < c->n_buckets; i++)
        for (int j = 0; j < c->n_tools; j++) {
            double v = is_tok ? (double)c->tok[i][j] : c->cost[i][j];
            if (v > m) m = v;
        }
    return m;
}

static void draw_grid(HDC dc, const RECT *p, double vmax, bool is_tok) {
    int pw = pen_w();
    HPEN pen = CreatePen(PS_SOLID, pw, RGB(226, 226, 226));
    HGDIOBJ oldpen = SelectObject(dc, pen);
    for (int i = 0; i <= 4; i++) {
        int y = p->bottom - (int)((p->bottom - p->top) * i / 4.0);
        MoveToEx(dc, p->left, y, NULL);
        LineTo(dc, p->right, y);
    }
    SelectObject(dc, oldpen);
    DeleteObject(pen);

    int off = TP_SCALE(6, g_dpi);
    for (int i = 0; i <= 4; i++) {
        double v = vmax * i / 4.0;
        wchar_t buf[32];
        if (is_tok) fmt_tok(buf, 32, v);
        else fmt_cost(buf, 32, v);
        SIZE sz;
        GetTextExtentPoint32W(dc, buf, (int)wcslen(buf), &sz);
        int y = p->bottom - (int)((p->bottom - p->top) * i / 4.0) - sz.cy / 2;
        draw_text(dc, p->left - off - sz.cx, y, buf, RGB(120, 120, 120));
    }

    HPEN bpen = CreatePen(PS_SOLID, pw, RGB(170, 170, 170));
    oldpen = SelectObject(dc, bpen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, p->left, p->top, p->right, p->bottom);
    SelectObject(dc, ob);
    SelectObject(dc, oldpen);
    DeleteObject(bpen);
}

static void draw_bars(HDC dc, const RECT *p, const Chart *c, bool is_tok, double vmax, int hover) {
    if (vmax <= 0 || c->n_buckets <= 0) return;
    double slot = (double)(p->right - p->left) / c->n_buckets;
    double bw = slot * 0.68;
    if (bw < 2) bw = 2;
    for (int i = 0; i < c->n_buckets; i++) {
        int x0 = p->left + (int)(slot * i + (slot - bw) / 2.0);
        int x1 = x0 + (int)(bw + 0.5);
        if (x1 <= x0) x1 = x0 + 1;
        int ybottom = p->bottom;
        for (int j = 0; j < c->n_tools; j++) {
            double v = is_tok ? (double)c->tok[i][j] : c->cost[i][j];
            if (v <= 0) continue;
            int hgt = (int)((p->bottom - p->top) * (v / vmax));
            if (hgt < 1) hgt = 1;
            int ytop = ybottom - hgt;
            if (ytop < p->top) ytop = p->top;
            fill_rect(dc, x0, ytop, x1, ybottom, tool_color(c->tool_names[j], j));
            ybottom = ytop;
        }
        if (i == hover) {
            int pad = TP_SCALE(2, g_dpi);
            if (pad < 1) pad = 1;
            HPEN pen = CreatePen(PS_SOLID, pen_w(), RGB(70, 70, 70));
            HGDIOBJ old = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, x0 - pad, p->top, x1 + pad, p->bottom + pen_w());
            SelectObject(dc, ob);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
    }
}

static void draw_legend(HDC dc, const Chart *c, int x, int y) {
    int sw = TP_SCALE(10, g_dpi);
    if (sw < 4) sw = 4;
    int gap = TP_SCALE(4, g_dpi);
    int adv = TP_SCALE(18, g_dpi);
    for (int j = 0; j < c->n_tools; j++) {
        fill_rect(dc, x, y + gap, x + sw, y + sw + gap, tool_color(c->tool_names[j], j));
        draw_text(dc, x + sw + gap, y, c->tool_wide[j], RGB(70, 70, 70));
        SIZE sz;
        GetTextExtentPoint32W(dc, c->tool_wide[j], (int)wcslen(c->tool_wide[j]), &sz);
        x += sw + gap + sz.cx + adv;
    }
}

static void draw_xlabels(HDC dc, const Chart *c, const RECT *cost_plot) {
    if (c->n_buckets <= 0) return;
    double slot = (double)(cost_plot->right - cost_plot->left) / c->n_buckets;
    int min_slot = TP_SCALE(36, g_dpi);
    int step = 1;
    while (slot * step < min_slot) step++;
    int yoff = TP_SCALE(5, g_dpi);
    for (int i = 0; i < c->n_buckets; i += step) {
        SIZE sz;
        GetTextExtentPoint32W(dc, c->labels[i], (int)wcslen(c->labels[i]), &sz);
        int x = cost_plot->left + (int)(slot * i + (slot - sz.cx) / 2.0);
        draw_text(dc, x, cost_plot->bottom + yoff, c->labels[i], RGB(90, 90, 90));
    }
}

static void draw_hover(HDC dc, const Chart *c, const RECT *rc, int b) {
    if (b < 0 || b >= c->n_buckets) return;
    wchar_t lines[MAX_TOOLS + 4][80];
    int n = 0;
    swprintf(lines[n++], 80, L"%ls", c->labels[b]);
    double tt = 0, tc = 0;
    for (int j = 0; j < c->n_tools; j++) { tt += (double)c->tok[b][j]; tc += c->cost[b][j]; }
    wchar_t t1[32], t2[32];
    fmt_tok(t1, 32, tt);
    fmt_cost(t2, 32, tc);
    swprintf(lines[n++], 80, L"Token: %ls", t1);
    swprintf(lines[n++], 80, L"花费: %ls", t2);
    for (int j = 0; j < c->n_tools && n < MAX_TOOLS + 4; j++) {
        wchar_t a[32], b2[32];
        fmt_tok(a, 32, (double)c->tok[b][j]);
        fmt_cost(b2, 32, c->cost[b][j]);
        swprintf(lines[n++], 80, L"%ls  %ls / %ls", c->tool_wide[j], a, b2);
    }

    int wmax = 0;
    for (int i = 0; i < n; i++) {
        SIZE sz;
        GetTextExtentPoint32W(dc, lines[i], (int)wcslen(lines[i]), &sz);
        if (sz.cx > wmax) wmax = sz.cx;
    }
    int padx = TP_SCALE(9, g_dpi);
    int lh = TP_SCALE(17, g_dpi);
    int bw = wmax + padx * 2;
    int bh = n * lh + TP_SCALE(12, g_dpi);
    int x0 = rc->right - bw - TP_SCALE(18, g_dpi);
    int y0 = TP_SCALE(30, g_dpi);
    int xmin = TP_SCALE(80, g_dpi);
    if (x0 < xmin) x0 = xmin;
    fill_rect(dc, x0, y0, x0 + bw, y0 + bh, RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, pen_w(), RGB(150, 150, 150));
    HGDIOBJ old = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x0, y0, x0 + bw, y0 + bh);
    SelectObject(dc, ob);
    SelectObject(dc, old);
    DeleteObject(pen);
    for (int i = 0; i < n; i++)
        draw_text(dc, x0 + padx, y0 + TP_SCALE(6, g_dpi) + i * lh, lines[i],
                  i == 0 ? RGB(40, 40, 40) : RGB(80, 80, 80));
}

static void draw_all(HDC dc, const RECT *rc, Chart *c) {
    fill_rect(dc, 0, 0, rc->right, rc->bottom, RGB(255, 255, 255));
    HGDIOBJ oldf = SelectObject(dc, get_font());
    SetBkMode(dc, TRANSPARENT);

    RECT tokp, costp;
    chart_layout(rc, &tokp, &costp);
    draw_legend(dc, c, TP_SCALE(74, g_dpi), TP_SCALE(4, g_dpi));

    if (!c->loaded || c->n_buckets == 0) {
        const wchar_t *msg = L"暂无数据";
        SIZE sz;
        GetTextExtentPoint32W(dc, msg, (int)wcslen(msg), &sz);
        draw_text(dc, (rc->right - sz.cx) / 2, (rc->bottom - sz.cy) / 2, msg, RGB(140, 140, 140));
        SelectObject(dc, oldf);
        return;
    }

    double tmax = plot_max(c, true);
    double cmax = plot_max(c, false);
    double tn = nice_max(tmax);
    double cn = nice_max(cmax);
    draw_grid(dc, &tokp, tn, true);
    draw_grid(dc, &costp, cn, false);
    if (tmax <= 0 && cmax <= 0) {
        const wchar_t *msg = L"暂无数据或未命中价目表";
        SIZE sz;
        GetTextExtentPoint32W(dc, msg, (int)wcslen(msg), &sz);
        draw_text(dc, (rc->right - sz.cx) / 2, (rc->bottom - sz.cy) / 2, msg, RGB(140, 140, 140));
    } else {
        draw_bars(dc, &tokp, c, true, tn, c->hover);
        draw_bars(dc, &costp, c, false, cn, c->hover);
    }
    draw_xlabels(dc, c, &costp);
    draw_hover(dc, c, rc, c->hover);
    SelectObject(dc, oldf);
}

/* ---------- window ---------- */

static int bucket_at(Chart *c, int x, int y, const RECT *rc) {
    if (c->n_buckets <= 0) return -1;
    RECT tokp, costp;
    chart_layout(rc, &tokp, &costp);
    if (x < tokp.left || x >= tokp.right) return -1;
    if (y < tokp.top || y > costp.bottom) return -1;
    double slot = (double)(tokp.right - tokp.left) / c->n_buckets;
    if (slot <= 0) return -1;
    int i = (int)((x - tokp.left) / slot);
    if (i < 0) i = 0;
    if (i >= c->n_buckets) i = c->n_buckets - 1;
    return i;
}

static LRESULT CALLBACK chart_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp;
    switch (msg) {
    case WM_CREATE:
        g_ch.hwnd = h;
        g_dpi = tp_dpi_for_window(h);
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        if (g_dpi <= 0) g_dpi = 96;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            draw_all(mem, &rc, &g_ch);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        RECT rc;
        GetClientRect(h, &rc);
        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);
        int b = bucket_at(&g_ch, x, y, &rc);
        if (b != g_ch.hover) {
            g_ch.hover = b;
            InvalidateRect(h, NULL, FALSE);
        }
        if (!g_ch.tracking) {
            TRACKMOUSEEVENT tme;
            memset(&tme, 0, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
            g_ch.tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_ch.tracking = false;
        if (g_ch.hover != -1) {
            g_ch.hover = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND chart_create(HWND parent, Db *db) {
    memset(&g_ch, 0, sizeof(g_ch));
    g_ch.db = db;
    g_ch.gran = 0;
    g_ch.hover = -1;

    static bool cls_done = false;
    if (!cls_done) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = chart_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = CHART_CLASS;
        RegisterClassExW(&wc);
        cls_done = true;
    }
    return CreateWindowExW(0, CHART_CLASS, L"", WS_CHILD,
                           0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), NULL);
}

void chart_reload(HWND chart) {
    if (!chart || chart != g_ch.hwnd) return;
    load_data(&g_ch);
    g_ch.hover = -1;
    InvalidateRect(chart, NULL, FALSE);
}

void chart_set_granularity(HWND chart, int gran) {
    if (!chart || chart != g_ch.hwnd) return;
    if (gran < 0 || gran > 2) return;
    g_ch.gran = gran;
    chart_reload(chart);
}

void chart_set_range(HWND chart, const char *start_day, const char *end_day) {
    if (!chart || chart != g_ch.hwnd) return;
    g_ch.has_start = valid_day(start_day);
    g_ch.has_end = valid_day(end_day);
    if (g_ch.has_start) snprintf(g_ch.start_day, sizeof(g_ch.start_day), "%s", start_day);
    if (g_ch.has_end) snprintf(g_ch.end_day, sizeof(g_ch.end_day), "%s", end_day);
    chart_reload(chart);
}

void chart_clear_range(HWND chart) {
    if (!chart || chart != g_ch.hwnd) return;
    g_ch.has_start = false;
    g_ch.has_end = false;
    chart_reload(chart);
}

void chart_get_range(HWND chart, char *start_day, size_t scap, char *end_day, size_t ecap) {
    if (chart != g_ch.hwnd) return;
    if (start_day && scap) snprintf(start_day, scap, "%s", g_ch.eff_start);
    if (end_day && ecap) snprintf(end_day, ecap, "%s", g_ch.eff_end);
}
