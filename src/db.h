#ifndef TOKENPET_DB_H
#define TOKENPET_DB_H

#include "common.h"

typedef struct Db Db;

Db *db_open(const wchar_t *path);
void db_close(Db *d);
void db_lock(Db *d);
void db_unlock(Db *d);

/* upsert a token item. returns xp delta (may be 0; negative only when replace=true) */
int64_t db_upsert_item(Db *d, const char *key, const char *tool, const char *sid, int64_t ts,
                       int64_t tin, int64_t tout, int64_t tr, int64_t cr, int64_t cw,
                       double cost, bool replace);

void db_upsert_meta(Db *d, const char *tool, const char *sid, const char *title,
                    const char *model, const char *provider, const char *dir, int64_t updated);

int64_t db_total_xp(Db *d);
int64_t db_today_xp(Db *d);
int64_t db_tool_day_xp(Db *d, const char *tool, int64_t day_start, int64_t day_end);
double  db_total_cost(Db *d);
int64_t db_count_items(Db *d);

/* sync cursor persistence for the Windows-side scanners */
bool db_sync_get(Db *d, const char *path, int64_t *size, int64_t *offset, uint32_t *fp);
void db_sync_set(Db *d, const char *path, int64_t size, int64_t offset, uint32_t fp);

/* source registry */
void db_source_upsert(Db *d, const char *tool, const char *label, const char *path,
                      const char *distro, const char *origin, int enabled, const char *status);

/* key-value state */
char *db_state_get(Db *d, const char *key);
void  db_state_set(Db *d, const char *key, const char *value);

/* maintenance */
void db_reset_items(Db *d);
void db_exec(Db *d, const char *sql);

/* raw handle for reporting (caller must db_lock/db_unlock) */
void *db_raw(Db *d);

#endif
