#include "realm.h"
#include "cJSON.h"
#include <math.h>

static void set_defaults(RealmTable *t) {
    memset(t, 0, sizeof(*t));
    const char *sub4[] = { "初期", "中期", "后期", "圆满" };
    const char *qi9[] = { "一层", "二层", "三层", "四层", "五层", "六层", "七层", "八层", "九层" };

    struct { const char *name; int64_t min; const char **levels; int nl; } defs[] = {
        { "凡人", 0LL, NULL, 0 },
        { "炼气", 1000000000LL, qi9, 9 },
        { "筑基", 10000000000LL, sub4, 4 },
        { "金丹", 100000000000LL, sub4, 4 },
        { "元婴", 1000000000000LL, sub4, 4 },
        { "化神", 10000000000000LL, sub4, 4 },
        { "炼虚", 100000000000000LL, sub4, 4 },
        { "合体", 1000000000000000LL, sub4, 4 },
        { "大乘", 10000000000000000LL, sub4, 4 },
        { "渡劫", 100000000000000000LL, sub4, 4 },
    };
    int n = (int)(sizeof(defs) / sizeof(defs[0]));
    for (int i = 0; i < n && t->n < 24; i++) {
        RealmDef *r = &t->realms[t->n++];
        snprintf(r->name, sizeof(r->name), "%s", defs[i].name);
        r->min = defs[i].min;
        r->n_levels = defs[i].nl;
        for (int j = 0; j < defs[i].nl && j < REALM_MAX_LEVELS; j++)
            snprintf(r->levels[j], sizeof(r->levels[j]), "%s", defs[i].levels[j]);
    }
}

static const wchar_t *default_json =
    L"{\n"
    L"  \"comment\": \"修为境界表：min 为进入该境界所需总修为（token 数），levels 为小台阶\",\n"
    L"  \"realms\": [\n"
    L"    {\"name\": \"凡人\", \"min\": 0},\n"
    L"    {\"name\": \"炼气\", \"min\": 1000000000, \"levels\": [\"一层\",\"二层\",\"三层\",\"四层\",\"五层\",\"六层\",\"七层\",\"八层\",\"九层\"]},\n"
    L"    {\"name\": \"筑基\", \"min\": 10000000000, \"levels\": [\"初期\",\"中期\",\"后期\",\"圆满\"]},\n"
    L"    {\"name\": \"金丹\", \"min\": 100000000000, \"levels\": [\"初期\",\"中期\",\"后期\",\"圆满\"]},\n"
    L"    {\"name\": \"元婴\", \"min\": 1000000000000, \"levels\": [\"初期\",\"中期\",\"后期\",\"圆满\"]},\n"
    L"    {\"name\": \"化神\", \"min\": 10000000000000, \"levels\": [\"初期\",\"中期\",\"后期\",\"圆满\"]},\n"
    L"    {\"name\": \"炼虚\", \"min\": 100000000000000, \"levels\": [\"初期\",\"中期\",\"后期\",\"圆满\"]},\n"
    L"    {\"name\": \"合体\", \"min\": 1000000000000000, \"levels\": [\"初期\",\"中期\",\"后期\",\"圆满\"]},\n"
    L"    {\"name\": \"大乘\", \"min\": 10000000000000000, \"levels\": [\"初期\",\"中期\",\"后期\",\"圆满\"]},\n"
    L"    {\"name\": \"渡劫\", \"min\": 100000000000000000, \"levels\": [\"初期\",\"中期\",\"后期\",\"圆满\"]}\n"
    L"  ]\n"
    L"}\n";

void realm_load(RealmTable *t, const wchar_t *config_dir) {
    set_defaults(t);
    wchar_t path[TP_PATH_MAX];
    tp_path_join(path, TP_PATH_MAX, config_dir, L"realms.json");

    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        /* write default config */
        f = _wfopen(path, L"wb");
        if (f) {
            char *u = tp_wide_to_utf8(default_json);
            fwrite(u, 1, strlen(u), f);
            free(u);
            fclose(f);
        }
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)xp_alloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "realms");
    if (arr && cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        t->n = 0;
        for (int i = 0; i < n && t->n < 24; i++) {
            cJSON *it = cJSON_GetArrayItem(arr, i);
            cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *minv = cJSON_GetObjectItemCaseSensitive(it, "min");
            cJSON *lv = cJSON_GetObjectItemCaseSensitive(it, "levels");
            if (!name || !name->valuestring || !minv) continue;
            RealmDef *r = &t->realms[t->n++];
            r->min = (int64_t)minv->valuedouble;
            snprintf(r->name, sizeof(r->name), "%s", name->valuestring);
            r->n_levels = 0;
            if (lv && cJSON_IsArray(lv)) {
                int ln = cJSON_GetArraySize(lv);
                for (int j = 0; j < ln && j < REALM_MAX_LEVELS; j++) {
                    cJSON *s = cJSON_GetArrayItem(lv, j);
                    if (s && s->valuestring)
                        snprintf(r->levels[r->n_levels++], sizeof(r->levels[0]), "%s", s->valuestring);
                }
            }
        }
    }
    cJSON_Delete(root);
}

void realm_compute(const RealmTable *t, int64_t xp, wchar_t *out_full, size_t cap,
                   float *out_progress, int64_t *out_next_at, int *out_realm_index, int *out_sub_index) {
    int idx = 0;
    for (int i = 0; i < t->n; i++) {
        if (xp >= t->realms[i].min) idx = i;
        else break;
    }
    const RealmDef *r = &t->realms[idx];
    int sub = -1;
    float progress = 0.0f;
    int64_t next_at = 0;

    if (idx + 1 < t->n) {
        int64_t next_min = t->realms[idx + 1].min;
        if (r->n_levels > 0 && next_min > r->min && xp >= r->min) {
            double span = (double)next_min / (double)r->min;   /* usually 10 */
            double rel = (double)xp / (double)r->min;
            if (rel < 1.0) rel = 1.0;
            int level = (int)floor(log(rel) / log(span) * r->n_levels);
            if (level < 0) level = 0;
            if (level >= r->n_levels) level = r->n_levels - 1;
            sub = level;
            double lo = pow(span, (double)level / r->n_levels) * (double)r->min;
            double hi = pow(span, (double)(level + 1) / r->n_levels) * (double)r->min;
            progress = (float)(((double)xp - lo) / (hi - lo));
            if (progress < 0) progress = 0;
            if (progress > 1) progress = 1;
            next_at = (int64_t)hi;
        } else {
            progress = (float)((double)(xp - r->min) / (double)(next_min - r->min));
            if (progress < 0) progress = 0;
            if (progress > 1) progress = 1;
            next_at = next_min;
        }
    }

    if (out_full) {
        char narrow[128];
        if (sub >= 0) snprintf(narrow, sizeof(narrow), "%s%s", r->name, r->levels[sub]);
        else snprintf(narrow, sizeof(narrow), "%s", r->name);
        wchar_t *w = tp_utf8_to_wide(narrow);
        wcsncpy(out_full, w, cap - 1);
        out_full[cap - 1] = 0;
        free(w);
    }
    if (out_progress) *out_progress = progress;
    if (out_next_at) *out_next_at = next_at;
    if (out_realm_index) *out_realm_index = idx;
    if (out_sub_index) *out_sub_index = sub;
}
