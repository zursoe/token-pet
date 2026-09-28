#include "scan.h"
#include "cJSON.h"

static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_parent_dead = 0;
static volatile int g_reset_cursors = 0;

static char g_opencode_db[1024] = "";
static char g_codex_dir[1024] = "";
static char g_kimi_dir[1024] = "";
static int g_poll_ms = 7200000;

#define MAX_EXTRAS 32
typedef struct { char tool[32]; char path[1024]; char label[128]; } ExtraSrc;
static ExtraSrc g_extras[MAX_EXTRAS];
static int g_extras_n = 0;
static long long g_emitted = 0;

long long g_scan_work = 0;

/* ---------- output ---------- */

static void out_json(cJSON *o) {
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return;
    pthread_mutex_lock(&g_out_lock);
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_out_lock);
    free(s);
}

void emit_hello(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "hello");
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "host", "wsl");
    out_json(o);
}

void emit_paths(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "paths");
    cJSON *arr = cJSON_AddArrayToObject(o, "items");
    struct { const char *tool; const char *path; } items[3];
    items[0].tool = "opencode"; items[0].path = g_opencode_db;
    items[1].tool = "codex";    items[1].path = g_codex_dir;
    items[2].tool = "kimi";     items[2].path = g_kimi_dir;
    for (int i = 0; i < 3; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "tool", items[i].tool);
        cJSON_AddStringToObject(it, "path", items[i].path);
        cJSON_AddBoolToObject(it, "exists",
                              tp_file_exists(items[i].path) || tp_dir_exists(items[i].path));
        cJSON_AddItemToArray(arr, it);
    }
    for (int i = 0; i < g_extras_n; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "tool", g_extras[i].tool);
        cJSON_AddStringToObject(it, "path", g_extras[i].path);
        cJSON_AddStringToObject(it, "label", g_extras[i].label);
        cJSON_AddBoolToObject(it, "exists",
                              tp_file_exists(g_extras[i].path) || tp_dir_exists(g_extras[i].path));
        cJSON_AddItemToArray(arr, it);
    }
    out_json(o);
}

void emit_item(const char *key, const char *tool, const char *sid, int64_t ts,
               int64_t tin, int64_t tout, int64_t tr, int64_t cr, int64_t cw, double cost) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "item");
    cJSON_AddStringToObject(o, "key", key);
    cJSON_AddStringToObject(o, "tool", tool);
    cJSON_AddStringToObject(o, "sid", sid ? sid : "");
    cJSON_AddNumberToObject(o, "ts", (double)ts);
    cJSON_AddNumberToObject(o, "tin", (double)tin);
    cJSON_AddNumberToObject(o, "tout", (double)tout);
    cJSON_AddNumberToObject(o, "tr", (double)tr);
    cJSON_AddNumberToObject(o, "cr", (double)cr);
    cJSON_AddNumberToObject(o, "cw", (double)cw);
    cJSON_AddNumberToObject(o, "cost", cost);
    out_json(o);
    g_emitted++;
}

void emit_meta(const char *tool, const char *sid, const char *title, const char *model,
               const char *provider, const char *dir, int64_t updated) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "meta");
    cJSON_AddStringToObject(o, "tool", tool);
    cJSON_AddStringToObject(o, "sid", sid ? sid : "");
    if (title) cJSON_AddStringToObject(o, "title", title);
    if (model) cJSON_AddStringToObject(o, "model", model);
    if (provider) cJSON_AddStringToObject(o, "provider", provider);
    if (dir) cJSON_AddStringToObject(o, "dir", dir);
    if (updated) cJSON_AddNumberToObject(o, "updated", (double)updated);
    out_json(o);
}

void emit_status(const char *phase, const char *detail, long long done, long long total) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "status");
    cJSON_AddStringToObject(o, "phase", phase);
    if (detail) cJSON_AddStringToObject(o, "detail", detail);
    cJSON_AddNumberToObject(o, "done", (double)done);
    cJSON_AddNumberToObject(o, "total", (double)total);
    out_json(o);
}

void emit_done(bool scan) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "done");
    cJSON_AddBoolToObject(o, "scan", scan);
    cJSON_AddNumberToObject(o, "emitted", (double)g_emitted);
    out_json(o);
}

/* ---------- stdin config ---------- */

