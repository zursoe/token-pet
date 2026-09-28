#include "bridge.h"
#include "scan.h"
#include "cJSON.h"
#include "sources.h"
#include "win_scan.h"
#include <ctype.h>

static Db *g_db = NULL;
static wchar_t g_distro[64] = L"Debian";
static HANDLE g_stop_event = NULL;
static HANDLE g_proc = NULL;
static HANDLE g_thread = NULL;
static volatile LONG g_reset_pending = 0;
static bool g_first_done = false;

/* convert C:\Users\x\... to /mnt/c/Users/x/... */
static void win_path_to_wsl(const wchar_t *win, wchar_t *out, size_t cap) {
    wchar_t tmp[TP_PATH_MAX];
    wcsncpy(tmp, win, TP_PATH_MAX - 1);
    tmp[TP_PATH_MAX - 1] = 0;
    for (int i = 0; tmp[i]; i++) if (tmp[i] == L'\\') tmp[i] = L'/';
    if (tmp[0] && tmp[1] == L':') {
        wchar_t drive = (wchar_t)towlower(tmp[0]);
        _snwprintf(out, cap, L"/mnt/%c/%s", drive, tmp + 3);
    } else {
        wcsncpy(out, tmp, cap - 1);
        out[cap - 1] = 0;
    }
}

static void dispatch_line(const char *line) {
    cJSON *d = cJSON_Parse(line);
    if (!d) return;
    cJSON *t = cJSON_GetObjectItemCaseSensitive(d, "t");
    const char *type = t && t->valuestring ? t->valuestring : "";

    if (strcmp(type, "item") == 0) {
        cJSON *j;
        const char *key = (j = cJSON_GetObjectItem(d, "key")) && j->valuestring ? j->valuestring : NULL;
        const char *tool = (j = cJSON_GetObjectItem(d, "tool")) && j->valuestring ? j->valuestring : NULL;
        const char *sid = (j = cJSON_GetObjectItem(d, "sid")) && j->valuestring ? j->valuestring : "";
        if (key && tool) {
            emit_item(key, tool, sid,
                      (int64_t)((j = cJSON_GetObjectItem(d, "ts")) ? j->valuedouble : 0),
                      (int64_t)((j = cJSON_GetObjectItem(d, "tin")) ? j->valuedouble : 0),
                      (int64_t)((j = cJSON_GetObjectItem(d, "tout")) ? j->valuedouble : 0),
                      (int64_t)((j = cJSON_GetObjectItem(d, "tr")) ? j->valuedouble : 0),
                      (int64_t)((j = cJSON_GetObjectItem(d, "cr")) ? j->valuedouble : 0),
                      (int64_t)((j = cJSON_GetObjectItem(d, "cw")) ? j->valuedouble : 0),
                      (j = cJSON_GetObjectItem(d, "cost")) ? j->valuedouble : 0.0);
        }
    } else if (strcmp(type, "meta") == 0) {
        cJSON *j;
        const char *tool = (j = cJSON_GetObjectItem(d, "tool")) && j->valuestring ? j->valuestring : NULL;
        const char *sid = (j = cJSON_GetObjectItem(d, "sid")) && j->valuestring ? j->valuestring : NULL;
        if (!tool || !sid) { cJSON_Delete(d); return; }
        const char *title = (j = cJSON_GetObjectItem(d, "title")) && j->valuestring ? j->valuestring : NULL;
        const char *model = (j = cJSON_GetObjectItem(d, "model")) && j->valuestring ? j->valuestring : NULL;
        const char *provider = (j = cJSON_GetObjectItem(d, "provider")) && j->valuestring ? j->valuestring : NULL;
        const char *dir = (j = cJSON_GetObjectItem(d, "dir")) && j->valuestring ? j->valuestring : NULL;
        int64_t updated = (j = cJSON_GetObjectItem(d, "updated")) ? (int64_t)j->valuedouble : 0;
        emit_meta(tool, sid, title, model, provider, dir, updated);
    } else if (strcmp(type, "paths") == 0) {
        cJSON *items = cJSON_GetObjectItem(d, "items");
        int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(items, i);
            const char *tool = (cJSON_GetObjectItem(it, "tool") && cJSON_GetObjectItem(it, "tool")->valuestring) ? cJSON_GetObjectItem(it, "tool")->valuestring : NULL;
            const char *path = (cJSON_GetObjectItem(it, "path") && cJSON_GetObjectItem(it, "path")->valuestring) ? cJSON_GetObjectItem(it, "path")->valuestring : NULL;
            cJSON *ex = cJSON_GetObjectItem(it, "exists");
            if (!tool || !path) continue;
            char label[96];
            snprintf(label, sizeof(label), "WSL %ls", g_distro);
            db_source_upsert(g_db, tool, label, path, "WSL", "auto", (ex && cJSON_IsTrue(ex)) ? 1 : 0,
                             (ex && cJSON_IsTrue(ex)) ? "ok" : "missing");
        }
    } else if (strcmp(type, "done") == 0) {
        if (!g_first_done) {
            g_first_done = true;
            if (g_startup_cycles_pending > 0) InterlockedDecrement(&g_startup_cycles_pending);
        }
    } else if (strcmp(type, "status") == 0) {
        cJSON *phase = cJSON_GetObjectItem(d, "phase");
        if (phase && phase->valuestring) {
            db_state_set(g_db, "collector_phase", phase->valuestring);
        }
    }
    cJSON_Delete(d);
}

