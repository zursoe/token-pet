#ifndef TOKENPET_REALM_H
#define TOKENPET_REALM_H

#include "common.h"

#define REALM_MAX_LEVELS 12

typedef struct {
    char name[64];
    int64_t min;
    char levels[REALM_MAX_LEVELS][32];
    int n_levels;
} RealmDef;

typedef struct {
    RealmDef realms[24];
    int n;
} RealmTable;

void realm_load(RealmTable *t, const wchar_t *config_dir);
void realm_compute(const RealmTable *t, int64_t xp, wchar_t *out_full, size_t cap,
                   float *out_progress, int64_t *out_next_at, int *out_realm_index, int *out_sub_index);

#endif
