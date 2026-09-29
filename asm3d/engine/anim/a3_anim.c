/*
 * ASM3D - a3_anim.c
 */
#include "a3_anim.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_json.h"
#include "../core/a3_hash.h"
#include "../platform/a3_platform.h"
#include "../scene/a3_components.h"
#include "../resource/a3_assets.h"

u32 A3_T_ANIMATOR = 0xFFFFFFFFu;
u32 A3_T_MOTION = 0xFFFFFFFFu;

/* ======================================================================== */
/* Clips                                                                    */
/* ======================================================================== */

void a3_anim_clip_init(A3AnimClip *c, const char *name) {
    a3_zero_struct(c);
    a3_strcpy(c->name, sizeof(c->name), name ? name : "Animation");
    c->duration = 2.0f;
}

void a3_anim_clip_free(A3AnimClip *c) {
    for (u32 i = 0; i < c->track_count; ++i) a3_array_free(c->tracks[i].keys);
    c->track_count = 0;
}

void a3_anim_clip_copy(A3AnimClip *dst, const A3AnimClip *src) {
    a3_anim_clip_free(dst);
    *dst = *src;
    for (u32 i = 0; i < src->track_count; ++i) {
        A3AnimTrack *t = &dst->tracks[i];
        t->keys.data = 0;
        t->keys.count = t->keys.cap = 0;
        if (src->tracks[i].keys.count && a3_array_reserve(t->keys, src->tracks[i].keys.count, A3_MEM_ANIM)) {
            a3_memcpy(t->keys.data, src->tracks[i].keys.data, sizeof(A3AnimKey) * src->tracks[i].keys.count);
            t->keys.count = src->tracks[i].keys.count;
        }
    }
}

i32 a3_anim_track_find(const A3AnimClip *c, const char *target, const char *component, const char *field) {
    for (u32 i = 0; i < c->track_count; ++i) {
        const A3AnimTrack *t = &c->tracks[i];
        if (a3_streq(t->target, target ? target : "") && a3_streq(t->component, component) && a3_streq(t->field, field)) return (i32)i;
    }
    return -1;
}

i32 a3_anim_track_add(A3AnimClip *c, const char *target, const char *component, const char *field) {
    i32 found = a3_anim_track_find(c, target, component, field);
    if (found >= 0) return found;
    if (c->track_count >= A3_ANIM_MAX_TRACKS) return -1;
    A3AnimTrack *t = &c->tracks[c->track_count];
    a3_zero_struct(t);
    a3_strcpy(t->target, sizeof(t->target), target ? target : "");
    a3_strcpy(t->component, sizeof(t->component), component);
    a3_strcpy(t->field, sizeof(t->field), field);
    return (i32)c->track_count++;
}

void a3_anim_track_remove(A3AnimClip *c, u32 track) {
    if (track >= c->track_count) return;
    a3_array_free(c->tracks[track].keys);
    a3_memmove(&c->tracks[track], &c->tracks[track + 1], sizeof(A3AnimTrack) * (c->track_count - track - 1));
    c->track_count--;
}

i32 a3_anim_key_set(A3AnimTrack *t, f32 time, const f32 *value, i32 interp) {
    time = a3_maxf(time, 0);
    for (u32 i = 0; i < t->keys.count; ++i) {
        if (a3_absf(t->keys.data[i].time - time) < 0.001f) {
            for (int k = 0; k < 4; ++k) t->keys.data[i].value[k] = value[k];
            t->keys.data[i].interp = interp;
            return (i32)i;
        }
    }
    A3AnimKey key;
    key.time = time;
    for (int k = 0; k < 4; ++k) key.value[k] = value[k];
    key.interp = interp;
    if (!a3_array_push(t->keys, key, A3_MEM_ANIM)) return -1;
    /* insertion sort step */
    i32 i = (i32)t->keys.count - 1;
    while (i > 0 && t->keys.data[i - 1].time > key.time) { t->keys.data[i] = t->keys.data[i - 1]; --i; }
    t->keys.data[i] = key;
    return i;
}

void a3_anim_key_remove(A3AnimTrack *t, u32 key) {
    if (key >= t->keys.count) return;
    a3_memmove(&t->keys.data[key], &t->keys.data[key + 1], sizeof(A3AnimKey) * (t->keys.count - key - 1));
    t->keys.count--;
}

