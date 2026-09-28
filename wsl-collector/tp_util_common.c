#include "tp_util.h"
#include "cJSON.h"

void tpu_join(char *out, size_t cap, const char *a, const char *b) {
    if (!b || !*b) { snprintf(out, cap, "%s", a ? a : ""); return; }
    if (a && a[0] && (b[0] == '/' || b[0] == '\\')) { snprintf(out, cap, "%s", b); return; }
    size_t al = a ? strlen(a) : 0;
    if (al > 0 && (a[al - 1] == '/' || a[al - 1] == '\\')) snprintf(out, cap, "%s%s", a, b);
    else snprintf(out, cap, "%s%c%s", a ? a : "", 
#ifdef _WIN32
                  '\\',
#else
                  '/',
#endif
                  b);
}

const char *tpu_basename(const char *path) {
    const char *best = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') best = p + 1;
    }
    return best;
}

bool tpu_has_dir_component(const char *path, const char *comp) {
    char pat1[128], pat2[128];
    snprintf(pat1, sizeof(pat1), "/%s/", comp);
    snprintf(pat2, sizeof(pat2), "\\%s\\", comp);
    return strstr(path, pat1) != NULL || strstr(path, pat2) != NULL;
}

uint32_t tp_fnv32(const unsigned char *d, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 16777619u;
    }
    return h;
}

int64_t tp_parse_iso_ms(const char *iso) {
    if (!iso) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    int n = sscanf(iso, "%d-%d-%dT%d:%d:%d.%d", &y, &mo, &d, &h, &mi, &s, &ms);
    if (n < 6) return 0;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = s;
#ifdef _WIN32
    time_t t = _mkgmtime(&tmv);
#else
    time_t t = timegm(&tmv);
#endif
    return (int64_t)t * 1000 + ms;
}

char *tp_read_all(const char *path, size_t *len) {
    FILE *f = tp_fopen(path, "rb");
    if (!f) return NULL;
    tp_fseek(f, 0);
    char *buf = (char *)malloc(1 << 20);
    size_t cap = 1 << 20, used = 0;
    size_t rd;
    while ((rd = fread(buf + used, 1, cap - used - 1, f)) > 0) {
        used += rd;
        if (used + 1 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
    }
    fclose(f);
    buf[used] = 0;
    if (len) *len = used;
    return buf;
}

/* ---------- streaming line reader ---------- */

void lr_init(TpLineReader *lr, size_t cap) {
    memset(lr, 0, sizeof(*lr));
    lr->cap = cap;
    lr->buf = (char *)malloc(cap);
    lr->buf[0] = 0;
}

void lr_free(TpLineReader *lr) {
    free(lr->buf);
    lr->buf = NULL;
}

int lr_feed(TpLineReader *lr, int c) {
    if (c == '\n') {
        lr->consumed += (long long)lr->len + 1;
        lr->lineno++;
        lr->buf[lr->len < lr->cap ? lr->len : lr->cap - 1] = 0;
        int complete = !lr->overflow;
        lr->len = 0;
        lr->overflow = false;
        return complete;
    }
    if (lr->len < lr->cap - 1) {
        lr->buf[lr->len++] = (char)c;
    } else {
        lr->len++;
        lr->overflow = true;
    }
    return 0;
}

/* ---------- cursors ---------- */

static void ensure_cap(TpCursorSet *cs, int need) {
    if (cs->n + need <= cs->cap) return;
    int nc = cs->cap ? cs->cap * 2 : 64;
    while (nc < cs->n + need) nc *= 2;
    cs->arr = (TpCursor *)realloc(cs->arr, (size_t)nc * sizeof(TpCursor));
    memset(cs->arr + cs->cap, 0, (size_t)(nc - cs->cap) * sizeof(TpCursor));
    cs->cap = nc;
}

TpCursor *cursors_get(TpCursorSet *cs, const char *path) {
    for (int i = 0; i < cs->n; i++) {
        if (strcmp(cs->arr[i].path, path) == 0) return &cs->arr[i];
    }
    ensure_cap(cs, 1);
    TpCursor *c = &cs->arr[cs->n++];
    memset(c, 0, sizeof(*c));
    snprintf(c->path, sizeof(c->path), "%s", path);
    return c;
}

void cursors_load(TpCursorSet *cs, const char *path) {
    memset(cs, 0, sizeof(*cs));
    size_t len;
    char *txt = tp_read_all(path, &len);
    if (!txt) return;
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) return;
    int n = cJSON_GetArraySize(root);
    ensure_cap(cs, n);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(root, i);
        if (!it) continue;
        TpCursor *c = &cs->arr[cs->n++];
        memset(c, 0, sizeof(*c));
        cJSON *j;
        if ((j = cJSON_GetObjectItem(it, "path")) && j->valuestring)
            snprintf(c->path, sizeof(c->path), "%s", j->valuestring);
        if ((j = cJSON_GetObjectItem(it, "size"))) c->size = (long long)j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "offset"))) c->offset = (long long)j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "fp"))) c->fp = (uint32_t)j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "line_no"))) c->line_no = (long long)j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "cum_in"))) c->cum_in = j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "cum_cached"))) c->cum_cached = j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "cum_cw"))) c->cum_cw = j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "cum_out"))) c->cum_out = j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "cum_reason"))) c->cum_reason = j->valuedouble;
    }
    cJSON_Delete(root);
}

void cursors_save(TpCursorSet *cs, const char *path) {
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < cs->n; i++) {
        TpCursor *c = &cs->arr[i];
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "path", c->path);
        cJSON_AddNumberToObject(it, "size", (double)c->size);
        cJSON_AddNumberToObject(it, "offset", (double)c->offset);
        cJSON_AddNumberToObject(it, "fp", (double)c->fp);
        cJSON_AddNumberToObject(it, "line_no", (double)c->line_no);
        cJSON_AddNumberToObject(it, "cum_in", c->cum_in);
        cJSON_AddNumberToObject(it, "cum_cached", c->cum_cached);
        cJSON_AddNumberToObject(it, "cum_cw", c->cum_cw);
        cJSON_AddNumberToObject(it, "cum_out", c->cum_out);
        cJSON_AddNumberToObject(it, "cum_reason", c->cum_reason);
        cJSON_AddItemToArray(root, it);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;
    FILE *f = tp_fopen(path, "wb");
    if (f) {
        fwrite(out, 1, strlen(out), f);
        fclose(f);
    }
    free(out);
}

void cursors_free(TpCursorSet *cs) {
    free(cs->arr);
    cs->arr = NULL;
    cs->n = cs->cap = 0;
}
