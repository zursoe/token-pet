#include "scan.h"
#include "sqlite3.h"
#include "cJSON.h"

/* session meta change tracking (in-memory) */
typedef struct {
    char id[128];
    long long updated;
} SesSeen;

static SesSeen *g_seen = NULL;
static int g_seen_n = 0, g_seen_cap = 0;

static long long seen_updated(const char *id) {
    for (int i = 0; i < g_seen_n; i++) {
        if (strcmp(g_seen[i].id, id) == 0) return g_seen[i].updated;
    }
    return -1;
}

static void seen_set(const char *id, long long updated) {
    for (int i = 0; i < g_seen_n; i++) {
        if (strcmp(g_seen[i].id, id) == 0) { g_seen[i].updated = updated; return; }
    }
    if (g_seen_n == g_seen_cap) {
        g_seen_cap = g_seen_cap ? g_seen_cap * 2 : 256;
        g_seen = (SesSeen *)realloc(g_seen, (size_t)g_seen_cap * sizeof(SesSeen));
    }
    SesSeen *s = &g_seen[g_seen_n++];
    snprintf(s->id, sizeof(s->id), "%s", id);
    s->updated = updated;
}

static void emit_sessions_meta(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT id, title, model, directory, time_updated FROM session";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(st, 0);
        const char *title = (const char *)sqlite3_column_text(st, 1);
        const char *model = (const char *)sqlite3_column_text(st, 2);
        const char *dir = (const char *)sqlite3_column_text(st, 3);
        long long updated = sqlite3_column_int64(st, 4);
        if (!id) continue;
        if (seen_updated(id) == updated) continue;
        seen_set(id, updated);

        /* model column is a JSON object in opencode */
        char modelbuf[256] = "";
        if (model && model[0] == '{') {
            cJSON *m = cJSON_Parse(model);
            if (m) {
                cJSON *mid = cJSON_GetObjectItemCaseSensitive(m, "id");
                cJSON *pid = cJSON_GetObjectItemCaseSensitive(m, "providerID");
                if (mid && mid->valuestring) snprintf(modelbuf, sizeof(modelbuf), "%s", mid->valuestring);
                if (pid && pid->valuestring) {
                    size_t l = strlen(modelbuf);
                    snprintf(modelbuf + l, sizeof(modelbuf) - l, " (%s)", pid->valuestring);
                }
                cJSON_Delete(m);
            }
        } else if (model) {
            snprintf(modelbuf, sizeof(modelbuf), "%s", model);
        }
        emit_meta("opencode", id, title, modelbuf[0] ? modelbuf : NULL, NULL, dir, updated);
    }
    sqlite3_finalize(st);
}

static void emit_message(sqlite3_stmt *st) {
    const char *id = (const char *)sqlite3_column_text(st, 1);
    const char *session_id = (const char *)sqlite3_column_text(st, 2);
    long long t_created = sqlite3_column_int64(st, 3);
    const char *data = (const char *)sqlite3_column_text(st, 4);
    if (!id || !data) return;

    cJSON *d = cJSON_Parse(data);
    if (!d) return;
    cJSON *role = cJSON_GetObjectItemCaseSensitive(d, "role");
    if (!role || !role->valuestring || strcmp(role->valuestring, "assistant") != 0) {
        cJSON_Delete(d);
        return;
    }
    cJSON *tokens = cJSON_GetObjectItemCaseSensitive(d, "tokens");
    if (!tokens) { cJSON_Delete(d); return; }
    double in = 0, out = 0, tr = 0, cr = 0, cw = 0;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(tokens, "input"))) in = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(tokens, "output"))) out = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(tokens, "reasoning"))) tr = j->valuedouble;
    cJSON *cache = cJSON_GetObjectItemCaseSensitive(tokens, "cache");
    if (cache) {
        if ((j = cJSON_GetObjectItemCaseSensitive(cache, "read"))) cr = j->valuedouble;
        if ((j = cJSON_GetObjectItemCaseSensitive(cache, "write"))) cw = j->valuedouble;
    }
    double cost = 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(d, "cost"))) cost = j->valuedouble;
    cJSON *timeo = cJSON_GetObjectItemCaseSensitive(d, "time");
    long long ts = t_created;
    if (timeo) {
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(timeo, "created");
        if (tc) ts = (long long)tc->valuedouble;
    }
    if (in + out + tr + cr + cw > 0) {
        char key[300];
        snprintf(key, sizeof(key), "oc:%s", id);
        emit_item(key, "opencode", session_id ? session_id : "", ts,
                  (int64_t)in, (int64_t)out, (int64_t)tr, (int64_t)cr, (int64_t)cw, cost);
    }
    cJSON_Delete(d);
}

void scan_opencode(const char *db_path, TpCursorSet *cs, bool first_scan) {
    if (!tp_file_exists(db_path)) return;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 5000);

    emit_sessions_meta(db);

    TpCursor *c = cursors_get(cs, db_path);
    long long max_rowid = 0;
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT IFNULL(MAX(rowid),0) FROM message", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) max_rowid = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }
    long long lo;
    if (c->line_no <= 0 || c->line_no > max_rowid) {
        lo = 0; /* first scan or db recreated */
    } else {
        lo = max_rowid - 300;         /* re-read recent rows for streaming updates */
        if (lo > c->line_no) lo = c->line_no;
        if (lo < 0) lo = 0;
    }
    /* remember highest rowid */
    c->line_no = max_rowid;
    c->size = tp_file_size(db_path);
    (void)first_scan;

    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT rowid, id, session_id, time_created, data FROM message WHERE rowid > ? ORDER BY rowid";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, lo);
        long long n = 0;
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            emit_message(st);
            n++;
            if (n % 2000 == 0) emit_status("scan", "opencode", n, 0);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}
