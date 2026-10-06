#include "db.h"
#include "sqlite3.h"

struct Db {
    sqlite3 *db;
    CRITICAL_SECTION cs;
};

static int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        tp_log("db exec error: %s | sql: %.120s", err ? err : "?", sql);
        sqlite3_free(err);
    }
    return rc;
}

static void src_of_key(const char *key, char *out, size_t cap) {
    if (!key || !key[0]) { snprintf(out, cap, ""); return; }
    if (strncmp(key, "ccr:", 4) == 0) snprintf(out, cap, "ccr");
    else if (strncmp(key, "ccp:", 4) == 0) snprintf(out, cap, "ccp");
    else snprintf(out, cap, "%.2s", key);
}

/* one-time materialization of items into the per-day aggregate table */
static void agg_backfill(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    bool done = false;
    if (sqlite3_prepare_v2(db, "SELECT value FROM state WHERE key='agg_v1'", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) done = true;
        sqlite3_finalize(st);
    }
    if (done) return;
    int64_t t0 = tp_now_ms();
    exec_sql(db, "BEGIN;");
    int rc = exec_sql(db,
        "INSERT INTO items_agg(day,tool,sid,src,cnt,tin,tout,tr,cr,cw,last_ts)"
        " SELECT date(ts/1000,'unixepoch','localtime'), tool, sid,"
        " CASE WHEN key LIKE 'ccr:%' THEN 'ccr' WHEN key LIKE 'ccp:%' THEN 'ccp'"
        "      ELSE substr(key,1,2) END,"
        " COUNT(*), SUM(tin), SUM(tout), SUM(tr), SUM(cr), SUM(cw), MAX(ts)"
        " FROM items GROUP BY 1,2,3,4;");
    if (rc == SQLITE_OK) {
        exec_sql(db, "DELETE FROM state WHERE key='agg_v1';"
                     "INSERT INTO state(key,value) VALUES('agg_v1','1');"
                     "COMMIT;");
        tp_log("db: items_agg backfilled in %lld ms", (long long)(tp_now_ms() - t0));
    } else {
        exec_sql(db, "ROLLBACK;");
        tp_log("db: items_agg backfill failed, will retry next start");
    }
}