static f32 hermite(f32 p1, f32 p2, f32 m1, f32 m2, f32 u) {
    f32 u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p1 + (u3 - 2 * u2 + u) * m1 + (-2 * u3 + 3 * u2) * p2 + (u3 - u2) * m2;
}

void a3_anim_track_sample(const A3AnimTrack *t, f32 time, b32 quat, f32 out[4]) {
    u32 n = t->keys.count;
    if (!n) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    const A3AnimKey *k = t->keys.data;
    if (n == 1 || time <= k[0].time) { for (int i = 0; i < 4; ++i) out[i] = k[0].value[i]; return; }
    if (time >= k[n - 1].time) { for (int i = 0; i < 4; ++i) out[i] = k[n - 1].value[i]; return; }
    /* binary search: k[a].time <= time < k[a+1].time */
    u32 lo = 0, hi = n - 1;
    while (hi - lo > 1) { u32 mid = (lo + hi) / 2; if (k[mid].time <= time) lo = mid; else hi = mid; }
    const A3AnimKey *a = &k[lo], *b = &k[lo + 1];
    f32 span = b->time - a->time;
    f32 u = span > 1e-6f ? (time - a->time) / span : 1.0f;
    if (a->interp == A3_INTERP_STEP) { for (int i = 0; i < 4; ++i) out[i] = a->value[i]; return; }
    if (quat) {
        /* shortest-path normalized blend; smooth keys ease in and out */
        if (a->interp == A3_INTERP_SMOOTH) u = u * u * (3 - 2 * u);
        f32 dot = a->value[0] * b->value[0] + a->value[1] * b->value[1] + a->value[2] * b->value[2] + a->value[3] * b->value[3];
        f32 s = dot < 0 ? -1.0f : 1.0f, len = 0;
        for (int i = 0; i < 4; ++i) { out[i] = a->value[i] + (b->value[i] * s - a->value[i]) * u; len += out[i] * out[i]; }
        len = a3_sqrtf(len);
        if (len > 1e-8f) for (int i = 0; i < 4; ++i) out[i] /= len;
        return;
    }
    if (a->interp == A3_INTERP_LINEAR) { for (int i = 0; i < 4; ++i) out[i] = a->value[i] + (b->value[i] - a->value[i]) * u; return; }
    /* smooth: Catmull-Rom tangents from the neighbours, flat at the first/last key */
    const A3AnimKey *p0 = lo > 0 ? &k[lo - 1] : 0, *p3 = lo + 2 < n ? &k[lo + 2] : 0;
    for (int i = 0; i < 4; ++i) {
        f32 m1 = p0 ? (b->value[i] - p0->value[i]) / (b->time - p0->time) * span : 0.0f;
        f32 m2 = p3 ? (p3->value[i] - a->value[i]) / (p3->time - a->time) * span : 0.0f;
        out[i] = hermite(a->value[i], b->value[i], m1, m2, u);
    }
}

f32 a3_anim_clip_length(const A3AnimClip *c) {
    f32 len = 0;
    for (u32 i = 0; i < c->track_count; ++i)
        if (c->tracks[i].keys.count) len = a3_maxf(len, c->tracks[i].keys.data[c->tracks[i].keys.count - 1].time);
    return len;
}

/* ======================================================================== */
/* Applying to components                                                   */
/* ======================================================================== */

b32 a3_anim_field_animatable(u32 type) {
    switch (type) {
    case A3_FIELD_BOOL: case A3_FIELD_I32: case A3_FIELD_U32: case A3_FIELD_F32: case A3_FIELD_VEC2: case A3_FIELD_VEC3:
    case A3_FIELD_VEC4: case A3_FIELD_COLOR: case A3_FIELD_QUAT: case A3_FIELD_ENUM: return 1;
    default: return 0;
    }
}

static A3Entity find_child(A3World *w, A3Entity root, const char *name, u32 depth) {
    if (depth > 32) return A3_ENTITY_NULL;
    for (A3Entity c = a3_entity_first_child(w, root); !a3_entity_is_null(c); c = a3_entity_next_sibling(w, c)) {
        if (a3_streq(a3_entity_name(w, c), name)) return c;
        A3Entity r = find_child(w, c, name, depth + 1);
        if (!a3_entity_is_null(r)) return r;
    }
    return A3_ENTITY_NULL;
}

