#ifndef TOKENPET_ANIM_H
#define TOKENPET_ANIM_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { POSE_STAND = 0, POSE_MEDITATE, POSE_SWORD, POSE_COUNT } PetPose;

typedef struct {
    float x, y;
    float vy;
    float alpha;
    float life;
    char text[48];
} PetPopup;

#define PET_MAX_POPUPS 24

typedef struct {
    float    t;              /* seconds since start */
    float    blink;          /* 0 open .. 1 closed */
    float    blink_timer;    /* next blink countdown */
    float    pose_timer;     /* time until next idle pose switch */
    PetPose  pose;
    float    gain_pulse;     /* 0..1 decaying glow on token gain */
    float    breakthrough;   /* 0..1 decaying burst */
    float    working;        /* 0..1 aura when a session is active */
    PetPopup popups[PET_MAX_POPUPS];
    int      popup_count;
    /* stats shown on bubble (P2/P3) */
    wchar_t  realm[64];
    int64_t  total_xp;
    int64_t  today_xp;
    wchar_t  faction[64];
} PetAnim;

/* GDI+ lifecycle (implemented in anim.cpp) */
void anim_gfx_startup(void);
void anim_gfx_shutdown(void);

/* optional sprite pack: dir containing manifest.json + frames/ */
void anim_load_sprites(const wchar_t *character_dir);

void anim_init(PetAnim *a);
void anim_update(PetAnim *a, float dt);
void anim_add_popup(PetAnim *a, int64_t delta_tokens);
void anim_trigger_breakthrough(PetAnim *a);
void anim_pulse_gain(PetAnim *a, float strength);

/* Renders the character directly into a top-down 32bpp premultiplied ARGB buffer. */
void anim_draw_surface(void *bits, int w, int h, float page_scale, PetAnim *a);

/* Builds a tray/window icon procedurally. Caller owns the HICON. */
HICON anim_make_icon(int size);

void anim_format_big(char *out, size_t cap, int64_t v);

/* debug/preview: jump to the next action (anim2 mode) */
void anim_next_action(void);

#ifdef __cplusplus
}
#endif

#endif
