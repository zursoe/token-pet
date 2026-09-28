#include "anim.h"
#include "cJSON.h"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <math.h>

using namespace Gdiplus;

/* sprite pack: 0 stand, 1 blink, 2 meditate, 3 sword, 4 breakthrough */
static Image *g_sprites[5] = { NULL, NULL, NULL, NULL, NULL };
static bool g_sprite_mode = false;

/* v2 action-sequence engine */
static void a2_load(const wchar_t *dir, cJSON *actions);
static void a2_start(const char *name, bool fade);
static void a2_update(float dt);
static void a2_draw(Graphics &g, PetAnim *a);
static bool a2_has(const char *name);

extern "C" void anim_load_sprites(const wchar_t *dir) {
    if (!dir || !dir[0]) return;
    wchar_t manifest[TP_PATH_MAX];
    _snwprintf(manifest, TP_PATH_MAX, L"%s\\manifest.json", dir);
    manifest[TP_PATH_MAX - 1] = 0;
    FILE *f = _wfopen(manifest, L"rb");
    if (!f) {
        tp_log("sprites: manifest not found, procedural mode");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { fclose(f); return; }
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    cJSON *frames = cJSON_GetObjectItem(root, "frames");
    cJSON *actions = cJSON_GetObjectItem(root, "actions");
    if (mode && mode->valuestring && strcmp(mode->valuestring, "anim2") == 0 && actions) {
        a2_load(dir, actions);
    } else if (mode && mode->valuestring && strcmp(mode->valuestring, "sprites") == 0 && frames) {
        const char *keys[5] = { "stand", "blink", "meditate", "sword", "breakthrough" };
        for (int i = 0; i < 5; i++) {
            cJSON *j = cJSON_GetObjectItem(frames, keys[i]);
            if (!j || !j->valuestring) continue;
            wchar_t *rel = tp_utf8_to_wide(j->valuestring);
            for (wchar_t *p = rel; *p; p++) if (*p == L'/') *p = L'\\';
            wchar_t path[TP_PATH_MAX];
            _snwprintf(path, TP_PATH_MAX, L"%s\\%s", dir, rel);
            path[TP_PATH_MAX - 1] = 0;
            free(rel);
            Image *img = Image::FromFile(path);
            if (img && img->GetLastStatus() == Ok) g_sprites[i] = img;
            else if (img) delete img;
        }
        g_sprite_mode = g_sprites[0] != NULL;
        tp_log("sprites: %s (stand=%d blink=%d meditate=%d sword=%d breakthrough=%d)",
               g_sprite_mode ? "loaded" : "incomplete",
               g_sprites[0] != NULL, g_sprites[1] != NULL, g_sprites[2] != NULL,
               g_sprites[3] != NULL, g_sprites[4] != NULL);
    }
    cJSON_Delete(root);
}

/* ================= v2 action-sequence engine ================= */

#define A2_MAX_ACTIONS 12
#define A2_MAX_FRAMES 12

typedef struct {
    char name[32];
    Image *frames[A2_MAX_FRAMES];
    int n;
    float fps;
    bool loop;
    char next[32];
} A2Action;

static A2Action g_a2[A2_MAX_ACTIONS];
static int g_a2_n = 0;
static bool g_anim2 = false;
static char g_a2_cur[32] = "idle";
static char g_a2_prev[32] = "";
static float g_a2_clock = 0;
static float g_a2_fade = 0;
static float g_a2_blink = 4.0f;
static float g_a2_idle = 16.0f;
static float g_a2_hold = 0;

static A2Action *a2_find(const char *name) {
    for (int i = 0; i < g_a2_n; i++)
        if (strcmp(g_a2[i].name, name) == 0) return &g_a2[i];
    return NULL;
}

static bool a2_has(const char *name) { return a2_find(name) != NULL; }

static void a2_load(const wchar_t *dir, cJSON *actions) {
    g_a2_n = 0;
    cJSON *act = NULL;
    cJSON_ArrayForEach(act, actions) {
        if (g_a2_n >= A2_MAX_ACTIONS) break;
        const char *name = act->string;
        cJSON *jf = cJSON_GetObjectItem(act, "frames");
        if (!name || !jf) continue;
        A2Action *A = &g_a2[g_a2_n];
        memset(A, 0, sizeof(*A));
        snprintf(A->name, sizeof(A->name), "%s", name);
        cJSON *jfps = cJSON_GetObjectItem(act, "fps");
        A->fps = (jfps && jfps->valuedouble > 0.1) ? (float)jfps->valuedouble : 4.0f;
        cJSON *jloop = cJSON_GetObjectItem(act, "loop");
        A->loop = jloop && cJSON_IsTrue(jloop);
        cJSON *jnext = cJSON_GetObjectItem(act, "next");
        if (jnext && jnext->valuestring) snprintf(A->next, sizeof(A->next), "%s", jnext->valuestring);
        int fn = cJSON_GetArraySize(jf);
        for (int i = 0; i < fn && i < A2_MAX_FRAMES; i++) {
            cJSON *fp = cJSON_GetArrayItem(jf, i);
            if (!fp || !fp->valuestring) continue;
            wchar_t *rel = tp_utf8_to_wide(fp->valuestring);
            for (wchar_t *q = rel; *q; q++) if (*q == L'/') *q = L'\\';
            wchar_t path[TP_PATH_MAX];
            _snwprintf(path, TP_PATH_MAX, L"%s\\%s", dir, rel);
            path[TP_PATH_MAX - 1] = 0;
            free(rel);
            Image *img = Image::FromFile(path);
            if (img && img->GetLastStatus() == Ok) A->frames[A->n++] = img;
            else if (img) delete img;
        }
        if (A->n > 0) g_a2_n++;
    }
    g_anim2 = g_a2_n > 0 && a2_find("idle") != NULL;
    if (g_anim2) {
        snprintf(g_a2_cur, sizeof(g_a2_cur), "idle");
        g_a2_clock = 0;
        g_a2_fade = 0;
    }
    tp_log("anim2: %s (%d actions)", g_anim2 ? "loaded" : "incomplete", g_a2_n);
}

static void a2_start(const char *name, bool fade) {
    if (!a2_has(name)) return;
    snprintf(g_a2_prev, sizeof(g_a2_prev), "%s", g_a2_cur);
    snprintf(g_a2_cur, sizeof(g_a2_cur), "%s", name);
    g_a2_clock = 0;
    g_a2_fade = fade ? 0.28f : 0;
}

static void a2_update(float dt) {
    if (!g_anim2) return;
    A2Action *A = a2_find(g_a2_cur);
    if (!A) { snprintf(g_a2_cur, sizeof(g_a2_cur), "idle"); g_a2_clock = 0; return; }
    g_a2_clock += dt;
    if (g_a2_fade > 0) g_a2_fade -= dt;

    float dur = (float)A->n / A->fps;
    if (g_a2_clock >= dur) {
        if (A->loop) {
            g_a2_clock = fmodf(g_a2_clock, dur);
        } else {
            const char *nx = A->next[0] ? A->next : "idle";
            a2_start(nx, true);
            return;
        }
    }

    if (strcmp(g_a2_cur, "idle") == 0) {
        g_a2_blink -= dt;
        g_a2_idle -= dt;
        if (g_a2_blink <= 0) {
            g_a2_blink = 3.0f + (float)(rand() % 500) / 100.0f;
            if (a2_has("blink")) a2_start("blink", true);
        } else if (g_a2_idle <= 0) {
            g_a2_idle = 26.0f + (float)(rand() % 2500) / 100.0f;
            const char *pick = NULL;
            if (a2_has("meditate_in") && a2_has("sword_in"))
                pick = (rand() % 2) ? "meditate_in" : "sword_in";
            else if (a2_has("meditate_in")) pick = "meditate_in";
            else if (a2_has("sword_in")) pick = "sword_in";
            if (pick) {
                a2_start(pick, true);
                g_a2_hold = 16.0f + (float)(rand() % 1800) / 100.0f;
            }
        }
    } else if (strcmp(g_a2_cur, "meditate") == 0 || strcmp(g_a2_cur, "sword") == 0) {
        g_a2_hold -= dt;
        if (g_a2_hold <= 0) {
            a2_start(strcmp(g_a2_cur, "meditate") == 0 ? "meditate_out" : "sword_out", true);
            g_a2_idle = 18.0f + (float)(rand() % 2000) / 100.0f;
        }
    }
}

static void draw_image_alpha(Graphics &g, Image *img, REAL alpha) {
    if (!img || alpha <= 0.01f) return;
    RectF dst(45.0f, 48.0f, 210.0f, 279.0f);
    if (alpha > 0.99f) {
        g.DrawImage(img, dst, 0.0f, 0.0f, (REAL)img->GetWidth(), (REAL)img->GetHeight(), UnitPixel);
        return;
    }
    ColorMatrix cm = { {
        1, 0, 0, 0, 0,
        0, 1, 0, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 0, alpha, 0,
        0, 0, 0, 0, 1
    } };
    ImageAttributes attrs;
    attrs.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
    g.DrawImage(img, dst, 0.0f, 0.0f, (REAL)img->GetWidth(), (REAL)img->GetHeight(),
                UnitPixel, &attrs);
}

static void a2_draw_idx(Graphics &g, A2Action *A, int idx, REAL alpha) {
    if (!A || A->n == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= A->n) idx = A->n - 1;
    draw_image_alpha(g, A->frames[idx], alpha);
}

static void a2_draw(Graphics &g, PetAnim *a) {
    float t = a->t;
    float extra = 0;
    if (strcmp(g_a2_cur, "sword") == 0 || strcmp(g_a2_cur, "sword_in") == 0 ||
        strcmp(g_a2_cur, "sword_out") == 0)
        extra = sinf(t * 1.1f) * 4.0f;
    else if (strcmp(g_a2_cur, "meditate") == 0)
        extra = sinf(t * 1.2f) * 2.5f;
    if (extra != 0) g.TranslateTransform(0.0f, extra, MatrixOrderAppend);

    A2Action *A = a2_find(g_a2_cur);
    int idx = A ? (int)(g_a2_clock * A->fps) : 0;
    if (g_a2_fade > 0 && g_a2_prev[0]) {
        float k = g_a2_fade / 0.28f;
        A2Action *P = a2_find(g_a2_prev);
        if (P) a2_draw_idx(g, P, P->n - 1, k);
        a2_draw_idx(g, A, idx, 1.0f - k);
    } else {
        a2_draw_idx(g, A, idx, 1.0f);
    }
}

extern "C" void anim_next_action(void) {
    if (!g_anim2) return;
    if (strcmp(g_a2_cur, "idle") == 0) a2_start("meditate_in", true);
    else if (strcmp(g_a2_cur, "meditate_in") == 0 || strcmp(g_a2_cur, "meditate") == 0)
        a2_start("sword_in", true);
    else if (strcmp(g_a2_cur, "sword_in") == 0 || strcmp(g_a2_cur, "sword") == 0)
        a2_start("breakthrough", true);
    else a2_start("idle", true);
    g_a2_hold = 12.0f;
}

static void draw_sprite_pose(Graphics &g, PetAnim *a) {
    Image *img = g_sprites[0];
    if (a->breakthrough > 0.35f && g_sprites[4]) img = g_sprites[4];
    else if (a->pose == POSE_MEDITATE && g_sprites[2]) img = g_sprites[2];
    else if (a->pose == POSE_SWORD && g_sprites[3]) img = g_sprites[3];
    else if (a->blink > 0.5f && g_sprites[1]) img = g_sprites[1];
    if (!img) return;
    RectF dst(45.0f, 48.0f, 210.0f, 279.0f);
    g.DrawImage(img, dst, 0.0f, 0.0f, (REAL)img->GetWidth(), (REAL)img->GetHeight(), UnitPixel);
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static ULONG_PTR g_gdi_token = 0;
static bool g_gdi_on = false;

extern "C" void anim_gfx_startup(void) {
    if (g_gdi_on) return;
    GdiplusStartupInput in;
    if (GdiplusStartup(&g_gdi_token, &in, NULL) == Ok) g_gdi_on = true;
}

extern "C" void anim_gfx_shutdown(void) {
    if (!g_gdi_on) return;
    GdiplusShutdown(g_gdi_token);
    g_gdi_on = false;
}

/* ---------- helpers ---------- */

static void draw_sleeve(Graphics &g, float x, float y, float w, float h, float rot,
                        SolidBrush &robe, Pen &trim) {
    GraphicsState st = g.Save();
    g.TranslateTransform(x, y, MatrixOrderAppend);
    g.RotateTransform(rot, MatrixOrderAppend);
    GraphicsPath p;
    p.StartFigure();
    p.AddBezier(-w / 2.0f, 0.0f, -w / 2.0f - 6.0f, h * 0.5f, -w / 2.0f - 4.0f, h * 0.8f, -w / 2.0f + 2.0f, h);
    p.AddLine(-w / 2.0f + 2.0f, h, w / 2.0f - 2.0f, h);
    p.AddBezier(w / 2.0f - 2.0f, h, w / 2.0f + 4.0f, h * 0.8f, w / 2.0f + 6.0f, h * 0.5f, w / 2.0f, 0.0f);
    p.AddLine(w / 2.0f, 0.0f, -w / 2.0f, 0.0f);
    p.CloseFigure();
    g.FillPath(&robe, &p);
    g.DrawPath(&trim, &p);
    g.Restore(st);
}

static void draw_sword(Graphics &g, float cx, float cy, float angle_deg, float len) {
    GraphicsState st = g.Save();
    g.TranslateTransform(cx, cy, MatrixOrderAppend);
    g.RotateTransform(angle_deg, MatrixOrderAppend);
    Pen blade(Color(235, 205, 215, 228), 5.0f);
    blade.SetLineJoin(LineJoinRound);
    blade.SetStartCap(LineCapRound);
    blade.SetEndCap(LineCapRound);
    Pen edge(Color(160, 255, 255, 255), 1.5f);
    SolidBrush hilt(Color(255, 205, 165, 80));
    g.DrawLine(&blade, 0.0f, 0.0f, 0.0f, -len);
    g.DrawLine(&edge, -1.5f, -4.0f, -1.5f, -len + 4.0f);
    g.FillEllipse(&hilt, -7.0f, 2.0f, 14.0f, 14.0f);
    g.DrawLine(&blade, -12.0f, 0.0f, 12.0f, 0.0f);
    g.Restore(st);
}

static void draw_head(Graphics &g, float cx, float cy, float r, float blink, float face_dx) {
    SolidBrush skin(Color(255, 255, 226, 192));
    SolidBrush hair(Color(255, 46, 50, 62));
    SolidBrush bun(Color(255, 56, 60, 74));
    SolidBrush dark(Color(255, 34, 36, 44));
    SolidBrush white(Color(255, 255, 255, 255));
    SolidBrush blush(Color(95, 250, 150, 150));
    SolidBrush red(Color(255, 200, 70, 70));
    Pen face_pen(Color(255, 60, 62, 72), 2.4f);
    face_pen.SetStartCap(LineCapRound);
    face_pen.SetEndCap(LineCapRound);

    /* ears */
    g.FillEllipse(&skin, cx - r - 4.0f, cy + 2.0f, 10.0f, 14.0f);
    g.FillEllipse(&skin, cx + r - 6.0f, cy + 2.0f, 10.0f, 14.0f);
    /* head */
    g.FillEllipse(&skin, cx - r, cy - r, r * 2.0f, r * 2.0f);
    /* back hair */
    GraphicsPath bh;
    bh.AddArc(cx - r - 3.0f, cy - r - 4.0f, (r + 3.0f) * 2.0f, (r + 5.0f) * 2.0f, 150.0f, 240.0f);
    bh.AddBezier(cx + r + 3.0f, cy + r - 30.0f, cx + r - 6.0f, cy + r + 6.0f, cx - r + 6.0f, cy + r + 6.0f, cx - r - 3.0f, cy + r - 30.0f);
    bh.CloseFigure();
    g.FillPath(&hair, &bh);
    /* fringe */
    GraphicsPath fh;
    fh.StartFigure();
    fh.AddBezier(cx - r - 2.0f, cy - r + 16.0f, cx - r * 0.7f, cy - r - 14.0f, cx + r * 0.7f, cy - r - 14.0f, cx + r + 2.0f, cy - r + 16.0f);
    fh.AddBezier(cx + r + 2.0f, cy - r + 16.0f, cx + r * 0.35f, cy - r + 24.0f, cx + r * 0.1f, cy - r + 8.0f, cx - r * 0.12f, cy - r + 18.0f);
    fh.AddBezier(cx - r * 0.12f, cy - r + 18.0f, cx - r * 0.32f, cy - r + 26.0f, cx - r * 0.6f, cy - r + 10.0f, cx - r - 2.0f, cy - r + 16.0f);
    fh.CloseFigure();
    g.FillPath(&hair, &fh);
    /* topknot */
    g.FillEllipse(&bun, cx - 15.0f, cy - r - 26.0f, 30.0f, 26.0f);
    g.FillEllipse(&red, cx - 16.0f, cy - r - 8.0f, 32.0f, 7.0f);
    Pen pin(Color(255, 220, 200, 120), 3.0f);
    g.DrawLine(&pin, cx - 20.0f, cy - r - 14.0f, cx + 20.0f, cy - r - 22.0f);

    /* eyes */
    float eye_y = cy + 2;
    float eye_h = 13.0f * (1.0f - 0.92f * blink);
    if (eye_h < 1.2f) eye_h = 1.2f;
    float ex1 = cx - 15 + face_dx, ex2 = cx + 15 + face_dx;
    g.FillEllipse(&dark, ex1 - 5.0f, eye_y - eye_h / 2.0f, 10.0f, eye_h);
    g.FillEllipse(&dark, ex2 - 5.0f, eye_y - eye_h / 2.0f, 10.0f, eye_h);
    if (blink < 0.6f) {
        g.FillEllipse(&white, ex1 - 1.5f, eye_y - eye_h / 2.0f + 2.0f, 3.4f, 3.4f);
        g.FillEllipse(&white, ex2 - 1.5f, eye_y - eye_h / 2.0f + 2.0f, 3.4f, 3.4f);
    }
    /* eyebrows */
    g.DrawLine(&face_pen, ex1 - 6.0f, eye_y - 13.0f, ex1 + 6.0f, eye_y - 15.0f);
    g.DrawLine(&face_pen, ex2 - 6.0f, eye_y - 15.0f, ex2 + 6.0f, eye_y - 13.0f);
    /* mouth */
    GraphicsPath mp;
    mp.AddArc(cx - 5.0f + face_dx, cy + 18.0f, 10.0f, 6.0f, 20.0f, 140.0f);
    g.DrawPath(&face_pen, &mp);
    /* blush + nose */
    g.FillEllipse(&blush, cx - 30.0f, cy + 10.0f, 14.0f, 8.0f);
    g.FillEllipse(&blush, cx + 16.0f, cy + 10.0f, 14.0f, 8.0f);
    g.FillEllipse(&dark, cx - 1.2f + face_dx, cy + 11.0f, 2.4f, 2.4f);
}

static void draw_aura(Graphics &g, float cx, float cy, float radius, BYTE alpha, BYTE r, BYTE gg, BYTE b) {
    for (int i = 6; i >= 1; i--) {
        float rr = radius * i / 6.0f;
        BYTE a = (BYTE)(alpha * (1.0f - (i - 1) / 6.0f) * 0.35f);
        SolidBrush br(Color(a, r, gg, b));
        g.FillEllipse(&br, cx - rr, cy - rr, rr * 2.0f, rr * 2.0f);
    }
}

static GraphicsPath *build_robe(float hem_dy, float squash) {
    GraphicsPath *p = new GraphicsPath();
    float y0 = 186, y1 = 252, y2 = 312 + hem_dy;
    float sh_w = 44, wa_w = 32, hem_w = 54;
    float cx = 150;
    p->StartFigure();
    p->AddBezier(cx - sh_w, y0, cx - sh_w - 4, y0 + 24, cx - wa_w - 4 * squash, y1 - 10, cx - wa_w, y1);
    p->AddBezier(cx - wa_w, y1, cx - wa_w - 12, y1 + 30, cx - hem_w, y2 - 18, cx - hem_w, y2);
    p->AddLine(cx - hem_w, y2, cx + hem_w, y2);
    p->AddBezier(cx + hem_w, y2, cx + hem_w, y2 - 18, cx + wa_w + 12, y1 + 30, cx + wa_w, y1);
    p->AddBezier(cx + wa_w, y1, cx + wa_w + 4 * squash, y1 - 10, cx + sh_w + 4, y0 + 24, cx + sh_w, y0);
    p->AddBezier(cx + sh_w, y0, cx + 20, y0 - 8, cx - 20, y0 - 8, cx - sh_w, y0);
    p->CloseFigure();
    return p;
}

static void draw_popups(Graphics &g, PetAnim *a) {
    if (a->popup_count == 0) return;
    static FontFamily *fam = NULL;
    static Font *font = NULL;
    static StringFormat *fmt = NULL;
    if (!font) {
        fam = new FontFamily(L"Microsoft YaHei");
        if (!fam->IsAvailable()) {
            delete fam;
            fam = new FontFamily(L"Segoe UI");
        }
        font = new Font(fam, 22.0f, FontStyleBold, UnitPixel);
        fmt = new StringFormat();
        fmt->SetAlignment(StringAlignmentCenter);
        fmt->SetLineAlignment(StringAlignmentCenter);
    }
    for (int i = 0; i < a->popup_count; i++) {
        PetPopup *p = &a->popups[i];
        wchar_t *w = tp_utf8_to_wide(p->text);
        SolidBrush outline(Color((BYTE)(p->alpha * 200), 20, 40, 70));
        SolidBrush fill(Color((BYTE)(p->alpha * 255), 130, 230, 255));
        RectF r(p->x - 70, p->y - 16, 140, 32);
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++) {
                if (!dx && !dy) continue;
                RectF rr(r.X + dx * 2.0f, r.Y + dy * 2.0f, r.Width, r.Height);
                g.DrawString(w, -1, font, rr, fmt, &outline);
            }
        g.DrawString(w, -1, font, r, fmt, &fill);
        free(w);
    }
}

static void draw_plates(Graphics &g, PetAnim *a) {
    static FontFamily *fam = NULL;
    static Font *f_big = NULL, *f_small = NULL;
    static StringFormat *fmt = NULL;
    if (!fam) {
        fam = new FontFamily(L"Microsoft YaHei");
        if (!fam->IsAvailable()) {
            delete fam;
            fam = new FontFamily(L"Segoe UI");
        }
        f_big = new Font(fam, 19.0f, FontStyleBold, UnitPixel);
        f_small = new Font(fam, 12.5f, FontStyleRegular, UnitPixel);
        fmt = new StringFormat();
        fmt->SetAlignment(StringAlignmentCenter);
        fmt->SetLineAlignment(StringAlignmentCenter);
    }
    wchar_t line2[160];
    char big[64];
    anim_format_big(big, sizeof(big), a->total_xp);
    wchar_t *bigW = tp_utf8_to_wide(big);
    _snwprintf(line2, 160, L"总修为 %s", bigW ? bigW : L"");
    line2[159] = 0;
    free(bigW);

    SolidBrush outline(Color(185, 12, 22, 46));
    SolidBrush fill1(Color(255, 252, 253, 255));
    SolidBrush fill2(Color(240, 205, 230, 255));

    RectF r1(20, 12, 260, 28);
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            if (!dx && !dy) continue;
            RectF rr(r1.X + dx * 1.6f, r1.Y + dy * 1.6f, r1.Width, r1.Height);
            g.DrawString(a->realm, -1, f_big, rr, fmt, &outline);
        }
    g.DrawString(a->realm, -1, f_big, r1, fmt, &fill1);

    RectF r2(20, 42, 260, 20);
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            if (!dx && !dy) continue;
            RectF rr(r2.X + dx * 1.4f, r2.Y + dy * 1.4f, r2.Width, r2.Height);
            g.DrawString(line2, -1, f_small, rr, fmt, &outline);
        }
    g.DrawString(line2, -1, f_small, r2, fmt, &fill2);
}

