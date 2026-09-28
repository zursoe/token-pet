#ifndef TP_SCAN_H
#define TP_SCAN_H

#include "tp_util.h"

/* emit callbacks implemented in collector.c */
void emit_hello(void);
void emit_paths(void);
void emit_item(const char *key, const char *tool, const char *sid, int64_t ts,
               int64_t tin, int64_t tout, int64_t tr, int64_t cr, int64_t cw, double cost);
void emit_meta(const char *tool, const char *sid, const char *title, const char *model,
               const char *provider, const char *dir, int64_t updated);
void emit_status(const char *phase, const char *detail, long long done, long long total);
void emit_done(bool scan);

void scan_opencode(const char *db_path, TpCursorSet *cs, bool first_scan);
void scan_codex(const char *dir, TpCursorSet *cs, bool first_scan);
void scan_kimi(const char *dir, TpCursorSet *cs, bool first_scan);
void scan_claude(const char *projects_dir, TpCursorSet *cs);

/* shared counter used to throttle status updates */
extern long long g_scan_work;

#endif
