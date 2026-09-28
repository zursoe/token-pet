#include "common.h"
#include "db.h"
#include "sqlite3.h"

/* cc-switch.db (read-only):
   - proxy_request_logs rows with data_source='proxy' (unique to cc-switch)
   - usage_daily_rollups gap-fill (day level, replaceable)
   - current provider names for the "门派" display
*/

static int64_t to_ms(double v) {
    if (v > 1e12) return (int64_t)v;
    if (v > 1e9) return (int64_t)(v * 1000);
    return (int64_t)v;
}

static void day_bounds_ms(const char *date, int64_t *start, int64_t *end) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) { *start = 0; *end = 0; return; }
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)d;
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    *start = (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
    *end = *start + 86400000LL;
}

void scan_ccswitch(Db *db, const char *db_path, int64_t *out_added_xp) {
    if (out_added_xp) *out_added_xp = 0;
    if (!tp_file_exists(db_path)) return;

    sqlite3 *c = NULL;
    if (sqlite3_open_v2(db_path, &c, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (c) sqlite3_close(c);
        return;
    }
    sqlite3_busy_timeout(c, 5000);
    int64_t total_delta = 0;

    /* 1. proxy rows */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c,
            "SELECT request_id, app_type, input_tokens, output_tokens, cache_read_tokens,"
            " cache_creation_tokens, total_cost_usd, created_at, session_id"
            " FROM proxy_request_logs WHERE data_source='proxy'",
            -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *rid = (const char *)sqlite3_column_text(st, 0);
            const char *app = (const char *)sqlite3_column_text(st, 1);
            int64_t in = sqlite3_column_int64(st, 2);
            int64_t out = sqlite3_column_int64(st, 3);
            int64_t cr = sqlite3_column_int64(st, 4);
            int64_t cw = sqlite3_column_int64(st, 5);
            const char *costs = (const char *)sqlite3_column_text(st, 6);
            double cost = costs ? strtod(costs, NULL) : 0.0;
            int64_t ts = to_ms(sqlite3_column_double(st, 7));
            const char *sid = (const char *)sqlite3_column_text(st, 8);
            if (!rid || !app) continue;
            if (in + out + cr + cw == 0) continue;
            char key[300];
            snprintf(key, sizeof(key), "ccp:%s", rid);
            total_delta += db_upsert_item(db, key, app, sid ? sid : "", ts, in, out, 0, cr, cw, cost, false);
        }
        sqlite3_finalize(st);
    }

    /* 2. daily rollup gap-fill (replaceable day items) */
    if (sqlite3_prepare_v2(c,
            "SELECT date, app_type, input_tokens, output_tokens, cache_read_tokens, cache_creation_tokens"
            " FROM usage_daily_rollups",
            -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *date = (const char *)sqlite3_column_text(st, 0);
            const char *app = (const char *)sqlite3_column_text(st, 1);
            int64_t in = sqlite3_column_int64(st, 2);
            int64_t out = sqlite3_column_int64(st, 3);
            int64_t cr = sqlite3_column_int64(st, 4);
            int64_t cw = sqlite3_column_int64(st, 5);
            if (!date || !app) continue;
            int64_t rollup_total = in + out + cr + cw;
            if (rollup_total <= 0) continue;
            /* skip today: live direct data would fight with the rollup gap */
            {
                SYSTEMTIME nowst;
                GetLocalTime(&nowst);
                char today[16];
                snprintf(today, sizeof(today), "%04d-%02d-%02d", nowst.wYear, nowst.wMonth, nowst.wDay);
                if (strcmp(date, today) >= 0) continue;
            }
            int64_t day_start, day_end;
            day_bounds_ms(date, &day_start, &day_end);
            if (day_end == 0) continue;

            int64_t direct = db_tool_day_xp(db, app, day_start, day_end);
            int64_t gap = rollup_total - direct;
            if (gap < 0) gap = 0;

            char key[200];
            snprintf(key, sizeof(key), "ccr:%s:%s", date, app);
            int64_t noon = day_start + 43200000LL;
            total_delta += db_upsert_item(db, key, app, "", noon, gap, 0, 0, 0, 0, 0.0, true);
        }
        sqlite3_finalize(st);
    }

    /* 3. current providers -> faction state */
    if (sqlite3_prepare_v2(c, "SELECT name, app_type FROM providers WHERE is_current=1", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            const char *app = (const char *)sqlite3_column_text(st, 1);
            if (!name || !app) continue;
            char k[64];
            snprintf(k, sizeof(k), "faction_%s", app);
            db_state_set(db, k, name);
        }
        sqlite3_finalize(st);
    }

    sqlite3_close(c);
    if (out_added_xp) *out_added_xp = total_delta;
}