extern "C" void anim_draw_surface(void *bits, int w, int h, float page_scale, PetAnim *a) {
    Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *)bits);
    Graphics g(&bmp);
    g.SetPageUnit(UnitPixel);
    g.SetPageScale(page_scale);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    float t = a->t;
    float sway = sinf(t * 0.7f) * 1.6f;
    float breath = sinf(t * 1.6f);
    float bob = breath * 1.8f;
    bool meditating = (a->pose == POSE_MEDITATE);

    GraphicsState st = g.Save();
    g.TranslateTransform(150.0f, 170.0f + bob, MatrixOrderAppend);
    g.RotateTransform(sway * 0.35f, MatrixOrderAppend);
    g.TranslateTransform(-150.0f, -170.0f, MatrixOrderAppend);

    /* ground shadow */
    {
        SolidBrush sh(Color(55, 10, 15, 30));
        float sw = meditating ? 120.0f : 108.0f;
        g.FillEllipse(&sh, 150.0f - sw / 2.0f, 306.0f, sw, 20.0f);
    }

    /* auras */
    if (a->breakthrough > 0) {
        float k = 1.0f - a->breakthrough;
        draw_aura(g, 150, 200, 110 + 90 * k, (BYTE)(220 * a->breakthrough), 255, 210, 120);
    }
    if (meditating || a->working > 0.05f) draw_aura(g, 150, 210, 95, 70, 130, 200, 255);
    if (a->gain_pulse > 0) {
        float k = a->gain_pulse;
        draw_aura(g, 150, 200, 70 + 60 * (1.0f - k), (BYTE)(150 * k), 140, 240, 200);
    }

    if (meditating) g.TranslateTransform(0.0f, 16.0f + sinf(t * 1.2f) * 3.0f, MatrixOrderAppend);

    float body_squash = meditating ? 0.82f : 1.0f;
    float head_dy = meditating ? 14.0f : 0;

    if (g_anim2) {
        a2_draw(g, a);
    } else if (g_sprite_mode) {
        draw_sprite_pose(g, a);
    } else {
    /* sword */
    if (a->pose == POSE_SWORD) {
        float ang = t * 70.0f;
        float rr = 118.0f;
        float sx = 150 + cosf(ang * (float)M_PI / 180.0f) * rr;
        float sy = 215 + sinf(ang * (float)M_PI / 180.0f) * rr * 0.42f;
        draw_sword(g, sx, sy, ang + 90.0f, 66.0f);
    } else if (!meditating) {
        GraphicsState s2 = g.Save();
        g.TranslateTransform(196.0f, 170.0f, MatrixOrderAppend);
        g.RotateTransform(24.0f, MatrixOrderAppend);
        draw_sword(g, 0, 0, 0, 150.0f);
        g.Restore(s2);
    }

    /* legs */
    if (!meditating) {
        SolidBrush boots(Color(255, 52, 58, 74));
        g.FillEllipse(&boots, 126.0f, 296.0f, 24.0f, 18.0f);
        g.FillEllipse(&boots, 150.0f, 296.0f, 24.0f, 18.0f);
    }

    /* robe */
    SolidBrush robe(Color(255, 245, 247, 252));
    SolidBrush robe_shadow(Color(70, 150, 170, 215));
    Pen trim(Color(255, 96, 126, 196), 2.6f);
    GraphicsPath *body = build_robe(meditating ? -6.0f : 0.0f, body_squash);
    g.FillPath(&robe, body);
    g.FillPath(&robe_shadow, body);
    g.DrawPath(&trim, body);
    delete body;

    float arm_sw = meditating ? 10.0f : 6.0f;
    draw_sleeve(g, 106, 194, 26, 52 + arm_sw, meditating ? 30.0f : 8.0f, robe, trim);
    draw_sleeve(g, 194, 194, 26, 52 + arm_sw, meditating ? -30.0f : -8.0f, robe, trim);

    /* belt */
    {
        SolidBrush belt(Color(255, 200, 82, 72));
        g.FillRectangle(&belt, 116.0f, 244.0f + (meditating ? 8.0f : 0.0f), 68.0f, 11.0f);
        SolidBrush knot(Color(255, 224, 120, 96));
        g.FillEllipse(&knot, 144.0f, 242.0f + (meditating ? 8.0f : 0.0f), 12.0f, 12.0f);
    }

    /* neck */
    {
        SolidBrush skin(Color(255, 240, 205, 172));
        g.FillRectangle(&skin, 141.0f, 172.0f + head_dy, 18.0f, 16.0f);
    }

    draw_head(g, 150, 140 + head_dy, 38, a->blink, meditating ? 0.0f : sway * 0.12f);

    if (meditating) {
        Pen ring(Color(120, 140, 220, 255), 3.0f);
        float rr = 96 + sinf(t * 1.8f) * 6.0f;
        g.DrawEllipse(&ring, 150.0f - rr, 236.0f - rr * 0.34f, rr * 2.0f, rr * 0.68f);
    }
    } /* end procedural body */

    g.Restore(st);

    /* breakthrough rings */
    if (a->breakthrough > 0) {
        float k = 1.0f - a->breakthrough;
        for (int i = 0; i < 3; i++) {
            float kk = k - i * 0.12f;
            if (kk <= 0) continue;
            float rr = 60 + 240 * kk;
            BYTE alpha = (BYTE)(180 * a->breakthrough * (1.0f - kk));
            Pen ring(Color(alpha, 255, 225, 150), 4.0f);
            g.DrawEllipse(&ring, 150.0f - rr, 210.0f - rr * 0.5f, rr * 2.0f, rr);
        }
    }

    /* gain sparks */
    if (a->gain_pulse > 0.05f) {
        int n = 10;
        for (int i = 0; i < n; i++) {
            float ph = (float)i / n * 6.28318f + t * 2.0f;
            float rr = 60 + sinf(t * 3.0f + i) * 14;
            float x = 150 + cosf(ph) * rr;
            float y = 210 + sinf(ph) * rr * 0.45f - a->gain_pulse * 30.0f;
            BYTE al = (BYTE)(200 * a->gain_pulse);
            SolidBrush sp(Color(al, 170, 255, 220));
            g.FillEllipse(&sp, x - 3.0f, y - 3.0f, 6.0f, 6.0f);
        }
    }

    draw_popups(g, a);
    draw_plates(g, a);
}

