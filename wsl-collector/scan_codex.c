#include "scan.h"
#include "cJSON.h"

/* ---------- helpers ---------- */

static void stem_of(const char *path, char *out, size_t cap) {
    const char *base = tpu_basename(path);
    snprintf(out, cap, "%s", base);
    char *dot = strstr(out, ".jsonl");
    if (dot) *dot = 0;
}

/* thread id = last 36 chars of rollout stem (uuid) */
static const char *thread_id_of(const char *stem) {
    size_t n = strlen(stem);
    if (n >= 36) return stem + n - 36;
    return stem;
}

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
static double jnum(cJSON *o, const char *k) {
    cJSON *j = jo(o, k);
    return j ? j->valuedouble : 0;
}

/* returns thread id from session_meta line (or NULL) */
static const char *meta_id(cJSON *d, char *out, size_t cap) {
    cJSON *p = jo(d, "payload");
    if (!p) return NULL;
    cJSON *id = jo(p, "id");
    if (id && id->valuestring) {
        snprintf(out, cap, "%s", id->valuestring);
        return out;
    }
    return NULL;
}

/* ---------- codex file scan ---------- */

typedef struct {
    char id[64];
    char title[512];
} TitleEntry;

typedef struct {
    TpCursorSet *cs;
    bool first_scan;
    long long files;
    long long total_files;
    TitleEntry *titles;
    int n_titles;
} CodexCtx;

static void codex_process_line(const char *line, TpCursor *c, const char *stem, CodexCtx *ctx) {
    const char *tid = thread_id_of(stem);
    cJSON *d = cJSON_Parse(line);
    if (!d) return;
    cJSON *type = jo(d, "type");
    const char *ts = type && type->valuestring ? type->valuestring : "";

    if (strcmp(ts, "session_meta") == 0) {
        char idbuf[64] = "";
        const char *id = meta_id(d, idbuf, sizeof(idbuf));
        cJSON *p = jo(d, "payload");
        cJSON *cwd = p ? jo(p, "cwd") : NULL;
        const char *title = NULL;
        if (id) {
            for (int i = 0; i < ctx->n_titles; i++) {
                if (strcmp(ctx->titles[i].id, id) == 0) { title = ctx->titles[i].title; break; }
            }
            if (title) emit_meta("codex", id, title, NULL, NULL, cwd && cwd->valuestring ? cwd->valuestring : NULL, 0);
            else emit_meta("codex", id, NULL, NULL, NULL, cwd && cwd->valuestring ? cwd->valuestring : NULL, 0);
        }
    } else if (strcmp(ts, "turn_context") == 0) {
        cJSON *p = jo(d, "payload");
        cJSON *m = p ? jo(p, "model") : NULL;
        if (m && m->valuestring) emit_meta("codex", tid, NULL, m->valuestring, NULL, NULL, 0);
    } else if (strstr(line, "\"token_count\"")) {
        cJSON *p = jo(d, "payload");
        if (p) {
            cJSON *pt = jo(p, "type");
            if (pt && pt->valuestring && strcmp(pt->valuestring, "token_count") == 0) {
                cJSON *info = jo(p, "info");
                cJSON *tot = info ? jo(info, "total_token_usage") : NULL;
                if (tot) {
                    double in = jnum(tot, "input_tokens");
                    double cached = jnum(tot, "cached_input_tokens");
                    double cw = jnum(tot, "cache_write_input_tokens");
                    double out = jnum(tot, "output_tokens");
                    double reason = jnum(tot, "reasoning_output_tokens");
                    double total = jnum(tot, "total_tokens");

                    double d_in = in - c->cum_in;
                    double d_cached = cached - c->cum_cached;
                    double d_cw = cw - c->cum_cw;
                    double d_out = out - c->cum_out;
                    double d_reason = reason - c->cum_reason;
                    if (d_in < 0 || d_cached < 0 || d_cw < 0 || d_out < 0 || d_reason < 0) {
                        d_in = d_cached = d_cw = d_out = d_reason = 0;
                    }
                    c->cum_in = in;
                    c->cum_cached = cached;
                    c->cum_cw = cw;
                    c->cum_out = out;
                    c->cum_reason = reason;

                    int64_t tin = (int64_t)(d_in - d_cached > 0 ? d_in - d_cached : 0);
                    int64_t cr = (int64_t)(d_cached > 0 ? d_cached : 0);
                    int64_t cwv = (int64_t)(d_cw > 0 ? d_cw : 0);
                    int64_t tout = (int64_t)(d_out - d_reason > 0 ? d_out - d_reason : 0);
                    int64_t tr = (int64_t)(d_reason > 0 ? d_reason : 0);

                    if (tin + tout + tr + cr + cwv > 0) {
                        cJSON *jts = jo(d, "timestamp");
                        int64_t tsms = jts && jts->valuestring ? tp_parse_iso_ms(jts->valuestring) : tp_now_ms();
                        char key[1400];
                        snprintf(key, sizeof(key), "cx:%s:%s:%.0f", stem,
                                 jts && jts->valuestring ? jts->valuestring : "0", total);
                        emit_item(key, "codex", tid, tsms, tin, tout, tr, cr, cwv, 0.0);
                    }
                }
            }
        }
    }
    cJSON_Delete(d);
}

