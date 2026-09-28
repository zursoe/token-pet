#ifndef TOKENPET_WIN_SCAN_H
#define TOKENPET_WIN_SCAN_H

#include "common.h"
#include "db.h"

void win_scan_init(Db *db, const wchar_t *config_dir);
void win_scan_once(void);
void win_scan_trigger(void);
void win_scan_reset_all(void);
int64_t win_scan_take_pending_gain(void);
void win_scan_start_thread(void);

extern volatile LONG g_startup_cycles_pending;

#endif