extern "C" HICON anim_make_icon(int size) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    float s = (float)size / 32.0f;
    g.ScaleTransform(s, s);
    SolidBrush skin(Color(255, 255, 226, 192));
    SolidBrush hair(Color(255, 46, 50, 62));
    SolidBrush dark(Color(255, 34, 36, 44));
    g.FillEllipse(&skin, 4.0f, 5.0f, 24.0f, 24.0f);
    g.FillPie(&hair, 2.0f, 1.0f, 28.0f, 26.0f, 180.0f, 180.0f);
    g.FillEllipse(&hair, 13.0f, 0.0f, 7.0f, 7.0f);
    g.FillEllipse(&dark, 10.0f, 13.0f, 4.0f, 5.0f);
    g.FillEllipse(&dark, 18.0f, 13.0f, 4.0f, 5.0f);
    HICON ic = NULL;
    bmp.GetHICON(&ic);
    return ic;
}

/* ---------- animation logic (pure C++ but C-linkage API) ---------- */

extern "C" void anim_init(PetAnim *a) {
    memset(a, 0, sizeof(*a));
    a->pose = POSE_STAND;
    a->pose_timer = 18.0f;
    a->blink_timer = 3.0f;
    wcsncpy(a->realm, L"凡人", 63);
    wcsncpy(a->faction, L"—", 63);
}

