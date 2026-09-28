#include "sources.h"
#include "cJSON.h"

void sources_cfg_path(wchar_t *out, size_t cap) {
    wchar_t exe_dir[TP_PATH_MAX];
    tp_exe_dir(exe_dir, TP_PATH_MAX);
    tp_path_join(out, cap, exe_dir, L"config\\sources.json");
}

static cJSON *load_root(void) {
    wchar_t path[TP_PATH_MAX];
    sources_cfg_path(path, TP_PATH_MAX);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { fclose(f); return NULL; }
    char *buf = (char *)xp_alloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) root = cJSON_CreateObject();
    return root;
}

static void save_root(cJSON *root) {
    wchar_t path[TP_PATH_MAX];
    sources_cfg_path(path, TP_PATH_MAX);
    char *out = cJSON_Print(root);
    if (!out) return;
    FILE *f = _wfopen(path, L"wb");
    if (f) {
        fwrite(out, 1, strlen(out), f);
        fclose(f);
    }
    free(out);
}

char *sources_cfg_get(const char *section, const char *tool) {
    cJSON *root = load_root();
    if (!root) return NULL;
    char *result = NULL;
    cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, section);
    if (sec) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(sec, tool);
        if (v && v->valuestring && v->valuestring[0]) result = xp_strdup(v->valuestring);
    }
    cJSON_Delete(root);
    return result;
}

void sources_cfg_set(const char *section, const char *tool, const char *value) {
    cJSON *root = load_root();
    if (!root) return;
    cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, section);
    if (!sec) sec = cJSON_AddObjectToObject(root, section);
    if (value && value[0]) {
        cJSON_DeleteItemFromObjectCaseSensitive(sec, tool);
        cJSON_AddStringToObject(sec, tool, value);
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(sec, tool);
    }
    save_root(root);
    cJSON_Delete(root);
}