static u8 *field_ptr(A3World *w, A3Entity e, const char *component, const char *field, const A3FieldDesc **out_fd) {
    A3ComponentType *ct = a3_component_type_by_name(component);
    if (!ct) return 0;
    const A3FieldDesc *fd = a3_component_find_field(ct, field);
    if (!fd || !a3_anim_field_animatable(fd->type)) return 0;
    u8 *data = (u8 *)a3_component_get(w, e, ct->id);
    if (!data) return 0;
    *out_fd = fd;
    return data + fd->offset;
}

b32 a3_anim_read_field(A3World *w, A3Entity e, const char *component, const char *field, f32 out[4]) {
    const A3FieldDesc *fd;
    u8 *p = field_ptr(w, e, component, field, &fd);
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!p) return 0;
    switch (fd->type) {
    case A3_FIELD_BOOL: out[0] = *(b32 *)p ? 1.0f : 0.0f; break;
    case A3_FIELD_I32: case A3_FIELD_ENUM: out[0] = (f32)*(i32 *)p; break;
    case A3_FIELD_U32: out[0] = (f32)*(u32 *)p; break;
    case A3_FIELD_F32: out[0] = *(f32 *)p; break;
    case A3_FIELD_VEC2: a3_memcpy(out, p, 8); break;
    case A3_FIELD_VEC3: a3_memcpy(out, p, 12); break;
    default: a3_memcpy(out, p, 16); break; /* vec4, color, quat */
    }
    return 1;
}

static void write_field(u8 *p, const A3FieldDesc *fd, const f32 v[4]) {
    switch (fd->type) {
    case A3_FIELD_BOOL: *(b32 *)p = v[0] > 0.5f; break;
    case A3_FIELD_I32: case A3_FIELD_ENUM: *(i32 *)p = (i32)a3_floorf(v[0] + 0.5f); break;
    case A3_FIELD_U32: *(u32 *)p = (u32)a3_maxf(a3_floorf(v[0] + 0.5f), 0); break;
    case A3_FIELD_F32: {
        f32 x = v[0];
        if (fd->max > fd->min) x = a3_clampf(x, fd->min, fd->max);
        *(f32 *)p = x;
    } break;
    case A3_FIELD_VEC2: a3_memcpy(p, v, 8); break;
    case A3_FIELD_VEC3: a3_memcpy(p, v, 12); break;
    case A3_FIELD_QUAT: {
        A3Quat q = a3_quat_normalize(*(const A3Quat *)(const void *)v);
        a3_memcpy(p, &q, 16);
    } break;
    default: a3_memcpy(p, v, 16); break;
    }
}

void a3_anim_apply(const A3AnimClip *c, A3World *w, A3Entity e, f32 time) {
    if (!c || !a3_entity_valid(w, e)) return;
    for (u32 i = 0; i < c->track_count; ++i) {
        const A3AnimTrack *t = &c->tracks[i];
        if (!t->keys.count) continue;
        A3Entity target = t->target[0] ? find_child(w, e, t->target, 0) : e;
        if (!a3_entity_valid(w, target)) continue;
        const A3FieldDesc *fd;
        u8 *p = field_ptr(w, target, t->component, t->field, &fd);
        if (!p) continue;
        f32 v[4];
        a3_anim_track_sample(t, time, fd->type == A3_FIELD_QUAT, v);
        write_field(p, fd, v);
    }
}

/* ======================================================================== */
/* JSON                                                                     */
/* ======================================================================== */

