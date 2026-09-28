#include "common.h"
#include "db.h"
#include "realm.h"
#include "win_scan.h"
#include "bridge.h"
#include "sqlite3.h"

static void fmt_big(char *out, size_t cap, int64_t v) {
    if (v < 10000) snprintf(out, cap, "%lld", (long long)v);
    else if (v < 100000000LL) snprintf(out, cap, "%.1f万", (double)v / 10000.0);
    else if (v < 1000000000000LL) snprintf(out, cap, "%.2f亿", (double)v / 100000000.0);
    else snprintf(out, cap, "%.2f万亿", (double)v / 1000000000000.0);
}

int scan_report_run(void) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t exe_dir[TP_PATH_MAX], config_dir[TP_PATH_MAX], db_path[TP_PATH_MAX];
    tp_exe_dir(exe_dir, TP_PATH_MAX);
    tp_path_join(config_dir, TP_PATH_MAX, exe_dir, L"config");
    tp_path_join(db_path, TP_PATH_MAX, exe_dir, L"data\\pet.db");

    /* also tee the report to data\scan-report.txt (console may be detached) */
    {
        wchar_t report_path[TP_PATH_MAX];
        tp_path_join(report_path, TP_PATH_MAX, exe_dir, L"data\\scan-report.txt");
        FILE *rf = NULL;
        _wfreopen_s(&rf, report_path, L"w", stdout);
    }

    Db *db = db_open(db_path);
    if (!db) {
        printf("[scan-report] 无法打开数据库\n");
        return 1;
    }
    RealmTable realm;
    realm_load(&realm, config_dir);

    printf("[scan-report] 扫描 Windows 侧数据源...\n");
    win_scan_init(db, config_dir);
    win_scan_once();

    printf("[scan-report] 运行 WSL 采集器 (--once)...\n");
    bridge_run_once_sync(db, g_settings.wsl_distro);

    printf("\n===== Token-Pet 扫描报告 =====\n");
    printf("%-10s %8s %18s\n", "工具", "条目", "修为(token)");
    int64_t grand = 0;
    db_lock(db);
    sqlite3 *raw = (sqlite3 *)db_raw(db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT tool, COUNT(*), COALESCE(SUM(tin+tout+tr+cr+cw),0) FROM items GROUP BY tool ORDER BY 3 DESC",
            -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *tool = (const char *)sqlite3_column_text(st, 0);
            int64_t n = sqlite3_column_int64(st, 1);
            int64_t xp = sqlite3_column_int64(st, 2);
            grand += xp;
            char big[64];
            fmt_big(big, sizeof(big), xp);
            printf("%-10s %8lld %18s\n", tool ? tool : "?", (long long)n, big);
        }
        sqlite3_finalize(st);
    }
    printf("------------------------------------------\n");
    {
        char big[64];
        fmt_big(big, sizeof(big), grand);
        printf("总修为: %s\n", big);
    }
    wchar_t realm_name[64];
    realm_compute(&realm, grand, realm_name, 64, NULL, NULL, NULL, NULL);
    char *rn = tp_wide_to_utf8(realm_name);
    printf("当前境界: %s\n", rn);
    free(rn);

    printf("\n--- 修为最高的会话 ---\n");
    if (sqlite3_prepare_v2(raw,
            "SELECT i.tool, i.sid, COALESCE(m.title,''), COALESCE(m.model,''),"
            " COALESCE(SUM(i.tin+i.tout+i.tr+i.cr+i.cw),0) AS xp"
            " FROM items i LEFT JOIN meta m ON m.tool=i.tool AND m.sid=i.sid"
            " GROUP BY i.tool, i.sid ORDER BY xp DESC LIMIT 8",
            -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *tool = (const char *)sqlite3_column_text(st, 0);
            const char *sid = (const char *)sqlite3_column_text(st, 1);
            const char *title = (const char *)sqlite3_column_text(st, 2);
            const char *model = (const char *)sqlite3_column_text(st, 3);
            int64_t xp = sqlite3_column_int64(st, 4);
            char big[64];
            fmt_big(big, sizeof(big), xp);
            printf("  [%s] %-9s %s | %s\n", tool ? tool : "?", big,
                   (title && title[0]) ? title : (sid ? sid : "?"),
                   (model && model[0]) ? model : "-");
        }
        sqlite3_finalize(st);
    }
    db_unlock(db);

    db_close(db);
    return 0;
}
