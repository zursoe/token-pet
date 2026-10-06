#include "pricing.h"
#include "cJSON.h"
#include "sqlite3.h"
#include <ctype.h>

#define PRICING_MAX_RULES 64

#define CUR_CNY 0
#define CUR_USD 1

typedef struct {
    char match[64];
    int currency;
    double in, out, cr, cw;
} PriceRule;

typedef struct {
    bool loaded;
    double usd_cny;
    PriceRule dflt;
    PriceRule rules[PRICING_MAX_RULES];
    int n;
} PriceTable;

static PriceTable g_pt;

static const char *default_json =
"{\n"
"  \"comment\": \"API 价目表：单价为每 100 万 token；currency 可选 CNY/USD，USD 按 usd_cny 折算；match 为模型名子串（不区分大小写，自上而下先具体后泛化），未命中再拿工具名匹配，最后落到 default；reasoning token 按 output 计价；省略 cache_read/cache_write 时按 input 价。默认值为占位价，请按官方价格校准；修改后重启生效。\",\n"
"  \"usd_cny\": 7.2,\n"
"  \"default\": { \"currency\": \"CNY\", \"input\": 1, \"output\": 3, \"cache_read\": 0.1, \"cache_write\": 1 },\n"
"  \"models\": [\n"
"    { \"match\": \"claude-opus\", \"currency\": \"USD\", \"input\": 15, \"output\": 75, \"cache_read\": 1.5, \"cache_write\": 18.75 },\n"
"    { \"match\": \"claude-sonnet\", \"currency\": \"USD\", \"input\": 3, \"output\": 15, \"cache_read\": 0.3, \"cache_write\": 3.75 },\n"
"    { \"match\": \"claude-haiku\", \"currency\": \"USD\", \"input\": 0.8, \"output\": 4, \"cache_read\": 0.08, \"cache_write\": 1 },\n"
"    { \"match\": \"claude\", \"currency\": \"USD\", \"input\": 3, \"output\": 15, \"cache_read\": 0.3, \"cache_write\": 3.75 },\n"
"    { \"match\": \"gpt-6\", \"currency\": \"USD\", \"input\": 2, \"output\": 12, \"cache_read\": 0.2 },\n"
"    { \"match\": \"gpt-5\", \"currency\": \"USD\", \"input\": 1.25, \"output\": 10, \"cache_read\": 0.125 },\n"
"    { \"match\": \"codex\", \"currency\": \"USD\", \"input\": 1.25, \"output\": 10, \"cache_read\": 0.125 },\n"
"    { \"match\": \"gemini\", \"currency\": \"USD\", \"input\": 1.25, \"output\": 10 },\n"
"    { \"match\": \"deepseek\", \"currency\": \"CNY\", \"input\": 2, \"output\": 8, \"cache_read\": 0.5 },\n"
"    { \"match\": \"kimi\", \"currency\": \"CNY\", \"input\": 4, \"output\": 16 },\n"
"    { \"match\": \"glm\", \"currency\": \"CNY\", \"input\": 2, \"output\": 8 },\n"
"    { \"match\": \"qwen\", \"currency\": \"CNY\", \"input\": 1, \"output\": 4 },\n"
"    { \"match\": \"doubao\", \"currency\": \"CNY\", \"input\": 0.8, \"output\": 2 },\n"
"    { \"match\": \"minimax\", \"currency\": \"CNY\", \"input\": 1, \"output\": 8 }\n"
"  ]\n"
"}\n";

