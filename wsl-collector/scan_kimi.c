#include "scan.h"
#include "cJSON.h"

static uint32_t tail_fp(const char *path, long long size) {
    if (size <= 0) return 0;
    long long start = size > 4096 ? size - 4096 : 0;
    FILE *f = tp_fopen(path, "rb");
    if (!f) return 0;
    tp_fseek(f, start);
    unsigned char buf[4096];
    size_t rd = fread(buf, 1, (size_t)(size - start), f);
    fclose(f);
    return tp_fnv32(buf, rd);
}

static bool cursor_valid(const char *path, TpCursor *c, long long size) {
    if (c->offset < 0 || c->offset > size) return false;
    if (c->size > 0 && c->size == size && c->fp != 0) {
        uint32_t cur = tail_fp(path, size);
        if (cur != c->fp) return false;
    }
    return true;
}

/* extract session id: .../sessions/<wd>/<session_id>/agents/... */
static void session_id_of(const char *path, char *out, size_t cap) {
    const char *p = strstr(path, "/sessions/");
    if (p) p += strlen("/sessions/");
    else {
        p = strstr(path, "\\sessions\\");
        if (!p) { snprintf(out, cap, "unknown"); return; }
        p += strlen("\\sessions\\");
    }
    const char *s1 = strpbrk(p, "/\\");
    if (!s1) { snprintf(out, cap, "%s", p); return; }
    const char *sid = s1 + 1;
    const char *s2 = strpbrk(sid, "/\\");
    if (!s2) { snprintf(out, cap, "%s", sid); return; }
    size_t n = (size_t)(s2 - sid);
    if (n >= cap) n = cap - 1;
    memcpy(out, sid, n);
    out[n] = 0;
}

static void kimi_process_line(const char *line, TpCursor *c, const char *sid, long long lineno) {
    (void)c;
    if (!strstr(line, "usage.record")) return;
    cJSON *d = cJSON_Parse(line);
    if (!d) return;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(d, "type");
    if (type && type->valuestring && strcmp(type->valuestring, "usage.record") == 0) {
        cJSON *u = cJSON_GetObjectItemCaseSensitive(d, "usage");
        cJSON *tm = cJSON_GetObjectItemCaseSensitive(d, "time");
        cJSON *model = cJSON_GetObjectItemCaseSensitive(d, "model");
        if (u) {
            double ino = 0, out = 0, cr = 0, cw = 0;
            cJSON *j;
            if ((j = cJSON_GetObjectItemCaseSensitive(u, "inputOther"))) ino = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(u, "output"))) out = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(u, "inputCacheRead"))) cr = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(u, "inputCacheCreation"))) cw = j->valuedouble;
            int64_t ts = tm ? (int64_t)tm->valuedouble : tp_now_ms();
            if (ino + out + cr + cw > 0) {
                char key[300];
                snprintf(key, sizeof(key), "km:%s:%lld", sid, lineno);
                emit_item(key, "kimi", sid, ts, (int64_t)ino, (int64_t)out, 0, (int64_t)cr, (int64_t)cw, 0.0);
            }
        }
        if (model && model->valuestring) {
            emit_meta("kimi", sid, NULL, model->valuestring, NULL, NULL, 0);
        }
    }
    cJSON_Delete(d);
}

typedef struct {
    TpCursorSet *cs;
    bool first_scan;
} KimiCtx;

static void kimi_file_cb(const char *path, void *ud) {
    KimiCtx *ctx = (KimiCtx *)ud;
    if (!tpu_has_dir_component(path, "agents")) return;

    TpCursor *c = cursors_get(ctx->cs, path);
    long long size = tp_file_size(path);
    if (size < 0) return;

    if (!cursor_valid(path, c, size)) {
        c->offset = 0;
        c->line_no = 0;
    }
    if (!ctx->first_scan && c->offset >= size) {
        c->size = size;
        if (c->fp == 0) c->fp = tail_fp(path, size);
        return;
    }

    FILE *f = tp_fopen(path, "rb");
    if (!f) return;
    if (tp_fseek(f, c->offset) != 0) { fclose(f); return; }

    char sid[128];
    session_id_of(path, sid, sizeof(sid));

    TpLineReader lr;
    lr_init(&lr, 1 << 20);
    lr.consumed = c->offset;
    lr.lineno = c->line_no;
    char chunk[65536];
    size_t rd;
    while ((rd = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        for (size_t i = 0; i < rd; i++) {
            if (lr_feed(&lr, (unsigned char)chunk[i])) {
                if (strstr(lr.buf, "usage.record")) kimi_process_line(lr.buf, c, sid, lr.lineno);
            }
        }
    }
    lr_free(&lr);
    fclose(f);
    c->offset = lr.consumed;
    c->line_no = lr.lineno;
    c->size = size;
    c->fp = tail_fp(path, size);
}

void scan_kimi(const char *dir, TpCursorSet *cs, bool first_scan) {
    if (!tp_dir_exists(dir)) return;
    KimiCtx ctx;
    ctx.cs = cs;
    ctx.first_scan = first_scan;
    tp_list_files(dir, "wire.jsonl", 7, kimi_file_cb, &ctx);
}
