#include "win_scan.h"
#include "scan.h"
#include "realm.h"
#include "sources.h"

extern void scan_ccswitch(Db *db, const char *db_path, int64_t *out_added_xp);

static Db *g_db = NULL;
static wchar_t g_cfg_dir[TP_PATH_MAX];
static TpCursorSet g_cs;
static HANDLE g_trigger = NULL;
static volatile LONG64 g_pending_gain = 0;
static bool g_cursors_loaded = false;

volatile LONG g_startup_cycles_pending = 2;

/* ---------- emit layer (used by shared scanners and the WSL bridge) ---------- */

void emit_hello(void) {}
void emit_paths(void) {}

void emit_item(const char *key, const char *tool, const char *sid, int64_t ts,
               int64_t tin, int64_t tout, int64_t tr, int64_t cr, int64_t cw, double cost) {
    int64_t delta = db_upsert_item(g_db, key, tool, sid, ts, tin, tout, tr, cr, cw, cost, false);
    if (delta != 0) InterlockedExchangeAdd64(&g_pending_gain, delta);
}

void emit_meta(const char *tool, const char *sid, const char *title, const char *model,
               const char *provider, const char *dir, int64_t updated) {
    db_upsert_meta(g_db, tool, sid, title, model, provider, dir, updated);
}

void emit_status(const char *phase, const char *detail, long long done, long long total) {
    (void)phase; (void)detail; (void)done; (void)total;
}

void emit_done(bool scan) {
    (void)scan;
}

/* ---------- path resolution (Windows side) ---------- */

static void get_userprofile(char *out, size_t cap) {
    wchar_t w[TP_PATH_MAX];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", w, TP_PATH_MAX);
    if (n == 0 || n >= TP_PATH_MAX) {
        wchar_t drive[16] = L"", path[TP_PATH_MAX] = L"";
        GetEnvironmentVariableW(L"HOMEDRIVE", drive, 16);
        GetEnvironmentVariableW(L"HOMEPATH", path, TP_PATH_MAX);
        _snwprintf(w, TP_PATH_MAX, L"%s%s", drive, path);
        w[TP_PATH_MAX - 1] = 0;
    }
    char *u = tp_wide_to_utf8(w);
    snprintf(out, cap, "%s", u);
    free(u);
}

typedef struct {
    char codex_dir[TP_PATH_MAX];
    char opencode_db[TP_PATH_MAX];
    char claude_projects[TP_PATH_MAX];
    char ccswitch_db[TP_PATH_MAX];
    char gemini_dir[TP_PATH_MAX];
} WinPaths;

static void resolve_win_paths(WinPaths *p) {
    memset(p, 0, sizeof(*p));
    char home[TP_PATH_MAX];
    get_userprofile(home, sizeof(home));
    tpu_join(p->codex_dir, sizeof(p->codex_dir), home, ".codex");
    tpu_join(p->claude_projects, sizeof(p->claude_projects), home, ".claude");
    tpu_join(p->claude_projects, sizeof(p->claude_projects), p->claude_projects, "projects");
    tpu_join(p->ccswitch_db, sizeof(p->ccswitch_db), home, ".cc-switch\\cc-switch.db");
    tpu_join(p->gemini_dir, sizeof(p->gemini_dir), home, ".gemini");

    char envbuf[TP_PATH_MAX];
    DWORD n = GetEnvironmentVariableA("OPENCODE_DB", envbuf, TP_PATH_MAX);
    if (n > 0 && n < TP_PATH_MAX) {
        snprintf(p->opencode_db, sizeof(p->opencode_db), "%s", envbuf);
    } else {
        n = GetEnvironmentVariableA("XDG_DATA_HOME", envbuf, TP_PATH_MAX);
        if (n > 0 && n < TP_PATH_MAX) {
            tpu_join(p->opencode_db, sizeof(p->opencode_db), envbuf, "opencode/opencode.db");
        } else {
            char base[TP_PATH_MAX];
            tpu_join(base, sizeof(base), home, ".local\\share");
            tpu_join(p->opencode_db, sizeof(p->opencode_db), base, "opencode\\opencode.db");
        }
    }

    /* manual overrides from config\sources.json (empty = auto) */
    char *ov;
    if ((ov = sources_cfg_get("win", "codex"))) { snprintf(p->codex_dir, sizeof(p->codex_dir), "%s", ov); free(ov); }
    if ((ov = sources_cfg_get("win", "opencode"))) { snprintf(p->opencode_db, sizeof(p->opencode_db), "%s", ov); free(ov); }
    if ((ov = sources_cfg_get("win", "claude"))) { snprintf(p->claude_projects, sizeof(p->claude_projects), "%s", ov); free(ov); }
    if ((ov = sources_cfg_get("win", "ccswitch"))) { snprintf(p->ccswitch_db, sizeof(p->ccswitch_db), "%s", ov); free(ov); }
    if ((ov = sources_cfg_get("win", "gemini"))) { snprintf(p->gemini_dir, sizeof(p->gemini_dir), "%s", ov); free(ov); }
}