static void apply_config(const char *line) {
    cJSON *d = cJSON_Parse(line);
    if (!d) return;
    cJSON *j;
    pthread_mutex_lock(&g_cfg_lock);
    if ((j = cJSON_GetObjectItemCaseSensitive(d, "poll_ms")) && j->valueint >= 60000)
        g_poll_ms = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(d, "opencode_db")) && j->valuestring && j->valuestring[0])
        snprintf(g_opencode_db, sizeof(g_opencode_db), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(d, "codex_dir")) && j->valuestring && j->valuestring[0])
        snprintf(g_codex_dir, sizeof(g_codex_dir), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(d, "kimi_dir")) && j->valuestring && j->valuestring[0])
        snprintf(g_kimi_dir, sizeof(g_kimi_dir), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(d, "reset_cursors")) && cJSON_IsTrue(j))
        g_reset_cursors = 1;
    cJSON *ex = cJSON_GetObjectItemCaseSensitive(d, "extras");
    if (ex && cJSON_IsArray(ex)) {
        g_extras_n = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, ex) {
            if (g_extras_n >= MAX_EXTRAS) break;
            cJSON *jt = cJSON_GetObjectItemCaseSensitive(it, "tool");
            cJSON *jp = cJSON_GetObjectItemCaseSensitive(it, "path");
            cJSON *jl = cJSON_GetObjectItemCaseSensitive(it, "label");
            if (!jt || !jt->valuestring || !jp || !jp->valuestring) continue;
            ExtraSrc *e = &g_extras[g_extras_n++];
            snprintf(e->tool, sizeof(e->tool), "%s", jt->valuestring);
            snprintf(e->path, sizeof(e->path), "%s", jp->valuestring);
            if (jl && jl->valuestring) snprintf(e->label, sizeof(e->label), "%s", jl->valuestring);
            else snprintf(e->label, sizeof(e->label), "%s", jp->valuestring);
        }
    }
    pthread_mutex_unlock(&g_cfg_lock);
    cJSON_Delete(d);
}

static void *stdin_reader(void *arg) {
    (void)arg;
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        if (strstr(line, "\"config\"")) apply_config(line);
    }
    g_parent_dead = 1;
    return NULL;
}

/* ---------- defaults ---------- */

static void default_paths(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/root";

    if (!g_opencode_db[0]) {
        const char *odb = getenv("OPENCODE_DB");
        if (odb && odb[0]) {
            snprintf(g_opencode_db, sizeof(g_opencode_db), "%s", odb);
        } else {
            const char *xdg = getenv("XDG_DATA_HOME");
            if (xdg && xdg[0]) tpu_join(g_opencode_db, sizeof(g_opencode_db), xdg, "opencode/opencode.db");
            else {
                char base[900];
                tpu_join(base, sizeof(base), home, ".local/share");
                tpu_join(g_opencode_db, sizeof(g_opencode_db), base, "opencode/opencode.db");
            }
        }
    }
    if (!g_codex_dir[0]) tpu_join(g_codex_dir, sizeof(g_codex_dir), home, ".codex");
    if (!g_kimi_dir[0]) tpu_join(g_kimi_dir, sizeof(g_kimi_dir), home, ".kimi-code/sessions");
}

int main(int argc, char **argv) {
    bool once = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) once = true;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);

    const char *home = getenv("HOME");
    if (!home) home = "/root";
    char state_dir[1000], cursors_path[1100];
    tpu_join(state_dir, sizeof(state_dir), home, ".local/share/token-pet");
    mkdir(state_dir, 0755);
    tpu_join(cursors_path, sizeof(cursors_path), state_dir, "cursors.json");

    TpCursorSet cs;
    cursors_load(&cs, cursors_path);
    default_paths();

    pthread_t th;
    bool thread_started = false;
    if (!once) {
        pthread_create(&th, NULL, stdin_reader, NULL);
        thread_started = true;
    }

    emit_hello();
    emit_paths();

    for (;;) {
        pthread_mutex_lock(&g_cfg_lock);
        char odb[1024], cdx[1024], kim[1024];
        snprintf(odb, sizeof(odb), "%s", g_opencode_db);
        snprintf(cdx, sizeof(cdx), "%s", g_codex_dir);
        snprintf(kim, sizeof(kim), "%s", g_kimi_dir);
        int poll = g_poll_ms;
        pthread_mutex_unlock(&g_cfg_lock);

        if (g_reset_cursors) {
            cs.n = 0;
            g_reset_cursors = 0;
            emit_status("reset", NULL, 0, 0);
        }
        emit_status("start", NULL, 0, 0);
        scan_opencode(odb, &cs, false);
        scan_codex(cdx, &cs, false);
        scan_kimi(kim, &cs, false);
        for (int i = 0; i < g_extras_n; i++) {
            ExtraSrc e;
            pthread_mutex_lock(&g_cfg_lock);
            e = g_extras[i];
            pthread_mutex_unlock(&g_cfg_lock);
            if (strcmp(e.tool, "codex") == 0) scan_codex(e.path, &cs, false);
            else if (strcmp(e.tool, "opencode") == 0) scan_opencode(e.path, &cs, false);
            else if (strcmp(e.tool, "kimi") == 0) scan_kimi(e.path, &cs, false);
            else if (strcmp(e.tool, "claude") == 0) scan_claude(e.path, &cs);
        }
        emit_done(true);

        if (once) break;
        cursors_save(&cs, cursors_path);

        /* sleep in slices, watch parent */
        for (int slept = 0; slept < poll && !g_parent_dead; slept += 100) {
            usleep(100 * 1000);
        }
        if (g_parent_dead) break;
    }

    if (thread_started) pthread_cancel(th);
    if (!once) cursors_save(&cs, cursors_path);
    cursors_free(&cs);
    return 0;
}