static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static bool ci_contains(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

static void set_defaults(void) {
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.usd_cny = 7.2;
    snprintf(g_pt.dflt.match, sizeof(g_pt.dflt.match), "default");
    g_pt.dflt.currency = CUR_CNY;
    g_pt.dflt.in = 1.0;
    g_pt.dflt.out = 3.0;
    g_pt.dflt.cr = 0.1;
    g_pt.dflt.cw = 1.0;
}

static void parse_rule(cJSON *it, PriceRule *r, const char *fallback_match) {
    memset(r, 0, sizeof(*r));
    cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "match");
    snprintf(r->match, sizeof(r->match), "%s",
             (m && m->valuestring) ? m->valuestring : (fallback_match ? fallback_match : ""));
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(it, "currency");
    r->currency = (cur && cur->valuestring && ci_equal(cur->valuestring, "USD")) ? CUR_USD : CUR_CNY;
    cJSON *j;
    r->in = (j = cJSON_GetObjectItemCaseSensitive(it, "input")) ? j->valuedouble : 1.0;
    r->out = (j = cJSON_GetObjectItemCaseSensitive(it, "output")) ? j->valuedouble : 3.0;
    r->cr = (j = cJSON_GetObjectItemCaseSensitive(it, "cache_read")) ? j->valuedouble : r->in;
    r->cw = (j = cJSON_GetObjectItemCaseSensitive(it, "cache_write")) ? j->valuedouble : r->in;
}

void pricing_load(const wchar_t *config_dir) {
    set_defaults();
    wchar_t path[TP_PATH_MAX];
    tp_path_join(path, TP_PATH_MAX, config_dir, L"pricing.json");

    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        f = _wfopen(path, L"wb");
        if (f) {
            fwrite(default_json, 1, strlen(default_json), f);
            fclose(f);
            tp_log("pricing: default pricing.json written");
        }
        g_pt.loaded = true;
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { fclose(f); g_pt.loaded = true; return; }
    char *buf = (char *)xp_alloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { g_pt.loaded = true; return; }

    cJSON *rate = cJSON_GetObjectItemCaseSensitive(root, "usd_cny");
    if (rate && rate->valuedouble > 0) g_pt.usd_cny = rate->valuedouble;

    cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "default");
    if (d && cJSON_IsObject(d)) parse_rule(d, &g_pt.dflt, "default");

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (arr && cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n && g_pt.n < PRICING_MAX_RULES; i++) {
            cJSON *it = cJSON_GetArrayItem(arr, i);
            if (!it || !cJSON_IsObject(it)) continue;
            PriceRule r;
            parse_rule(it, &r, NULL);
            if (!r.match[0]) continue;
            g_pt.rules[g_pt.n++] = r;
        }
    }
    cJSON_Delete(root);
    g_pt.loaded = true;
    tp_log("pricing: %d rules loaded, usd_cny=%.3f", g_pt.n, g_pt.usd_cny);
}

double pricing_cost_cny(const char *model, const char *tool,
                        int64_t tin, int64_t tout, int64_t tr, int64_t cr, int64_t cw) {
    if (!g_pt.loaded) return 0.0;
    const PriceRule *r = NULL;
    if (model && model[0]) {
        for (int i = 0; i < g_pt.n; i++) {
            if (ci_contains(model, g_pt.rules[i].match)) { r = &g_pt.rules[i]; break; }
        }
    }
    if (!r && tool && tool[0]) {
        for (int i = 0; i < g_pt.n; i++) {
            if (ci_contains(tool, g_pt.rules[i].match)) { r = &g_pt.rules[i]; break; }
        }
    }
    if (!r) r = &g_pt.dflt;
    double c = ((double)tin * r->in + (double)(tout + tr) * r->out +
                (double)cr * r->cr + (double)cw * r->cw) / 1e6;
    if (r->currency == CUR_USD) c *= g_pt.usd_cny;
    return c;
}

static void udf_cost_cny(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    const char *model = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : "";
    const char *tool = argc > 1 ? (const char *)sqlite3_value_text(argv[1]) : "";
    int64_t tin = argc > 2 ? sqlite3_value_int64(argv[2]) : 0;
    int64_t tout = argc > 3 ? sqlite3_value_int64(argv[3]) : 0;
    int64_t tr = argc > 4 ? sqlite3_value_int64(argv[4]) : 0;
    int64_t cr = argc > 5 ? sqlite3_value_int64(argv[5]) : 0;
    int64_t cw = argc > 6 ? sqlite3_value_int64(argv[6]) : 0;
    sqlite3_result_double(ctx, pricing_cost_cny(model, tool, tin, tout, tr, cr, cw));
}

void pricing_register(void *db) {
    if (!db) return;
    sqlite3_create_function((sqlite3 *)db, "cost_cny", 7,
                            SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                            udf_cost_cny, NULL, NULL);
}