/* ---------- scanning ---------- */

void win_scan_init(Db *db, const wchar_t *config_dir) {
    g_db = db;
    wcsncpy(g_cfg_dir, config_dir, TP_PATH_MAX - 1);
    g_trigger = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_cursors_loaded) {
        char cursor_path[TP_PATH_MAX];
        char *cd = tp_wide_to_utf8(config_dir);
        tpu_join(cursor_path, sizeof(cursor_path), cd, "cursors_win.json");
        free(cd);
        cursors_load(&g_cs, cursor_path);
        g_cursors_loaded = true;
    }
}

static void save_cursors(void) {
    char cursor_path[TP_PATH_MAX];
    char *cd = tp_wide_to_utf8(g_cfg_dir);
    tpu_join(cursor_path, sizeof(cursor_path), cd, "cursors_win.json");
    free(cd);
    cursors_save(&g_cs, cursor_path);
}

void win_scan_once(void) {
    WinPaths paths;
    resolve_win_paths(&paths);

    db_source_upsert(g_db, "codex", "Windows", paths.codex_dir, NULL, "auto",
                     tp_dir_exists(paths.codex_dir) ? 1 : 0,
                     tp_dir_exists(paths.codex_dir) ? "ok" : "missing");
    db_source_upsert(g_db, "opencode", "Windows", paths.opencode_db, NULL, "auto",
                     tp_file_exists(paths.opencode_db) ? 1 : 0,
                     tp_file_exists(paths.opencode_db) ? "ok" : "missing");
    db_source_upsert(g_db, "claude", "Windows", paths.claude_projects, NULL, "auto",
                     tp_dir_exists(paths.claude_projects) ? 1 : 0,
                     tp_dir_exists(paths.claude_projects) ? "ok" : "missing");
    db_source_upsert(g_db, "ccswitch", "Windows", paths.ccswitch_db, NULL, "auto",
                     tp_file_exists(paths.ccswitch_db) ? 1 : 0,
                     tp_file_exists(paths.ccswitch_db) ? "ok" : "missing");

    scan_opencode(paths.opencode_db, &g_cs, false);
    scan_codex(paths.codex_dir, &g_cs, false);
    scan_claude(paths.claude_projects, &g_cs);

    int64_t cc_delta = 0;
    scan_ccswitch(g_db, paths.ccswitch_db, &cc_delta);
    if (cc_delta != 0) InterlockedExchangeAdd64(&g_pending_gain, cc_delta);

    save_cursors();
    if (g_startup_cycles_pending > 0) InterlockedDecrement(&g_startup_cycles_pending);
}

void win_scan_trigger(void) {
    if (g_trigger) SetEvent(g_trigger);
}

void win_scan_reset_all(void) {
    db_reset_items(g_db);
    cursors_free(&g_cs);
    cursors_load(&g_cs, ""); /* empty */
    win_scan_trigger();
}

int64_t win_scan_take_pending_gain(void) {
    return InterlockedExchangeAdd64(&g_pending_gain, -g_pending_gain);
}

/* ---------- background thread ---------- */

static DWORD WINAPI scan_thread(LPVOID param) {
    (void)param;
    for (;;) {
        win_scan_once();
        int poll = g_settings.poll_ms >= 60000 ? g_settings.poll_ms : 7200000;
        DWORD w = WaitForSingleObject(g_trigger, (DWORD)poll);
        if (w == WAIT_OBJECT_0) ResetEvent(g_trigger);
    }
    return 0;
}

void win_scan_start_thread(void) {
    CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
}
