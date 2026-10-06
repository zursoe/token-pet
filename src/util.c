#include "common.h"
#include <stdarg.h>

void *xp_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        /* last resort: abort cleanly */
        ExitProcess(1);
    }
    return p;
}

void *xp_realloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) ExitProcess(1);
    return q;
}

char *xp_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)xp_alloc(n);
    memcpy(d, s, n);
    return d;
}

wchar_t *xp_wcsdup(const wchar_t *s) {
    if (!s) return NULL;
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = (wchar_t *)xp_alloc(n);
    memcpy(d, s, n);
    return d;
}

int64_t tp_now_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100ns since 1601 -> ms since 1970 */
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

int tp_dpi_for_window(HWND hwnd) {
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = NULL;
    static bool tried = false;
    if (!tried) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) fn = (GetDpiForWindowFn)(void *)GetProcAddress(user32, "GetDpiForWindow");
        tried = true;
    }
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d > 0) return (int)d;
    }
    HDC dc = hwnd ? GetDC(hwnd) : NULL;
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi > 0 ? dpi : 96;
}

wchar_t *tp_utf8_to_wide(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return xp_wcsdup(L"");
    wchar_t *w = (wchar_t *)xp_alloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *tp_wide_to_utf8(const wchar_t *s) {
    if (!s) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return xp_strdup("");
    char *c = (char *)xp_alloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, c, n, NULL, NULL);
    return c;
}

void tp_exe_dir(wchar_t *out, size_t cap) {
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) { wcsncpy(out, L".", cap - 1); out[cap - 1] = 0; return; }
    for (DWORD i = n; i > 0; i--) {
        if (out[i - 1] == L'\\' || out[i - 1] == L'/') { out[i - 1] = 0; break; }
    }
}

void tp_path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b) {
    if (!b || !*b) { wcsncpy(out, a, cap - 1); out[cap - 1] = 0; return; }
    size_t al = wcslen(a);
    bool need_sep = al > 0 && a[al - 1] != L'\\' && a[al - 1] != L'/';
    _snwprintf(out, cap, L"%s%s%s", a, need_sep ? L"\\" : L"", b);
    out[cap - 1] = 0;
}

bool tp_file_exists_utf8(const char *path) {
    wchar_t *w = tp_utf8_to_wide(path);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool tp_dir_exists_utf8(const char *path) {
    wchar_t *w = tp_utf8_to_wide(path);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool stat_utf8(const char *path, WIN32_FILE_ATTRIBUTE_DATA *fad) {
    wchar_t *w = tp_utf8_to_wide(path);
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, fad);
    free(w);
    return ok ? true : false;
}

int64_t tp_file_size_utf8(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!stat_utf8(path, &fad)) return -1;
    ULARGE_INTEGER u; u.LowPart = fad.nFileSizeLow; u.HighPart = fad.nFileSizeHigh;
    return (int64_t)u.QuadPart;
}

int64_t tp_file_mtime_utf8(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!stat_utf8(path, &fad)) return -1;
    ULARGE_INTEGER u; u.LowPart = fad.ftLastWriteTime.dwLowDateTime; u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

bool tp_mkdir_utf8(const char *path) {
    wchar_t *w = tp_utf8_to_wide(path);
    BOOL ok = CreateDirectoryW(w, NULL);
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS) ok = TRUE;
    free(w);
    return ok ? true : false;
}

char *tp_read_file_utf8(const char *path, size_t *out_len) {
    wchar_t *w = tp_utf8_to_wide(path);
    FILE *f = _wfopen(w, L"rb");
    free(w);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)xp_alloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    if (out_len) *out_len = rd;
    return buf;
}

bool tp_write_file_utf8(const char *path, const char *data, size_t len) {
    wchar_t *w = tp_utf8_to_wide(path);
    FILE *f = _wfopen(w, L"wb");
    free(w);
    if (!f) return false;
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    return wr == len;
}

void tp_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') s[--n] = 0;
        else break;
    }
}

bool tp_starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool tp_ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

void tp_snprintf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    dst[cap - 1] = 0;
}

int64_t tp_parse_i64(const char *s, int64_t dflt) {
    if (!s) return dflt;
    char *end = NULL;
    long long v = _strtoi64(s, &end, 10);
    if (end == s) return dflt;
    return (int64_t)v;
}

double tp_parse_f64(const char *s, double dflt) {
    if (!s) return dflt;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return dflt;
    return v;
}

