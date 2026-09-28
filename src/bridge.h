#ifndef TOKENPET_BRIDGE_H
#define TOKENPET_BRIDGE_H

#include "common.h"
#include "db.h"

void bridge_start(Db *db, const wchar_t *distro, bool reset_cursors);
void bridge_stop(void);
void bridge_set_reset_on_next(void);
void bridge_run_once_sync(Db *db, const wchar_t *distro);

#endif
