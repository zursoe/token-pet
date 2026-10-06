#ifndef TOKENPET_COMMON_H
#define TOKENPET_COMMON_H

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

#define TP_APP_NAME    L"Token-Pet"
#define TP_APP_CLASS   L"TokenPetWindowClass"
#define TP_PANEL_CLASS L"TokenPetPanelClass"
#define TP_MUTEX_NAME  L"TokenPet_SingleInstance_9f3c"
#define TP_VERSION     L"0.4.0"

/* max lengths */
#define TP_PATH_MAX 1024
#define TP_TEXT_MAX 512

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- util ---------- */
void  *xp_alloc(size_t n);
void  *xp_realloc(void *p, size_t n);
char  *xp_strdup(const char *s);
wchar_t *xp_wcsdup(const wchar_t *s);

int64_t tp_now_ms(void);

/* DPI of the monitor that hosts hwnd (96 = 100%) */
int tp_dpi_for_window(HWND hwnd);
#define TP_SCALE(v, dpi) MulDiv((v), (dpi), 96)

/* UTF-8 <-> UTF-16 */
wchar_t *tp_utf8_to_wide(const char *s);
char    *tp_wide_to_utf8(const wchar_t *s);

/* paths */
void tp_exe_dir(wchar_t *out, size_t cap);
void tp_path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b);
bool tp_file_exists_utf8(const char *path);
bool tp_dir_exists_utf8(const char *path);
int64_t tp_file_size_utf8(const char *path);
int64_t tp_file_mtime_utf8(const char *path);
bool tp_mkdir_utf8(const char *path);

/* file helpers */
char *tp_read_file_utf8(const char *path, size_t *out_len); /* malloc'd, NUL-terminated */
bool  tp_write_file_utf8(const char *path, const char *data, size_t len);

/* string helpers */
void  tp_trim(char *s);
bool  tp_starts_with(const char *s, const char *prefix);
bool  tp_ends_with(const char *s, const char *suffix);
void  tp_snprintf(char *dst, size_t cap, const char *fmt, ...);
int64_t tp_parse_i64(const char *s, int64_t dflt);
double  tp_parse_f64(const char *s, double dflt);

/* logging */
void tp_log(const char *fmt, ...);

/* settings (config/settings.json in exe dir) */
typedef struct {
    float   scale;
    int     x, y;             /* window pos */
    bool    has_pos;
    bool    topmost;
    bool    click_through;
    bool    autostart;
    int     poll_ms;
    bool    collector_enabled;
    wchar_t wsl_distro[64];
} TpSettings;

extern TpSettings g_settings;
void tp_settings_load(void);
void tp_settings_save(void);
void tp_settings_path(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
