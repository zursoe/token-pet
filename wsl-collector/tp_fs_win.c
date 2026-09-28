#include "tp_util.h"

static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    else w[0] = 0;
    return w;
}

FILE *tp_fopen(const char *path, const char *mode) {
    wchar_t *wp = to_wide(path);
    wchar_t wm[16];
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16);
    FILE *f = _wfopen(wp, wm);
    free(wp);
    return f;
}

int tp_fseek(FILE *f, int64_t off) {
    return _fseeki64(f, off, SEEK_SET);
}

int64_t tp_ftell(FILE *f) {
    return _ftelli64(f);
}

bool tp_file_exists(const char *p) {
    wchar_t *w = to_wide(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool tp_dir_exists(const char *p) {
    wchar_t *w = to_wide(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t tp_file_size(const char *p) {
    wchar_t *w = to_wide(p);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fad);
    free(w);
    if (!ok) return -1;
    ULARGE_INTEGER u;
    u.LowPart = fad.nFileSizeLow;
    u.HighPart = fad.nFileSizeHigh;
    return (int64_t)u.QuadPart;
}

void tp_list_files(const char *dir, const char *suffix, int max_depth, tp_file_cb cb, void *ud) {
    if (max_depth < 0 || !cb) return;
    char pattern[1500];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    wchar_t *wp = to_wide(pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char nameU[1200];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nameU, sizeof(nameU), NULL, NULL);
        char p[1500];
        tpu_join(p, sizeof(p), dir, nameU);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            tp_list_files(p, suffix, max_depth - 1, cb, ud);
        } else {
            size_t pl = strlen(p), sl = suffix ? strlen(suffix) : 0;
            if (!suffix || (pl >= sl && strcmp(p + pl - sl, suffix) == 0)) {
                cb(p, ud);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