static void codex_file_cb(const char *path, void *ud) {
    CodexCtx *ctx = (CodexCtx *)ud;
    const char *base = tpu_basename(path);
    if (strncmp(base, "rollout-", 8) != 0 || !strstr(base, ".jsonl")) return;

    ctx->files++;
    if ((ctx->files % 20) == 0) emit_status("scan", "codex", ctx->files, ctx->total_files);

    TpCursor *c = cursors_get(ctx->cs, path);
    long long size = tp_file_size(path);
    if (size < 0) return;

    bool full = false;
    if (!cursor_valid(path, c, size)) {
        c->offset = 0;
        c->line_no = 0;
        c->cum_in = c->cum_cached = c->cum_cw = c->cum_out = c->cum_reason = 0;
        full = true;
    }
    if (!ctx->first_scan && !full && c->offset >= size) {
        c->size = size;
        if (c->fp == 0) c->fp = tail_fp(path, size);
        return;
    }

    FILE *f = tp_fopen(path, "rb");
    if (!f) return;
    if (tp_fseek(f, c->offset) != 0) { fclose(f); return; }

    char stem[300];
    stem_of(path, stem, sizeof(stem));

    TpLineReader lr;
    lr_init(&lr, 1 << 20);
    lr.consumed = c->offset;
    char chunk[65536];
    size_t rd;
    while ((rd = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        for (size_t i = 0; i < rd; i++) {
            if (lr_feed(&lr, (unsigned char)chunk[i])) {
                if (strstr(lr.buf, "\"token_count\"") || strstr(lr.buf, "\"session_meta\"") ||
                    strstr(lr.buf, "\"turn_context\"")) {
                    codex_process_line(lr.buf, c, stem, ctx);
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

static void count_cb(const char *path, void *ud) {
    const char *base = tpu_basename(path);
    if (strncmp(base, "rollout-", 8) == 0 && strstr(base, ".jsonl")) {
        (*(long long *)ud)++;
    }
}

static void load_titles(const char *dir, CodexCtx *ctx) {
    char p[1200];
    tpu_join(p, sizeof(p), dir, "session_index.jsonl");
    if (!tp_file_exists(p)) return;
    FILE *f = tp_fopen(p, "rb");
    if (!f) return;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "thread_name")) continue;
        cJSON *d = cJSON_Parse(line);
        if (!d) continue;
        cJSON *id = jo(d, "id");
        cJSON *tn = jo(d, "thread_name");
        if (id && id->valuestring && tn && tn->valuestring) {
            if (ctx->n_titles % 64 == 0) {
                ctx->titles = (TitleEntry *)realloc(ctx->titles, (size_t)(ctx->n_titles + 64) * sizeof(TitleEntry));
            }
            TitleEntry *e = &ctx->titles[ctx->n_titles++];
            snprintf(e->id, sizeof(e->id), "%s", id->valuestring);
            snprintf(e->title, sizeof(e->title), "%s", tn->valuestring);
        }
        cJSON_Delete(d);
    }
    fclose(f);
}

void scan_codex(const char *dir, TpCursorSet *cs, bool first_scan) {
    if (!tp_dir_exists(dir)) return;

    CodexCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cs = cs;
    ctx.first_scan = first_scan;

    load_titles(dir, &ctx);

    char sub[1200];
    tpu_join(sub, sizeof(sub), dir, "sessions");
    if (tp_dir_exists(sub)) {
        long long total = 0;
        tp_list_files(sub, ".jsonl", 10, count_cb, &total);
        ctx.total_files = total;
        tp_list_files(sub, ".jsonl", 10, codex_file_cb, &ctx);
    }
    tpu_join(sub, sizeof(sub), dir, "archived_sessions");
    if (tp_dir_exists(sub)) {
        long long total = 0;
        tp_list_files(sub, ".jsonl", 4, count_cb, &total);
        ctx.total_files = total;
        tp_list_files(sub, ".jsonl", 4, codex_file_cb, &ctx);
    }
    free(ctx.titles);
}