Db *db_open(const wchar_t *path) {
    Db *d = (Db *)xp_alloc(sizeof(Db));
    InitializeCriticalSection(&d->cs);
    char *p = tp_wide_to_utf8(path);

    if (sqlite3_open_v2(p, &d->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        tp_log("db open failed: %s", p);
        free(p);
        DeleteCriticalSection(&d->cs);
        free(d);
        return NULL;
    }
    free(p);
    sqlite3_busy_timeout(d->db, 5000);
    exec_sql(d->db, "PRAGMA journal_mode=WAL;");
    exec_sql(d->db, "PRAGMA synchronous=NORMAL;");
    exec_sql(d->db,
        "CREATE TABLE IF NOT EXISTS items("
        " key TEXT PRIMARY KEY, tool TEXT, sid TEXT, ts INTEGER,"
        " tin INTEGER DEFAULT 0, tout INTEGER DEFAULT 0, tr INTEGER DEFAULT 0,"
        " cr INTEGER DEFAULT 0, cw INTEGER DEFAULT 0, cost REAL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_items_tool_ts ON items(tool, ts);"
        "CREATE INDEX IF NOT EXISTS idx_items_tool_sid ON items(tool, sid);"
        "CREATE INDEX IF NOT EXISTS idx_items_ts ON items(ts);"
        "CREATE TABLE IF NOT EXISTS meta("
        " tool TEXT, sid TEXT, title TEXT, model TEXT, provider TEXT, dir TEXT, updated INTEGER,"
        " PRIMARY KEY(tool, sid));"
        "CREATE TABLE IF NOT EXISTS sync_files("
        " path TEXT PRIMARY KEY, size INTEGER, offset INTEGER, fp INTEGER, mtime INTEGER);"
        "CREATE TABLE IF NOT EXISTS sources("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, tool TEXT, label TEXT, path TEXT UNIQUE,"
        " distro TEXT, origin TEXT, enabled INTEGER DEFAULT 1, status TEXT, last_scan INTEGER, note TEXT);"
        "CREATE TABLE IF NOT EXISTS state(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS sync_log("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, tool TEXT, location TEXT,"
        " added_items INTEGER, added_xp INTEGER, note TEXT);"
        "CREATE TABLE IF NOT EXISTS items_agg("
        " day TEXT, tool TEXT, sid TEXT, src TEXT, cnt INTEGER DEFAULT 0,"
        " tin INTEGER DEFAULT 0, tout INTEGER DEFAULT 0, tr INTEGER DEFAULT 0,"
        " cr INTEGER DEFAULT 0, cw INTEGER DEFAULT 0, last_ts INTEGER DEFAULT 0,"
        " PRIMARY KEY(day, tool, sid, src)) WITHOUT ROWID;"
    );
    agg_backfill(d->db);
    return d;
}

void db_close(Db *d) {
    if (!d) return;
    db_lock(d);
    if (d->db) sqlite3_close(d->db);
    db_unlock(d);
    DeleteCriticalSection(&d->cs);
    free(d);
}

void db_lock(Db *d) { if (d) EnterCriticalSection(&d->cs); }
void db_unlock(Db *d) { if (d) LeaveCriticalSection(&d->cs); }

void db_exec(Db *d, const char *sql) {
    db_lock(d);
    exec_sql(d->db, sql);
    db_unlock(d);
}

int64_t db_upsert_item(Db *d, const char *key, const char *tool, const char *sid, int64_t ts,
                       int64_t tin, int64_t tout, int64_t tr, int64_t cr, int64_t cw,
                       double cost, bool replace) {
    db_lock(d);
    int64_t delta = 0;
    sqlite3_stmt *st = NULL;
    int64_t o_tin = 0, o_tout = 0, o_tr = 0, o_cr = 0, o_cw = 0;
    bool exists = false;

    if (sqlite3_prepare_v2(d->db, "SELECT tin,tout,tr,cr,cw FROM items WHERE key=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            exists = true;
            o_tin = sqlite3_column_int64(st, 0);
            o_tout = sqlite3_column_int64(st, 1);
            o_tr = sqlite3_column_int64(st, 2);
            o_cr = sqlite3_column_int64(st, 3);
            o_cw = sqlite3_column_int64(st, 4);
        }
        sqlite3_finalize(st);
    }

    int64_t n_tin = tin, n_tout = tout, n_tr = tr, n_cr = cr, n_cw = cw;
    if (exists && !replace) {
        if (n_tin < o_tin) n_tin = o_tin;
        if (n_tout < o_tout) n_tout = o_tout;
        if (n_tr < o_tr) n_tr = o_tr;
        if (n_cr < o_cr) n_cr = o_cr;
        if (n_cw < o_cw) n_cw = o_cw;
    }
    delta = (n_tin + n_tout + n_tr + n_cr + n_cw) - (o_tin + o_tout + o_tr + o_cr + o_cw);

    if (!exists) {
        if (sqlite3_prepare_v2(d->db,
                "INSERT INTO items(key,tool,sid,ts,tin,tout,tr,cr,cw,cost) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, tool, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, sid ? sid : "", -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 4, ts);
            sqlite3_bind_int64(st, 5, n_tin);
            sqlite3_bind_int64(st, 6, n_tout);
            sqlite3_bind_int64(st, 7, n_tr);
            sqlite3_bind_int64(st, 8, n_cr);
            sqlite3_bind_int64(st, 9, n_cw);
            sqlite3_bind_double(st, 10, cost);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    } else if (delta != 0) {
        if (sqlite3_prepare_v2(d->db,
                "UPDATE items SET tin=?2,tout=?3,tr=?4,cr=?5,cw=?6, cost=CASE WHEN ?7>cost THEN ?7 ELSE cost END WHERE key=?1",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 2, n_tin);
            sqlite3_bind_int64(st, 3, n_tout);
            sqlite3_bind_int64(st, 4, n_tr);
            sqlite3_bind_int64(st, 5, n_cr);
            sqlite3_bind_int64(st, 6, n_cw);
            sqlite3_bind_double(st, 7, cost);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }

    int64_t d_tin = n_tin - o_tin, d_tout = n_tout - o_tout, d_tr = n_tr - o_tr;
    int64_t d_cr = n_cr - o_cr, d_cw = n_cw - o_cw;
    if (d_tin || d_tout || d_tr || d_cr || d_cw) {
        sqlite3_stmt *ag = NULL;
        if (sqlite3_prepare_v2(d->db,
                "INSERT INTO items_agg(day,tool,sid,src,cnt,tin,tout,tr,cr,cw,last_ts)"
                " VALUES(date(?1/1000,'unixepoch','localtime'),?2,?3,?4,?5,?6,?7,?8,?9,?10,?1)"
                " ON CONFLICT(day,tool,sid,src) DO UPDATE SET"
                " cnt=cnt+excluded.cnt,"
                " tin=tin+excluded.tin, tout=tout+excluded.tout, tr=tr+excluded.tr,"
                " cr=cr+excluded.cr, cw=cw+excluded.cw,"
                " last_ts=CASE WHEN excluded.last_ts>last_ts THEN excluded.last_ts ELSE last_ts END",
                -1, &ag, NULL) == SQLITE_OK) {
            char src[8];
            src_of_key(key, src, sizeof(src));
            sqlite3_bind_int64(ag, 1, ts);
            sqlite3_bind_text(ag, 2, tool, -1, SQLITE_STATIC);
            sqlite3_bind_text(ag, 3, sid ? sid : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(ag, 4, src, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ag, 5, exists ? 0 : 1);
            sqlite3_bind_int64(ag, 6, d_tin);
            sqlite3_bind_int64(ag, 7, d_tout);
            sqlite3_bind_int64(ag, 8, d_tr);
            sqlite3_bind_int64(ag, 9, d_cr);
            sqlite3_bind_int64(ag, 10, d_cw);
            sqlite3_step(ag);
            sqlite3_finalize(ag);
        }
    }
    db_unlock(d);
    return delta;
}

void db_upsert_meta(Db *d, const char *tool, const char *sid, const char *title,
                    const char *model, const char *provider, const char *dir, int64_t updated) {
    if (!sid) return;
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db,
            "INSERT INTO meta(tool,sid,title,model,provider,dir,updated) VALUES(?1,?2,?3,?4,?5,?6,?7)"
            " ON CONFLICT(tool,sid) DO UPDATE SET"
            " title=COALESCE(excluded.title, meta.title),"
            " model=COALESCE(excluded.model, meta.model),"
            " provider=COALESCE(excluded.provider, meta.provider),"
            " dir=COALESCE(excluded.dir, meta.dir),"
            " updated=MAX(excluded.updated, meta.updated)",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, tool, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, sid, -1, SQLITE_STATIC);
        if (title) sqlite3_bind_text(st, 3, title, -1, SQLITE_STATIC); else sqlite3_bind_null(st, 3);
        if (model) sqlite3_bind_text(st, 4, model, -1, SQLITE_STATIC); else sqlite3_bind_null(st, 4);
        if (provider) sqlite3_bind_text(st, 5, provider, -1, SQLITE_STATIC); else sqlite3_bind_null(st, 5);
        if (dir) sqlite3_bind_text(st, 6, dir, -1, SQLITE_STATIC); else sqlite3_bind_null(st, 6);
        sqlite3_bind_int64(st, 7, updated);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    db_unlock(d);
}

int64_t db_total_xp(Db *d) {
    int64_t v = 0;
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db, "SELECT COALESCE(SUM(tin+tout+tr+cr+cw),0) FROM items", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    db_unlock(d);
    return v;
}

static int64_t local_midnight_ms(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    st.wHour = 0; st.wMinute = 0; st.wSecond = 0; st.wMilliseconds = 0;
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

int64_t db_today_xp(Db *d) {
    int64_t v = 0;
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db, "SELECT COALESCE(SUM(tin+tout+tr+cr+cw),0) FROM items WHERE ts >= ?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, local_midnight_ms());
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    db_unlock(d);
    return v;
}

int64_t db_tool_day_xp(Db *d, const char *tool, int64_t day_start, int64_t day_end) {
    int64_t v = 0;
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db,
            "SELECT COALESCE(SUM(tin+tout+tr+cr+cw),0) FROM items"
            " WHERE tool=?1 AND ts>=?2 AND ts<?3 AND key NOT LIKE 'ccr:%'",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, tool, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, day_start);
        sqlite3_bind_int64(st, 3, day_end);
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    db_unlock(d);
    return v;
}

double db_total_cost(Db *d) {
    double v = 0;
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db, "SELECT COALESCE(SUM(cost),0) FROM items", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_double(st, 0);
        sqlite3_finalize(st);
    }
    db_unlock(d);
    return v;
}

int64_t db_count_items(Db *d) {
    int64_t v = 0;
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db, "SELECT COUNT(*) FROM items", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    db_unlock(d);
    return v;
}

bool db_sync_get(Db *d, const char *path, int64_t *size, int64_t *offset, uint32_t *fp) {
    bool found = false;
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db, "SELECT size,offset,fp FROM sync_files WHERE path=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            found = true;
            if (size) *size = sqlite3_column_int64(st, 0);
            if (offset) *offset = sqlite3_column_int64(st, 1);
            if (fp) *fp = (uint32_t)sqlite3_column_int64(st, 2);
        }
        sqlite3_finalize(st);
    }
    db_unlock(d);
    return found;
}

void db_sync_set(Db *d, const char *path, int64_t size, int64_t offset, uint32_t fp) {
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db,
            "INSERT INTO sync_files(path,size,offset,fp,mtime) VALUES(?1,?2,?3,?4,?5)"
            " ON CONFLICT(path) DO UPDATE SET size=excluded.size, offset=excluded.offset, fp=excluded.fp, mtime=excluded.mtime",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, size);
        sqlite3_bind_int64(st, 3, offset);
        sqlite3_bind_int64(st, 4, (int64_t)fp);
        sqlite3_bind_int64(st, 5, tp_now_ms());
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    db_unlock(d);
}