extern "C" void anim_pulse_gain(PetAnim *a, float strength) {
    if (strength > a->gain_pulse) a->gain_pulse = strength;
    if (a->gain_pulse > 1.5f) a->gain_pulse = 1.5f;
}

extern "C" void anim_trigger_breakthrough(PetAnim *a) {
    a->breakthrough = 1.0f;
    if (g_anim2) a2_start("breakthrough", true);
}

extern "C" void anim_add_popup(PetAnim *a, int64_t delta) {
    if (a->popup_count >= PET_MAX_POPUPS) {
        memmove(&a->popups[0], &a->popups[1], sizeof(PetPopup) * (PET_MAX_POPUPS - 1));
        a->popup_count = PET_MAX_POPUPS - 1;
    }
    PetPopup *p = &a->popups[a->popup_count++];
    memset(p, 0, sizeof(*p));
    p->x = 150.0f + (float)((rand() % 40) - 20);
    p->y = 150.0f + (float)(rand() % 30);
    p->vy = -34.0f;
    p->alpha = 1.0f;
    p->life = 1.8f;
    char buf[48];
    anim_format_big(buf, sizeof(buf), delta);
    tp_snprintf(p->text, sizeof(p->text), "+%s", buf);
}

extern "C" void anim_format_big(char *out, size_t cap, int64_t v) {
    if (v < 10000) {
        tp_snprintf(out, cap, "%lld", (long long)v);
    } else if (v < 100000000LL) {
        tp_snprintf(out, cap, "%.1f万", (double)v / 10000.0);
    } else if (v < 1000000000000LL) {
        tp_snprintf(out, cap, "%.2f亿", (double)v / 100000000.0);
    } else {
        tp_snprintf(out, cap, "%.2f万亿", (double)v / 1000000000000.0);
    }
}