void a3_anim_save_json(const A3AnimClip *c, A3StrBuf *out) {
    A3JsonWriter jw;
    a3_jw_init(&jw, out, 0);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", "asm3d.animation");
    a3_jw_kv_int(&jw, "version", 1);
    a3_jw_kv_string(&jw, "name", c->name);
    a3_jw_kv_number(&jw, "duration", c->duration);
    a3_jw_key(&jw, "tracks");
    a3_jw_begin_array(&jw);
    for (u32 i = 0; i < c->track_count; ++i) {
        const A3AnimTrack *t = &c->tracks[i];
        a3_jw_begin_object(&jw);
        if (t->target[0]) a3_jw_kv_string(&jw, "target", t->target);
        a3_jw_kv_string(&jw, "component", t->component);
        a3_jw_kv_string(&jw, "field", t->field);
        a3_jw_key(&jw, "keys");
        a3_jw_begin_array(&jw);
        for (u32 k = 0; k < t->keys.count; ++k) {
            const A3AnimKey *key = &t->keys.data[k];
            f32 row[6] = { key->time, key->value[0], key->value[1], key->value[2], key->value[3], (f32)key->interp };
            a3_jw_floats(&jw, row, 6);
        }
        a3_jw_end_array(&jw);
        a3_jw_end_object(&jw);
    }
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    a3_strbuf_append_char(out, '\n');
}

b32 a3_anim_load_json(A3AnimClip *c, const char *text, usize len, char *error, usize cap) {
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(64));
    A3JsonError err;
    A3Json *root = a3_json_parse(text, len, &ar, &err);
    b32 ok = 0;
    if (!root) { a3_snprintf(error, cap, "line %d: %s", err.line, err.message); goto done; }
    if (!a3_streq(a3_json_get_string(root, "format", ""), "asm3d.animation")) { a3_snprintf(error, cap, "not an ASM3D animation file"); goto done; }
    if (a3_json_get_number(root, "version", 1) > 1) { a3_snprintf(error, cap, "made with a newer ASM3D"); goto done; }
    a3_anim_clip_init(c, a3_json_get_string(root, "name", "Animation"));
    c->duration = (f32)a3_json_get_number(root, "duration", 0);
    A3_JSON_FOREACH(jt, a3_json_get(root, "tracks")) {
        i32 ti = a3_anim_track_add(c, a3_json_get_string(jt, "target", ""), a3_json_get_string(jt, "component", ""), a3_json_get_string(jt, "field", ""));
        if (ti < 0) break;
        A3_JSON_FOREACH(jk, a3_json_get(jt, "keys")) {
            f32 row[6] = { 0 };
            a3_json_get_floats(jk, row, 6);
            a3_anim_key_set(&c->tracks[ti], row[0], row + 1, a3_clampi((i32)row[5], 0, A3_INTERP_COUNT - 1));
        }
    }
    if (c->duration <= 0) c->duration = a3_maxf(a3_anim_clip_length(c), 0.1f);
    ok = 1;
done:
    a3_arena_release(&ar);
    return ok;
}

/* ======================================================================== */
/* Cache                                                                    */
/* ======================================================================== */

typedef struct CacheEntry { u64 key; b32 failed; A3AnimClip clip; } CacheEntry;
static A3_ARRAY_TYPE(CacheEntry *) g_cache;

const A3AnimClip *a3_anim_clip_get(const char *path) {
    if (!path || !*path) return 0;
    u64 key = a3_hash_str(path);
    for (u32 i = 0; i < g_cache.count; ++i) if (g_cache.data[i]->key == key) return g_cache.data[i]->failed ? 0 : &g_cache.data[i]->clip;
    CacheEntry *ce = A3_NEW(CacheEntry, A3_MEM_ANIM);
    if (!ce) return 0;
    ce->key = key;
    char abs[A3_PATH_MAX * 2], err[160] = "";
    a3_assets_path(path, abs, sizeof(abs));
    A3FileData fd;
    if (a3_file_read_all(abs, A3_MEM_TEMP, &fd) != A3_OK) { ce->failed = 1; a3_snprintf(err, sizeof(err), "file not found"); }
    else {
        if (!a3_anim_load_json(&ce->clip, (const char *)fd.data, fd.size, err, sizeof(err))) ce->failed = 1;
        a3_free(fd.data);
    }
    if (ce->failed) a3_log_hint(A3_LOG_ERROR, "anim", "Check the Clip field of the Animator.", "could not load animation '%s': %s", path, err);
    if (!a3_array_push(g_cache, ce, A3_MEM_ANIM)) { a3_anim_clip_free(&ce->clip); a3_free(ce); return 0; }
    return ce->failed ? 0 : &ce->clip;
}

