#ifndef TOKENPET_SOURCES_H
#define TOKENPET_SOURCES_H

#include "common.h"

/* config/sources.json: {"win": {"codex": "D:\\..."}, "wsl": {"codex": "/home/..."}} */

void sources_cfg_path(wchar_t *out, size_t cap);
/* returns malloc'd override value or NULL */
char *sources_cfg_get(const char *section, const char *tool);
/* set override; value NULL or "" removes it */
void sources_cfg_set(const char *section, const char *tool, const char *value);
/* copy sources.json "wsl_extras" array into cfg (cJSON object); cfg passed as void* */
void sources_cfg_add_extras(void *cfg);

#endif