static bool write_config_line(HANDLE stdin_w) {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "t", "config");
    cJSON_AddNumberToObject(cfg, "poll_ms", g_settings.poll_ms > 500 ? g_settings.poll_ms : 2500);
    char *ov;
    ov = sources_cfg_get("wsl", "opencode");
    cJSON_AddStringToObject(cfg, "opencode_db", ov ? ov : "");
    free(ov);
    ov = sources_cfg_get("wsl", "codex");
    cJSON_AddStringToObject(cfg, "codex_dir", ov ? ov : "");
    free(ov);
    ov = sources_cfg_get("wsl", "kimi");
    cJSON_AddStringToObject(cfg, "kimi_dir", ov ? ov : "");
    free(ov);
    bool reset = InterlockedExchange(&g_reset_pending, 0) != 0;
    cJSON_AddBoolToObject(cfg, "reset_cursors", reset);
    char *s = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    if (!s) return false;
    size_t len = strlen(s);
    char *buf = (char *)xp_alloc(len + 2);
    memcpy(buf, s, len);
    buf[len] = '\n';
    DWORD wr = 0;
    BOOL ok = WriteFile(stdin_w, buf, (DWORD)(len + 1), &wr, NULL);
    free(s);
    free(buf);
    return ok == TRUE;
}

static bool file_exists_wide(const wchar_t *path) {
    char *u = tp_wide_to_utf8(path);
    bool ok = tp_file_exists_utf8(u);
    free(u);
    return ok;
}

static const wchar_t *find_collector(wchar_t *out, size_t cap) {
    static const wchar_t *cands[] = {
        L"wsl-collector\\pet-collector",
        L"..\\wsl-collector\\pet-collector",
        L"..\\..\\wsl-collector\\pet-collector",
    };
    wchar_t exe_dir[TP_PATH_MAX];
    tp_exe_dir(exe_dir, TP_PATH_MAX);
    for (int i = 0; i < 3; i++) {
        tp_path_join(out, cap, exe_dir, cands[i]);
        if (file_exists_wide(out)) return out;
    }
    tp_path_join(out, cap, exe_dir, cands[0]);
    return NULL;
}