extern "C" void anim_update(PetAnim *a, float dt) {
    a->t += dt;
    if (g_anim2) a2_update(dt);

    a->blink_timer -= dt;
    if (a->blink_timer <= 0) {
        a->blink = 1.0f;
        if (a->blink_timer < -0.14f) {
            a->blink = 0;
            a->blink_timer = 2.0f + (float)(rand() % 400) / 100.0f;
        }
    } else if (a->blink > 0) {
        a->blink -= dt * 6.0f;
        if (a->blink < 0) a->blink = 0;
    }

    a->pose_timer -= dt;
    if (a->pose_timer <= 0) {
        int r = rand() % 100;
        if (a->pose == POSE_STAND) {
            if (r < 45) { a->pose = POSE_MEDITATE; a->pose_timer = 16.0f + (float)(rand() % 20); }
            else if (r < 75) { a->pose = POSE_SWORD; a->pose_timer = 10.0f + (float)(rand() % 10); }
            else { a->pose_timer = 8.0f + (float)(rand() % 12); }
        } else {
            a->pose = POSE_STAND;
            a->pose_timer = 20.0f + (float)(rand() % 25);
        }
    }

    if (a->gain_pulse > 0) a->gain_pulse -= dt * 0.8f;
    if (a->gain_pulse < 0) a->gain_pulse = 0;
    if (a->breakthrough > 0) a->breakthrough -= dt * 0.45f;
    if (a->breakthrough < 0) a->breakthrough = 0;

    for (int i = 0; i < a->popup_count; i++) {
        PetPopup *p = &a->popups[i];
        p->life -= dt;
        p->y += p->vy * dt;
        p->vy *= (1.0f - dt * 0.6f);
        p->alpha = p->life > 0.5f ? 1.0f : (p->life / 0.5f);
        if (p->life <= 0) {
            memmove(&a->popups[i], &a->popups[i + 1], sizeof(PetPopup) * (a->popup_count - i - 1));
            a->popup_count--;
            i--;
        }
    }
}