void tp_log(const char *fmt, ...) {
    wchar_t dir[TP_PATH_MAX], path[TP_PATH_MAX];
    tp_exe_dir(dir, TP_PATH_MAX);
    tp_path_join(path, TP_PATH_MAX, dir, L"data");
    char *du = tp_wide_to_utf8(path);
    tp_mkdir_utf8(du);
    free(du);
    tp_path_join(path, TP_PATH_MAX, dir, L"data\\token-pet.log");

    char buf[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;

    char line[2400];
    tp_snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, line, (DWORD)strlen(line), &wr, NULL);
    CloseHandle(h);
}

/* ---------- settings ---------- */

TpSettings g_settings;

void tp_settings_path(char *out, size_t cap) {
    wchar_t dir[TP_PATH_MAX], p[TP_PATH_MAX];
    tp_exe_dir(dir, TP_PATH_MAX);
    tp_path_join(p, TP_PATH_MAX, dir, L"config\\settings.json");
    char *u = tp_wide_to_utf8(p);
    tp_snprintf(out, cap, "%s", u);
    free(u);
}

void tp_settings_load(void) {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.scale = 1.0f;
    g_settings.topmost = true;
    g_settings.poll_ms = 7200000;
    g_settings.collector_enabled = true;
    wcsncpy(g_settings.wsl_distro, L"Debian", 63);

    char path[TP_PATH_MAX];
    tp_settings_path(path, sizeof(path));
    size_t len = 0;
    char *txt = tp_read_file_utf8(path, &len);
    if (!txt) return;

    /* ultra-light JSON value scanner: "key": value pairs (flat object) */
    const char *p;

    if ((p = strstr(txt, "\"scale\""))) g_settings.scale = (float)tp_parse_f64(p + 7, 1.0);
    if ((p = strstr(txt, "\"x\""))) g_settings.x = (int)tp_parse_i64(p + 3, 0);
    if ((p = strstr(txt, "\"y\""))) g_settings.y = (int)tp_parse_i64(p + 3, 0);
    if ((p = strstr(txt, "\"has_pos\""))) g_settings.has_pos = strstr(p, "true") && (strstr(p, "true") < p + 20);
    if ((p = strstr(txt, "\"topmost\""))) g_settings.topmost = strstr(p, "true") && (strstr(p, "true") < p + 20);
    if ((p = strstr(txt, "\"click_through\""))) g_settings.click_through = strstr(p, "true") && (strstr(p, "true") < p + 30);
    if ((p = strstr(txt, "\"autostart\""))) g_settings.autostart = strstr(p, "true") && (strstr(p, "true") < p + 20);
    if ((p = strstr(txt, "\"poll_ms\""))) g_settings.poll_ms = (int)tp_parse_i64(p + 9, 7200000);
    if (g_settings.poll_ms < 60000) g_settings.poll_ms = 7200000; /* migrate old realtime setting */
    if ((p = strstr(txt, "\"collector_enabled\""))) g_settings.collector_enabled = strstr(p, "true") && (strstr(p, "true") < p + 30);
    if ((p = strstr(txt, "\"wsl_distro\""))) {
        const char *q = strchr(p + 12, '"');
        if (q) {
            const char *r = strchr(q + 1, '"');
            if (r && (size_t)(r - q - 1) < 63) {
                char tmp[64];
                memcpy(tmp, q + 1, (size_t)(r - q - 1));
                tmp[r - q - 1] = 0;
                wchar_t *w = tp_utf8_to_wide(tmp);
                wcsncpy(g_settings.wsl_distro, w, 63);
                free(w);
            }
        }
    }
    free(txt);
}

void tp_settings_save(void) {
    char buf[2048];
    tp_snprintf(buf, sizeof(buf),
        "{\n"
        "  \"scale\": %.3f,\n"
        "  \"x\": %d,\n"
        "  \"y\": %d,\n"
        "  \"has_pos\": %s,\n"
        "  \"topmost\": %s,\n"
        "  \"click_through\": %s,\n"
        "  \"autostart\": %s,\n"
        "  \"poll_ms\": %d,\n"
        "  \"collector_enabled\": %s\n"
        "}\n",
        g_settings.scale, g_settings.x, g_settings.y,
        g_settings.has_pos ? "true" : "false",
        g_settings.topmost ? "true" : "false",
        g_settings.click_through ? "true" : "false",
        g_settings.autostart ? "true" : "false",
        g_settings.poll_ms,
        g_settings.collector_enabled ? "true" : "false");

    wchar_t dir[TP_PATH_MAX], p[TP_PATH_MAX];
    tp_exe_dir(dir, TP_PATH_MAX);
    tp_path_join(p, TP_PATH_MAX, dir, L"config");
    char *u = tp_wide_to_utf8(p);
    tp_mkdir_utf8(u);
    free(u);
    tp_path_join(p, TP_PATH_MAX, dir, L"config\\settings.json");
    u = tp_wide_to_utf8(p);
    tp_write_file_utf8(u, buf, strlen(buf));
    free(u);
}