void db_source_upsert(Db *d, const char *tool, const char *label, const char *path,
                      const char *distro, const char *origin, int enabled, const char *status) {
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db,
            "INSERT INTO sources(tool,label,path,distro,origin,enabled,status,last_scan) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"
            " ON CONFLICT(path) DO UPDATE SET tool=excluded.tool, label=excluded.label,"
            " distro=COALESCE(excluded.distro, sources.distro), status=excluded.status, last_scan=excluded.last_scan",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, tool, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, label, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, path, -1, SQLITE_STATIC);
        if (distro) sqlite3_bind_text(st, 4, distro, -1, SQLITE_STATIC); else sqlite3_bind_null(st, 4);
        sqlite3_bind_text(st, 5, origin ? origin : "auto", -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 6, enabled);
        if (status) sqlite3_bind_text(st, 7, status, -1, SQLITE_STATIC); else sqlite3_bind_null(st, 7);
        sqlite3_bind_int64(st, 8, tp_now_ms());
        sqlite3_step(st);
        sqlite3_finalize(st);

        /* mark other locations of the same tool+label as inactive (path changed) */
        if (sqlite3_prepare_v2(d->db,
                "UPDATE sources SET enabled=0 WHERE tool=?1 AND label=?2 AND path<>?3",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, tool, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, label, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, path, -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    db_unlock(d);
}

char *db_state_get(Db *d, const char *key) {
    char *out = NULL;
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db, "SELECT value FROM state WHERE key=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(st, 0);
            if (v) out = xp_strdup((const char *)v);
        }
        sqlite3_finalize(st);
    }
    db_unlock(d);
    return out;
}

void db_state_set(Db *d, const char *key, const char *value) {
    db_lock(d);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db,
            "INSERT INTO state(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, value ? value : "", -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    db_unlock(d);
}

void db_reset_items(Db *d) {
    db_exec(d, "DELETE FROM items; DELETE FROM items_agg; DELETE FROM sync_files; DELETE FROM meta;");
}

void *db_raw(Db *d) {
    return d ? d->db : NULL;
}