void a3_anim_clip_invalidate(const char *path) {
    u64 key = a3_hash_str(path);
    for (u32 i = 0; i < g_cache.count; ++i) {
        if (g_cache.data[i]->key != key) continue;
        a3_anim_clip_free(&g_cache.data[i]->clip);
        a3_free(g_cache.data[i]);
        a3_array_remove_swap(g_cache, i);
        return;
    }
}

void a3_anim_cache_clear(void) {
    for (u32 i = 0; i < g_cache.count; ++i) { a3_anim_clip_free(&g_cache.data[i]->clip); a3_free(g_cache.data[i]); }
    a3_array_free(g_cache);
}

/* ======================================================================== */
/* Components & update                                                      */
/* ======================================================================== */

void a3_anim_update(A3World *w, f32 dt) {
    if (!w || A3_T_ANIMATOR == 0xFFFFFFFFu) return;
    u32 n = 0;
    const A3Entity *ents = 0;
    A3CAnimator *an = (A3CAnimator *)a3_component_array(w, A3_T_ANIMATOR, &n, &ents);
    for (u32 i = 0; i < n; ++i) {
        A3CAnimator *a = &an[i];
        if (!a3_entity_active(w, ents[i])) continue;
        const A3AnimClip *clip = a3_anim_clip_get(a->clip.path);
        if (!a->started) { a->started = 1; a->time = 0; a->direction = 1; if (a->play_on_start) a->playing = 1; }
        if (!clip || !a->playing) continue;
        f32 len = clip->duration > 0 ? clip->duration : a3_anim_clip_length(clip);
        if (len <= 0) continue;
        a->time += dt * a->speed * (a->direction == 0 ? 1.0f : a->direction);
        if (a->loop == A3_ANIM_LOOP) {
            a->time = a3_fmodf(a->time, len);
            if (a->time < 0) a->time += len;
        } else if (a->loop == A3_ANIM_PINGPONG) {
            if (a->time > len) { a->time = 2 * len - a->time; a->direction = -1; }
            if (a->time < 0) { a->time = -a->time; a->direction = 1; }
            a->time = a3_clampf(a->time, 0, len);
        } else if (a->time >= len) {
            a->time = len;
            a->playing = 0;
        }
        a3_anim_apply(clip, w, ents[i], a->time);
    }
    /* procedural motion */
    A3CMotion *mo = (A3CMotion *)a3_component_array(w, A3_T_MOTION, &n, &ents);
    for (u32 i = 0; i < n; ++i) {
        A3CMotion *m = &mo[i];
        A3CTransform *t = a3_transform(w, ents[i]);
        if (!t || !m->active || !a3_entity_active(w, ents[i])) continue;
        if (!m->started) { m->started = 1; m->time = 0; m->base_pos = t->position; m->base_scale = t->scale; m->base_rot = t->rotation; }
        m->time += dt;
        f32 tt = m->time;
        A3Vec3 p = m->base_pos;
        if (m->bob_height != 0) p.y += a3_sinf(A3_TAU * (m->bob_speed * tt + m->phase)) * m->bob_height;
        if (m->move_speed != 0) {
            f32 k = 0.5f - 0.5f * a3_cosf(A3_TAU * (m->move_speed * tt + m->phase));
            p = a3_v3_madd(p, m->move_offset, k);
        }
        t->position = p;
        if (m->spin.x != 0 || m->spin.y != 0 || m->spin.z != 0) {
            A3Quat q = a3_quat_euler(m->spin.x * tt * A3_DEG2RAD, m->spin.y * tt * A3_DEG2RAD, m->spin.z * tt * A3_DEG2RAD);
            t->rotation = a3_quat_normalize(a3_quat_mul(m->base_rot, q));
        }
        if (m->pulse_amount != 0) t->scale = a3_v3_scale(m->base_scale, 1.0f + m->pulse_amount * a3_sinf(A3_TAU * (m->pulse_speed * tt + m->phase)));
    }
}

static const char *const g_loop_names[] = { "Once", "Loop", "Ping-Pong" };

