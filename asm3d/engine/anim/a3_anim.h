/*
 * ASM3D - a3_anim.h
 * Animation:
 *   - Keyframe clips (.a3anim, JSON). A track animates one reflected field of
 *     one component ("Transform.position", "Light.intensity", any custom
 *     component field) on the animated entity or on a named child. Keys use
 *     step, linear or smooth (Catmull-Rom) interpolation; rotations use
 *     normalized quaternion blending.
 *   - Animator component: plays a clip (once, loop, ping-pong) at a speed.
 *   - Motion component: no-keyframe procedural movement (spin, bob, pulse,
 *     back-and-forth) for coins, platforms, pickups and props.
 * Skeletal (bone) animation arrives with glTF import (see docs/STATUS.md).
 */
#ifndef A3_ANIM_H
#define A3_ANIM_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"
#include "../core/a3_strbuf.h"
#include "../core/a3_memory.h"
#include "../ecs/a3_ecs.h"

A3_EXTERN_C_BEGIN

#define A3_ANIM_MAX_TRACKS 32

typedef enum A3AnimInterp { A3_INTERP_SMOOTH = 0, A3_INTERP_LINEAR, A3_INTERP_STEP, A3_INTERP_COUNT } A3AnimInterp;
typedef enum A3AnimLoop { A3_ANIM_ONCE = 0, A3_ANIM_LOOP, A3_ANIM_PINGPONG } A3AnimLoop;

typedef struct A3AnimKey {
    f32 time;
    f32 value[4];
    i32 interp;               /* how to reach the NEXT key */
} A3AnimKey;

typedef struct A3AnimTrack {
    char target[A3_NAME_MAX];     /* child entity name, "" = the animated entity */
    char component[A3_NAME_MAX];
    char field[A3_NAME_MAX];
    A3_ARRAY_TYPE(A3AnimKey) keys; /* sorted by time */
} A3AnimTrack;

typedef struct A3AnimClip {
    char name[A3_NAME_MAX];
    f32 duration;
    A3AnimTrack tracks[A3_ANIM_MAX_TRACKS];
    u32 track_count;
} A3AnimClip;

/* ---- clips ---- */
void a3_anim_clip_init(A3AnimClip *c, const char *name);
void a3_anim_clip_free(A3AnimClip *c);
void a3_anim_clip_copy(A3AnimClip *dst, const A3AnimClip *src);
i32  a3_anim_track_find(const A3AnimClip *c, const char *target, const char *component, const char *field);
i32  a3_anim_track_add(A3AnimClip *c, const char *target, const char *component, const char *field);
void a3_anim_track_remove(A3AnimClip *c, u32 track);
/* Inserts (or replaces a key at the same time +-1 ms); keeps keys sorted. Returns key index. */
i32  a3_anim_key_set(A3AnimTrack *t, f32 time, const f32 *value, i32 interp);
void a3_anim_key_remove(A3AnimTrack *t, u32 key);
/* Value of a track at time t (clamped to the first/last key). `quat` blends as a rotation. */
void a3_anim_track_sample(const A3AnimTrack *t, f32 time, b32 quat, f32 out[4]);
/* Largest key time (use as duration when none is set). */
f32  a3_anim_clip_length(const A3AnimClip *c);

/* Applies every track at `time` to entity e (and its named children). */
void a3_anim_apply(const A3AnimClip *c, A3World *w, A3Entity e, f32 time);
/* Reads the current value of a component field into out[4]; false if not animatable. */
b32  a3_anim_read_field(A3World *w, A3Entity e, const char *component, const char *field, f32 out[4]);
b32  a3_anim_field_animatable(u32 field_type);

void a3_anim_save_json(const A3AnimClip *c, A3StrBuf *out);
b32  a3_anim_load_json(A3AnimClip *c, const char *text, usize len, char *error, usize error_cap);

/* Asset cache (project-relative path); 0 when the file is missing/broken (logged once). */
const A3AnimClip *a3_anim_clip_get(const char *path);
void a3_anim_clip_invalidate(const char *path);   /* after the editor saves a clip */
void a3_anim_cache_clear(void);

/* ---- components ---- */
extern u32 A3_T_ANIMATOR;
extern u32 A3_T_MOTION;

typedef struct A3CAnimator {
    A3AssetRef clip;
    b32 play_on_start;
    i32 loop;                 /* A3AnimLoop */
    f32 speed;
    b32 playing;              /* runtime (set to true to play from code) */
    f32 time;                 /* runtime */
    f32 direction;            /* runtime, ping-pong */
    b32 started;              /* runtime */
} A3CAnimator;

typedef struct A3CMotion {
    A3Vec3 spin;              /* degrees per second around X, Y, Z */
    f32 bob_height;           /* meters up and down */
    f32 bob_speed;            /* cycles per second */
    f32 pulse_amount;         /* 0.2 = grows and shrinks by 20% */
    f32 pulse_speed;          /* cycles per second */
    A3Vec3 move_offset;       /* back and forth between start and start + offset */
    f32 move_speed;           /* round trips per second */
    f32 phase;                /* 0..1, desynchronizes copies */
    b32 active;
    /* runtime */
    b32 started;
    f32 time;
    A3Vec3 base_pos; f32 _p0;
    A3Vec3 base_scale; f32 _p1;
    A3Quat base_rot;
} A3CMotion;

void a3_anim_register(void);                   /* components (called by a3_modules) */
void a3_anim_update(A3World *w, f32 dt);       /* Animator + Motion (play mode only) */

A3_EXTERN_C_END

#endif