static DWORD WINAPI bridge_thread(LPVOID param) {
    (void)param;
    for (;;) {
        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) break;

        /* build collector path */
        wchar_t exe_path[TP_PATH_MAX], wsl_exe[TP_PATH_MAX];
        const wchar_t *found = find_collector(exe_path, TP_PATH_MAX);
        win_path_to_wsl(found ? found : exe_path, wsl_exe, TP_PATH_MAX);

        SECURITY_ATTRIBUTES sa;
        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE out_r = NULL, out_w = NULL, in_r = NULL, in_w = NULL;
        if (!CreatePipe(&out_r, &out_w, &sa, 1 << 16)) { Sleep(10000); continue; }
        if (!CreatePipe(&in_r, &in_w, &sa, 1 << 12)) { CloseHandle(out_r); CloseHandle(out_w); Sleep(10000); continue; }
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

        wchar_t cmd[2400];
        _snwprintf(cmd, 2400, L"wsl.exe -d %s -- \"%s\"", g_distro, wsl_exe);
        cmd[2399] = 0;

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r;
        si.hStdOutput = out_w;
        si.hStdError = out_w;
        memset(&pi, 0, sizeof(pi));

        if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            tp_log("bridge: wsl.exe spawn failed (err=%lu)", GetLastError());
            CloseHandle(out_r); CloseHandle(out_w); CloseHandle(in_r); CloseHandle(in_w);
            Sleep(15000);
            continue;
        }
        CloseHandle(out_w);
        CloseHandle(in_r);
        g_proc = pi.hProcess;

        write_config_line(in_w);
        tp_log("bridge: collector started (pid=%lu) cmd=%ls", pi.dwProcessId, cmd);

        TpLineReader lr;
        lr_init(&lr, 1 << 20);
        char buf[65536];
        DWORD rd = 0;
        bool first_chunk = true;
        while (ReadFile(out_r, buf, sizeof(buf), &rd, NULL) && rd > 0) {
            if (first_chunk) {
                char dbg[300];
                DWORD n = rd < 200 ? rd : 200;
                memcpy(dbg, buf, n);
                dbg[n] = 0;
                tp_log("bridge: first output: %.200s", dbg);
                first_chunk = false;
            }
            for (DWORD i = 0; i < rd; i++) {
                if (lr_feed(&lr, (unsigned char)buf[i])) {
                    dispatch_line(lr.buf);
                }
            }
            if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) break;
        }
        lr_free(&lr);
        DWORD ec = 0;
        if (g_proc) GetExitCodeProcess(g_proc, &ec);
        tp_log("bridge: collector exited (code=%lu, lastErr=%lu, readBytes=%lu)", ec, GetLastError(), rd);

        if (g_proc) { TerminateProcess(g_proc, 0); CloseHandle(g_proc); g_proc = NULL; }
        CloseHandle(pi.hThread);
        CloseHandle(out_r);
        CloseHandle(in_w);

        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) break;
        WaitForSingleObject(g_stop_event, 5000); /* backoff */
    }
    return 0;
}

void bridge_start(Db *db, const wchar_t *distro, bool reset_cursors) {
    g_db = db;
    if (distro && distro[0]) wcsncpy(g_distro, distro, 63);
    if (reset_cursors) InterlockedExchange(&g_reset_pending, 1);
    g_first_done = false;
    if (g_stop_event) CloseHandle(g_stop_event);
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_thread) CloseHandle(g_thread);
    g_thread = CreateThread(NULL, 0, bridge_thread, NULL, 0, NULL);
}

/* synchronous single pass: wsl collector --once, used by --scan-report */
void bridge_run_once_sync(Db *db, const wchar_t *distro) {
    g_db = db;
    if (distro && distro[0]) wcsncpy(g_distro, distro, 63);
    wchar_t exe_path[TP_PATH_MAX], wsl_exe[TP_PATH_MAX];
    const wchar_t *found = find_collector(exe_path, TP_PATH_MAX);
    win_path_to_wsl(found ? found : exe_path, wsl_exe, TP_PATH_MAX);

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_r = NULL, out_w = NULL, in_r = NULL, in_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 16)) return;
    if (!CreatePipe(&in_r, &in_w, &sa, 1 << 12)) { CloseHandle(out_r); CloseHandle(out_w); return; }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    wchar_t cmd[2400];
    _snwprintf(cmd, 2400, L"wsl.exe -d %s -- \"%s\" --once", g_distro, wsl_exe);
    cmd[2399] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(out_r); CloseHandle(out_w); CloseHandle(in_r); CloseHandle(in_w);
        return;
    }
    CloseHandle(out_w);
    CloseHandle(in_r);

    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "t", "config");
    cJSON_AddNumberToObject(cfg, "poll_ms", 2500);
    char *cs = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    if (cs) {
        DWORD wr = 0;
        WriteFile(in_w, cs, (DWORD)strlen(cs), &wr, NULL);
        WriteFile(in_w, "\n", 1, &wr, NULL);
        free(cs);
    }
    CloseHandle(in_w);

    TpLineReader lr;
    lr_init(&lr, 1 << 20);
    char buf[65536];
    DWORD rd = 0;
    while (ReadFile(out_r, buf, sizeof(buf), &rd, NULL) && rd > 0) {
        for (DWORD i = 0; i < rd; i++) {
            if (lr_feed(&lr, (unsigned char)buf[i])) dispatch_line(lr.buf);
        }
    }
    lr_free(&lr);
    CloseHandle(out_r);
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void bridge_stop(void) {
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_proc) TerminateProcess(g_proc, 0);
}

void bridge_set_reset_on_next(void) {
    InterlockedExchange(&g_reset_pending, 1);
}
