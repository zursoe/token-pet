#ifndef TOKENPET_CHART_H
#define TOKENPET_CHART_H

#include "common.h"
#include "db.h"

/* create the chart child window (hidden until shown by the panel) */
HWND chart_create(HWND parent, Db *db);

/* 0 = day (30d), 1 = week (12w), 2 = month (12m); reloads data */
void chart_set_granularity(HWND chart, int gran);

/* re-run the aggregation query and repaint */
void chart_reload(HWND chart);

/* custom range as YYYY-MM-DD; NULL/empty falls back to earliest data / today */
void chart_set_range(HWND chart, const char *start_day, const char *end_day);
void chart_clear_range(HWND chart);

/* effective range of the last load (for the date pickers) */
void chart_get_range(HWND chart, char *start_day, size_t scap, char *end_day, size_t ecap);

#endif
