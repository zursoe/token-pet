#ifndef TOKENPET_PRICING_H
#define TOKENPET_PRICING_H

#include "common.h"

/* load config/pricing.json (writes a default template when missing) */
void pricing_load(const wchar_t *config_dir);

/* register the SQL scalar function cost_cny() on a sqlite3 handle */
void pricing_register(void *db);

/* RMB cost of one usage record, matched by model name then tool name */
double pricing_cost_cny(const char *model, const char *tool,
                        int64_t tin, int64_t tout, int64_t tr, int64_t cr, int64_t cw);

#endif
