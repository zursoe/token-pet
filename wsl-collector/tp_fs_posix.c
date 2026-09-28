#include "tp_util.h"

int64_t tp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool tp_file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

bool tp_dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t tp_file_size(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

FILE *tp_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}

int tp_fseek(FILE *f, int64_t off) {
    return fseeko(f, (off_t)off, SEEK_SET);
}

int64_t tp_ftell(FILE *f) {
    return (int64_t)ftello(f);
}

void tp_list_files(const char *dir, const char *suffix, int max_depth, tp_file_cb cb, void *ud) {
    if (max_depth < 0 || !cb) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char p[1400];
        tpu_join(p, sizeof(p), dir, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            tp_list_files(p, suffix, max_depth - 1, cb, ud);
        } else if (S_ISREG(st.st_mode)) {
            size_t pl = strlen(p), sl = suffix ? strlen(suffix) : 0;
            if (!suffix || (pl >= sl && strcmp(p + pl - sl, suffix) == 0)) {
                cb(p, ud);
            }
        }
    }
    closedir(dp);
}
