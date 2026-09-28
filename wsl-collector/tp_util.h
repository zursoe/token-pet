#ifndef TP_UTIL_H
#define TP_UTIL_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#endif

int64_t tp_now_ms(void);
void tpu_join(char *out, size_t cap, const char *a, const char *b);
bool tp_file_exists(const char *p);
bool tp_dir_exists(const char *p);
int64_t tp_file_size(const char *p);
uint32_t tp_fnv32(const unsigned char *d, size_t n);
int64_t tp_parse_iso_ms(const char *iso);
char *tp_read_all(const char *path, size_t *len);

/* platform file wrappers */
FILE *tp_fopen(const char *path, const char *mode);
int   tp_fseek(FILE *f, int64_t off);
int64_t tp_ftell(FILE *f);

/* path component helpers (handle both / and \\ separators) */
const char *tpu_basename(const char *path);
bool tpu_has_dir_component(const char *path, const char *comp);

/* list files recursively; cb receives full path; suffix filter (NULL = all) */
typedef void (*tp_file_cb)(const char *path, void *ud);
void tp_list_files(const char *dir, const char *suffix, int max_depth, tp_file_cb cb, void *ud);

/* streaming line reader: handles arbitrarily long lines with bounded buffer */
typedef struct {
    char *buf;
    size_t cap, len;
    bool overflow;
    long long consumed; /* bytes through last newline */
    long long lineno;
} TpLineReader;

void lr_init(TpLineReader *lr, size_t cap);
void lr_free(TpLineReader *lr);
int lr_feed(TpLineReader *lr, int c);

/* cursor store */
typedef struct {
    char path[1024];
    long long size;
    long long offset;
    uint32_t fp;
    long long line_no;
    double cum_in, cum_cached, cum_cw, cum_out, cum_reason;
} TpCursor;

typedef struct {
    TpCursor *arr;
    int n, cap;
} TpCursorSet;

void cursors_load(TpCursorSet *cs, const char *path);
void cursors_save(TpCursorSet *cs, const char *path);
TpCursor *cursors_get(TpCursorSet *cs, const char *path);
void cursors_free(TpCursorSet *cs);

#endif