void a3_anim_register(void) {
    if (A3_T_ANIMATOR != 0xFFFFFFFFu && a3_component_type(A3_T_ANIMATOR)) return;
    A3CAnimator d;
    a3_zero_struct(&d);
    d.play_on_start = 1; d.loop = A3_ANIM_LOOP; d.speed = 1.0f; d.direction = 1;
    d.clip.kind = A3_ASSET_ANIMATION;
    u32 t = a3_component_register("Animator", "Animation", sizeof(A3CAnimator), 16, &d, A3_COMP_BUILTIN,
        "Plays a keyframe animation (.a3anim) made in the Animation panel: doors opening, lights flickering, platforms moving along a path.");
    A3_T_ANIMATOR = t;
    a3_component_type(t)->icon = "animation";
    A3FieldDesc *f = A3_REFLECT_FIELD(t, A3CAnimator, clip, A3_FIELD_ASSET, "Clip", "The animation file to play.");
    f->asset_kind = A3_ASSET_ANIMATION;
    A3_REFLECT_FIELD(t, A3CAnimator, play_on_start, A3_FIELD_BOOL, "Play On Start", "Start playing when the game starts.");
    f = A3_REFLECT_FIELD(t, A3CAnimator, loop, A3_FIELD_ENUM, "Repeat", "Once stops at the end; Loop starts over; Ping-Pong plays back and forth.");
    f->enum_names = g_loop_names; f->enum_count = 3;
    a3_field_range(A3_REFLECT_FIELD(t, A3CAnimator, speed, A3_FIELD_F32, "Speed", "1 = normal, 2 = twice as fast, -1 = backwards."), -10, 10, 0.01f);
    A3_REFLECT_FIELD(t, A3CAnimator, playing, A3_FIELD_BOOL, "Playing", "Runtime: set from gameplay to start or pause.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CAnimator, time, A3_FIELD_F32, "Time", "Runtime: current position in seconds.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY | A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CAnimator, direction, A3_FIELD_F32, "Direction", "Runtime.")->flags |= A3_FIELD_FLAG_HIDDEN | A3_FIELD_FLAG_TRANSIENT;
    A3_REFLECT_FIELD(t, A3CAnimator, started, A3_FIELD_BOOL, "Started", "Runtime.")->flags |= A3_FIELD_FLAG_HIDDEN | A3_FIELD_FLAG_TRANSIENT;

    A3CMotion m;
    a3_zero_struct(&m);
    m.spin = a3_v3(0, 90, 0); m.bob_speed = 0.5f; m.pulse_speed = 1.0f; m.move_speed = 0.25f; m.active = 1;
    t = a3_component_register("Motion", "Animation", sizeof(A3CMotion), 16, &m, A3_COMP_BUILTIN,
        "Simple movement without keyframes: spin, bob up and down, pulse, or slide back and forth. Great for coins, pickups and moving platforms.");
    A3_T_MOTION = t;
    a3_component_type(t)->icon = "motion";
    a3_component_require(t, "Transform");
    A3_REFLECT_FIELD(t, A3CMotion, active, A3_FIELD_BOOL, "Active", "Turn the motion on or off.");
    A3_REFLECT_FIELD(t, A3CMotion, spin, A3_FIELD_VEC3, "Spin", "Degrees per second around X, Y and Z. (0, 90, 0) turns once every 4 seconds.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CMotion, bob_height, A3_FIELD_F32, "Bob Height", "Meters up and down (0 = off)."), 0, 100, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CMotion, bob_speed, A3_FIELD_F32, "Bob Speed", "Bobs per second."), 0, 20, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CMotion, pulse_amount, A3_FIELD_F32, "Pulse Amount", "Grow and shrink: 0.2 = 20% (0 = off)."), 0, 2, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CMotion, pulse_speed, A3_FIELD_F32, "Pulse Speed", "Pulses per second."), 0, 20, 0.01f);
    A3_REFLECT_FIELD(t, A3CMotion, move_offset, A3_FIELD_VEC3, "Move Distance", "Slides between the start and start + this offset (moving platforms).");
    a3_field_range(A3_REFLECT_FIELD(t, A3CMotion, move_speed, A3_FIELD_F32, "Move Speed", "Round trips per second."), 0, 10, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CMotion, phase, A3_FIELD_F32, "Offset", "0..1: start at a different point so copies don't move in sync."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER | A3_FIELD_FLAG_ADVANCED;
}
