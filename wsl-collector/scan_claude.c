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

static cJSON *jo(cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }

static void claude_process_line(const char *line, const char *fallback_sid) {
    if (!strstr(line, "usage") && !strstr(line, "\"summary\"")) return;
    cJSON *d = cJSON_Parse(line);
    if (!d) return;
    cJSON *type = jo(d, "type");
    const char *ts = type && type->valuestring ? type->valuestring : "";
    const char *sid = fallback_sid;
    cJSON *jsid = jo(d, "sessionId");
    if (jsid && jsid->valuestring) sid = jsid->valuestring;

    if (strcmp(ts, "summary") == 0) {
        cJSON *sum = jo(d, "summary");
        if (sum && sum->valuestring && sid) emit_meta("claude", sid, sum->valuestring, NULL, NULL, NULL, 0);
    } else if (strcmp(ts, "assistant") == 0) {
        cJSON *msg = jo(d, "message");
        if (msg) {
            cJSON *usage = jo(msg, "usage");
            if (usage) {
                double in = 0, out = 0, cr = 0, cw = 0;
                cJSON *j;
                if ((j = jo(usage, "input_tokens"))) in = j->valuedouble;
                if ((j = jo(usage, "output_tokens"))) out = j->valuedouble;
                if ((j = jo(usage, "cache_read_input_tokens"))) cr = j->valuedouble;
                if ((j = jo(usage, "cache_creation_input_tokens"))) cw = j->valuedouble;
                const char *mid = NULL;
                cJSON *jid = jo(msg, "id");
                cJSON *juuid = jo(d, "uuid");
                if (jid && jid->valuestring) mid = jid->valuestring;
                else if (juuid && juuid->valuestring) mid = juuid->valuestring;
                if (mid && in + out + cr + cw > 0) {
                    cJSON *jts = jo(d, "timestamp");
                    int64_t tsms = jts && jts->valuestring ? tp_parse_iso_ms(jts->valuestring) : tp_now_ms();
                    char key[300];
                    snprintf(key, sizeof(key), "cl:%s", mid);
                    emit_item(key, "claude", sid ? sid : "", tsms,
                              (int64_t)in, (int64_t)out, 0, (int64_t)cr, (int64_t)cw, 0.0);
                }
            }
            cJSON *model = jo(msg, "model");
            cJSON *cwd = jo(d, "cwd");
            if (sid && (model || cwd)) {
                emit_meta("claude", sid, NULL,
                          model && model->valuestring ? model->valuestring : NULL, NULL,
                          cwd && cwd->valuestring ? cwd->valuestring : NULL, 0);
            }
        }
    }
    cJSON_Delete(d);
}

typedef struct {
    TpCursorSet *cs;
    bool first_scan;
} ClaudeCtx;

static void claude_file_cb(const char *path, void *ud) {
    ClaudeCtx *ctx = (ClaudeCtx *)ud;
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

    /* fallback session id = file name without .jsonl */
    const char *base = strrchr(path, '/');
    if (!base) base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    char sid[300];
    snprintf(sid, sizeof(sid), "%s", base);
    char *dot = strstr(sid, ".jsonl");
    if (dot) *dot = 0;

    TpLineReader lr;
    lr_init(&lr, 1 << 20);
    lr.consumed = c->offset;
    char chunk[65536];
    size_t rd;
    while ((rd = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        for (size_t i = 0; i < rd; i++) {
            if (lr_feed(&lr, (unsigned char)chunk[i])) {
                if (strstr(lr.buf, "usage") || strstr(lr.buf, "\"summary\"")) {
                    claude_process_line(lr.buf, sid);
                }
            }
        }
    }
    lr_free(&lr);
    fclose(f);
    c->offset = lr.consumed;
    c->size = size;
    c->fp = tail_fp(path, size);
}

void scan_claude(const char *projects_dir, TpCursorSet *cs) {
    if (!tp_dir_exists(projects_dir)) return;
    ClaudeCtx ctx;
    ctx.cs = cs;
    ctx.first_scan = false;
    tp_list_files(projects_dir, ".jsonl", 8, claude_file_cb, &ctx);
}
