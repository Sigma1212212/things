/*
 * ASM3D - a3_particles.c
 * Particle pools (one per emitter, owned by this module and keyed by world
 * + entity GUID so scene copies for Play mode never share memory),
 * spawning, simulation, and billboard collection for the renderer.
 */
#include "a3_particles.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_hash.h"
#include "../core/a3_sort.h"
#include "../scene/a3_components.h"
#include "../resource/a3_assets.h"

u32 A3_T_PARTICLE_EMITTER = 0xFFFFFFFFu;

#define MAX_POOLS 256
#define HARD_MAX_PARTICLES 20000

typedef struct Pool {
    b32 used;
    A3World *world;
    u64 guid;
    A3Vec4 *pos;              /* xyz position, w age */
    A3Vec4 *vel;              /* xyz velocity, w lifetime */
    f32 *size_mul;            /* per-particle size variation */
    u32 count, cap;
    f32 emit_acc;
    f32 time;
    b32 started;
    b32 seen;
    A3Vec3 last_origin;
    A3Rng rng;
} Pool;

static struct {
    Pool pools[MAX_POOLS];
    A3_ARRAY_TYPE(A3ParticleInstance) instances;
    A3_ARRAY_TYPE(A3SortPair) keys, tmp;
} g_pt;

/* ======================================================================== */
/* Kernel                                                                   */
/* ======================================================================== */

void a3_particles_ref_integrate(A3Vec4 *pos, A3Vec4 *vel, u32 count, const f32 *p) {
    f32 k = p[0], gx = p[1], gy = p[2], gz = p[3], dt = p[4];
    for (u32 i = 0; i < count; ++i) {
        A3Vec4 v = vel[i];
        v.x = v.x * k + gx;
        v.y = v.y * k + gy;
        v.z = v.z * k + gz;
        v.w = v.w * 1.0f + 0.0f;
        vel[i] = v;
        A3Vec4 q = pos[i];
        q.x = (q.x + v.x * dt) + 0.0f;
        q.y = (q.y + v.y * dt) + 0.0f;
        q.z = (q.z + v.z * dt) + 0.0f;
        q.w = (q.w + v.w * 0.0f) + dt;
        pos[i] = q;
    }
}

#if A3_HAS_X64_ASM
void a3_particles_asm_integrate(A3Vec4 *pos, A3Vec4 *vel, u32 count, const f32 *p);
void a3_particles_integrate(A3Vec4 *pos, A3Vec4 *vel, u32 count, const f32 *p) { a3_particles_asm_integrate(pos, vel, count, p); }
const char *a3_particles_kernel_backend(void) { return "x86-64 SSE assembly"; }
#else
void a3_particles_integrate(A3Vec4 *pos, A3Vec4 *vel, u32 count, const f32 *p) { a3_particles_ref_integrate(pos, vel, count, p); }
const char *a3_particles_kernel_backend(void) { return "C reference"; }
#endif

/* ======================================================================== */
/* Pools                                                                    */
/* ======================================================================== */

static void pool_free(Pool *p) {
    a3_free(p->pos);
    a3_free(p->vel);
    a3_free(p->size_mul);
    a3_zero_struct(p);
}

static Pool *pool_get(A3World *w, u64 guid, u32 seed) {
    Pool *free_slot = 0;
    for (u32 i = 0; i < MAX_POOLS; ++i) {
        Pool *p = &g_pt.pools[i];
        if (p->used && p->world == w && p->guid == guid) return p;
        if (!p->used && !free_slot) free_slot = p;
    }
    if (!free_slot) return 0;
    a3_zero_struct(free_slot);
    free_slot->used = 1;
    free_slot->world = w;
    free_slot->guid = guid;
    a3_rng_seed(&free_slot->rng, guid ^ ((u64)seed << 32), 17);
    return free_slot;
}

static b32 pool_reserve(Pool *p, u32 cap) {
    if (cap <= p->cap) return 1;
    u32 nc = p->cap ? p->cap : 64;
    while (nc < cap) nc *= 2;
    A3Vec4 *np = (A3Vec4 *)a3_realloc(p->pos, sizeof(A3Vec4) * nc, A3_MEM_PARTICLES);
    if (!np) return 0;
    p->pos = np;
    A3Vec4 *nv = (A3Vec4 *)a3_realloc(p->vel, sizeof(A3Vec4) * nc, A3_MEM_PARTICLES);
    if (!nv) return 0;
    p->vel = nv;
    f32 *ns = (f32 *)a3_realloc(p->size_mul, sizeof(f32) * nc, A3_MEM_PARTICLES);
    if (!ns) return 0;
    p->size_mul = ns;
    p->cap = nc;
    return 1;
}

void a3_particles_release(A3World *w) {
    for (u32 i = 0; i < MAX_POOLS; ++i) if (g_pt.pools[i].used && g_pt.pools[i].world == w) pool_free(&g_pt.pools[i]);
}

u32 a3_particles_count(A3World *w) {
    u32 n = 0;
    for (u32 i = 0; i < MAX_POOLS; ++i) if (g_pt.pools[i].used && g_pt.pools[i].world == w) n += g_pt.pools[i].count;
    return n;
}

void a3_particles_restart(A3World *w, A3Entity e) {
    for (u32 i = 0; i < MAX_POOLS; ++i) {
        Pool *p = &g_pt.pools[i];
        if (p->used && p->world == w && p->guid == a3_entity_guid(w, e)) { p->started = 0; p->time = 0; p->emit_acc = 0; }
    }
}

/* ======================================================================== */
/* Simulation                                                               */
/* ======================================================================== */

static A3Vec3 random_unit(A3Rng *r) {
    /* uniform direction: z uniform in [-1, 1], angle uniform */
    f32 z = a3_rng_range_f32(r, -1, 1), a = a3_rng_range_f32(r, 0, A3_TAU);
    f32 s = a3_sqrtf(a3_maxf(1.0f - z * z, 0)), sn, cs;
    a3_sincosf(a, &sn, &cs);
    return a3_v3(s * cs, s * sn, z);
}

/* Direction inside a cone of half-angle `spread` around `axis`. */
static A3Vec3 cone_dir(A3Rng *r, A3Vec3 axis, f32 spread_deg) {
    f32 cos_max = a3_cosf(a3_clampf(spread_deg, 0, 180) * A3_DEG2RAD);
    f32 z = a3_rng_range_f32(r, cos_max, 1.0f), a = a3_rng_range_f32(r, 0, A3_TAU);
    f32 s = a3_sqrtf(a3_maxf(1.0f - z * z, 0)), sn, cs;
    a3_sincosf(a, &sn, &cs);
    A3Vec3 t = a3_absf(axis.y) < 0.99f ? a3_v3_norm(a3_v3_cross(axis, a3_v3(0, 1, 0))) : a3_v3(1, 0, 0);
    A3Vec3 b = a3_v3_cross(axis, t);
    return a3_v3_add(a3_v3_add(a3_v3_scale(t, s * cs), a3_v3_scale(b, s * sn)), a3_v3_scale(axis, z));
}

static void spawn(Pool *p, const A3CParticleEmitter *em, const A3Mat4 *world, u32 n) {
    u32 max = (u32)a3_clampi(em->max_particles, 1, HARD_MAX_PARTICLES);
    if (p->count + n > max) n = max > p->count ? max - p->count : 0;
    if (!n || !pool_reserve(p, p->count + n)) return;
    A3Vec3 origin = a3_mat4_get_translation(world);
    A3Vec3 up = a3_v3_norm(a3_mat4_mul_dir(world, a3_v3(0, 1, 0)));
    if (a3_v3_len(up) < 0.5f) up = a3_v3(0, 1, 0);
    for (u32 i = 0; i < n; ++i) {
        A3Vec3 off = a3_v3_zero();
        if (em->shape == A3_EMIT_SPHERE) off = a3_v3_scale(random_unit(&p->rng), em->radius * a3_powf(a3_rng_f32(&p->rng), 1.0f / 3.0f));
        else if (em->shape == A3_EMIT_BOX)
            off = a3_mat4_mul_dir(world, a3_v3(a3_rng_range_f32(&p->rng, -0.5f, 0.5f) * em->box_size.x, a3_rng_range_f32(&p->rng, -0.5f, 0.5f) * em->box_size.y,
                                               a3_rng_range_f32(&p->rng, -0.5f, 0.5f) * em->box_size.z));
        A3Vec3 dir = cone_dir(&p->rng, up, em->spread);
        f32 speed = em->speed * (1.0f + a3_rng_range_f32(&p->rng, -1, 1) * a3_clampf(em->speed_random, 0, 1));
        f32 life = a3_maxf(em->lifetime * (1.0f + a3_rng_range_f32(&p->rng, -1, 1) * a3_clampf(em->lifetime_random, 0, 0.95f)), 0.01f);
        u32 k = p->count++;
        A3Vec3 pos = a3_v3_add(origin, off);
        p->pos[k] = a3_v4(pos.x, pos.y, pos.z, 0);
        p->vel[k] = a3_v4(dir.x * speed, dir.y * speed, dir.z * speed, life);
        p->size_mul[k] = 1.0f + a3_rng_range_f32(&p->rng, -1, 1) * a3_clampf(em->size_random, 0, 0.95f);
    }
}

void a3_particles_update(A3World *w, f32 dt) {
    if (!w || A3_T_PARTICLE_EMITTER == 0xFFFFFFFFu) return;
    for (u32 i = 0; i < MAX_POOLS; ++i) if (g_pt.pools[i].world == w) g_pt.pools[i].seen = 0;
    dt = a3_clampf(dt, 0, 0.1f);
    u32 n = 0;
    const A3Entity *ents = 0;
    A3CParticleEmitter *ems = (A3CParticleEmitter *)a3_component_array(w, A3_T_PARTICLE_EMITTER, &n, &ents);
    for (u32 i = 0; i < n; ++i) {
        A3CParticleEmitter *em = &ems[i];
        Pool *p = pool_get(w, a3_entity_guid(w, ents[i]), em->seed);
        if (!p) continue;
        p->seen = 1;
        b32 active = a3_entity_active(w, ents[i]);
        A3Mat4 world = a3_transform_compute_world(w, ents[i]);
        A3Vec3 origin = a3_mat4_get_translation(&world);
        /* local-space effects move with the emitter */
        if (!em->world_space && p->started) {
            A3Vec3 d = a3_v3_sub(origin, p->last_origin);
            for (u32 k = 0; k < p->count; ++k) { p->pos[k].x += d.x; p->pos[k].y += d.y; p->pos[k].z += d.z; }
        }
        p->last_origin = origin;
        if (active && em->emitting) {
            if (!p->started) {
                p->started = 1;
                p->time = 0;
                p->emit_acc = 0;
                if (em->burst > 0) spawn(p, em, &world, (u32)em->burst);
            }
            b32 within = em->loop || p->time < em->duration;
            if (within && em->rate > 0) {
                p->emit_acc += em->rate * dt;
                u32 count = (u32)p->emit_acc;
                p->emit_acc -= (f32)count;
                if (count) spawn(p, em, &world, count);
            }
            p->time += dt;
        } else if (!em->emitting) {
            p->started = 0;
        }
        /* simulate */
        if (p->count && dt > 0) {
            f32 k = a3_maxf(1.0f - em->drag * dt, 0.0f);
            f32 params[5] = { k, em->gravity.x * dt, em->gravity.y * dt, em->gravity.z * dt, dt };
            a3_particles_integrate(p->pos, p->vel, p->count, params);
            /* remove dead particles (swap with the last) */
            for (u32 k2 = 0; k2 < p->count;) {
                if (p->pos[k2].w >= p->vel[k2].w) {
                    u32 last = --p->count;
                    p->pos[k2] = p->pos[last];
                    p->vel[k2] = p->vel[last];
                    p->size_mul[k2] = p->size_mul[last];
                } else ++k2;
            }
        }
    }
    /* emitters that were removed or deleted */
    for (u32 i = 0; i < MAX_POOLS; ++i) if (g_pt.pools[i].used && g_pt.pools[i].world == w && !g_pt.pools[i].seen) pool_free(&g_pt.pools[i]);
}

/* ======================================================================== */
/* Rendering                                                                */
/* ======================================================================== */

static A3Vec4 lin(A3Vec4 c) { return a3_v4(a3_srgb_to_linear(c.x), a3_srgb_to_linear(c.y), a3_srgb_to_linear(c.z), c.w); }

u32 a3_particles_collect(A3World *w, A3Vec3 cam, A3ParticleBatch *batches, u32 max_batches, const A3ParticleInstance **out, u32 *out_count) {
    a3_array_clear(g_pt.instances);
    *out = 0;
    *out_count = 0;
    if (!w || A3_T_PARTICLE_EMITTER == 0xFFFFFFFFu) return 0;
    u32 nb = 0;
    for (u32 i = 0; i < MAX_POOLS && nb < max_batches; ++i) {
        Pool *p = &g_pt.pools[i];
        if (!p->used || p->world != w || !p->count) continue;
        A3Entity e = a3_entity_find_by_guid(w, p->guid);
        A3CParticleEmitter *em = (A3CParticleEmitter *)a3_component_get(w, e, A3_T_PARTICLE_EMITTER);
        if (!em) continue;
        A3ParticleBatch *b = &batches[nb++];
        b->first = g_pt.instances.count;
        b->count = p->count;
        b->blend = em->blend;
        b->texture = em->texture.path[0] ? a3_assets_resolve_texture(&em->texture) : 0;
        b->distance = a3_v3_dist(p->last_origin, cam);
        A3Vec4 c0 = lin(em->color_start), c1 = lin(em->color_end);
        u32 base = g_pt.instances.count;
        for (u32 k = 0; k < p->count; ++k) {
            f32 t = a3_clampf(p->pos[k].w / p->vel[k].w, 0, 1);
            A3ParticleInstance inst;
            inst.pos = a3_v3(p->pos[k].x, p->pos[k].y, p->pos[k].z);
            inst.size = (em->size_start + (em->size_end - em->size_start) * t) * p->size_mul[k];
            inst.color = a3_v4_lerp(c0, c1, t);
            /* quick fade-in so particles never pop into existence */
            f32 fade_in = a3_clampf(p->pos[k].w * 12.0f, 0, 1);
            inst.color.w *= fade_in;
            if (!a3_array_push(g_pt.instances, inst, A3_MEM_PARTICLES)) { b->count = k; break; }
        }
        if (em->blend == A3_PARTICLE_ALPHA && b->count > 1) {
            /* back to front inside the emitter */
            /* back to front: key = inverted distance (mm), ascending sort */
            if (a3_array_reserve(g_pt.keys, b->count, A3_MEM_PARTICLES) && a3_array_reserve(g_pt.tmp, b->count, A3_MEM_PARTICLES) &&
                a3_array_reserve(g_pt.instances, g_pt.instances.count + b->count, A3_MEM_PARTICLES)) {
                for (u32 k = 0; k < b->count; ++k) {
                    f32 d = a3_v3_dist(g_pt.instances.data[base + k].pos, cam);
                    A3SortPair sp = { 0xFFFFFFFFull - (u64)a3_clampf(d * 1000.0f, 0, 4.0e9f), k, 0 };
                    g_pt.keys.data[k] = sp;
                }
                a3_radix_sort_pairs(g_pt.keys.data, g_pt.tmp.data, b->count);
                /* reorder through the reserved tail of the instance array */
                A3ParticleInstance *src = g_pt.instances.data + base, *tail = g_pt.instances.data + g_pt.instances.count;
                for (u32 k = 0; k < b->count; ++k) tail[k] = src[g_pt.keys.data[k].value];
                a3_memcpy(src, tail, sizeof(A3ParticleInstance) * b->count);
            }
        }
    }
    /* alpha batches far to near, additive ones first (order independent) */
    for (u32 i = 1; i < nb; ++i) {
        A3ParticleBatch t = batches[i];
        u32 j = i;
        while (j > 0) {
            A3ParticleBatch *pb = &batches[j - 1];
            b32 swap = (pb->blend == A3_PARTICLE_ALPHA && t.blend == A3_PARTICLE_ADDITIVE) ||
                       (pb->blend == t.blend && t.blend == A3_PARTICLE_ALPHA && pb->distance < t.distance);
            if (!swap) break;
            batches[j] = batches[j - 1];
            --j;
        }
        batches[j] = t;
    }
    *out = g_pt.instances.data;
    *out_count = g_pt.instances.count;
    return nb;
}

/* ======================================================================== */
/* Presets & registration                                                   */
/* ======================================================================== */

const char *const a3_particle_preset_names[A3_PARTICLES_PRESET_COUNT] = {
    "Fire", "Smoke", "Sparks", "Magic", "Rain", "Snow", "Dust", "Explosion", "Fountain", "Fireflies",
};

static void base_defaults(A3CParticleEmitter *e) {
    a3_zero_struct(e);
    e->emitting = 1; e->rate = 40; e->loop = 1; e->duration = 5; e->lifetime = 1.5f; e->lifetime_random = 0.3f;
    e->speed = 2; e->speed_random = 0.3f; e->spread = 20; e->shape = A3_EMIT_SPHERE; e->radius = 0.2f; e->box_size = a3_v3_one();
    e->gravity = a3_v3(0, 0, 0); e->drag = 0.5f; e->size_start = 0.3f; e->size_end = 0.1f; e->size_random = 0.3f;
    e->color_start = a3_v4(1, 1, 1, 1); e->color_end = a3_v4(1, 1, 1, 0); e->blend = A3_PARTICLE_ADDITIVE;
    e->world_space = 1; e->max_particles = 1000; e->seed = 1;
}

void a3_particles_preset(A3CParticleEmitter *e, u32 preset) {
    A3AssetRef keep = e->texture;
    base_defaults(e);
    e->texture = keep;
    switch (preset) {
    default:
    case A3_PARTICLES_FIRE:
        e->rate = 90; e->lifetime = 0.9f; e->speed = 1.6f; e->spread = 12; e->radius = 0.25f; e->gravity = a3_v3(0, 2.0f, 0);
        e->size_start = 0.55f; e->size_end = 0.1f; e->color_start = a3_v4(1.0f, 0.6f, 0.15f, 0.9f); e->color_end = a3_v4(0.9f, 0.15f, 0.02f, 0);
        break;
    case A3_PARTICLES_SMOKE:
        e->rate = 18; e->lifetime = 4.0f; e->speed = 0.8f; e->spread = 15; e->radius = 0.3f; e->gravity = a3_v3(0.2f, 0.4f, 0);
        e->size_start = 0.5f; e->size_end = 2.2f; e->blend = A3_PARTICLE_ALPHA; e->color_start = a3_v4(0.35f, 0.35f, 0.37f, 0.55f);
        e->color_end = a3_v4(0.55f, 0.55f, 0.58f, 0); e->drag = 0.3f;
        break;
    case A3_PARTICLES_SPARKS:
        e->rate = 60; e->lifetime = 0.7f; e->speed = 6; e->speed_random = 0.5f; e->spread = 35; e->radius = 0.05f;
        e->gravity = a3_v3(0, -9.81f, 0); e->drag = 0.2f; e->size_start = 0.08f; e->size_end = 0.02f;
        e->color_start = a3_v4(1, 0.85f, 0.4f, 1); e->color_end = a3_v4(1, 0.35f, 0.05f, 0);
        break;
    case A3_PARTICLES_MAGIC:
        e->rate = 50; e->lifetime = 1.8f; e->speed = 0.6f; e->spread = 180; e->radius = 0.6f; e->gravity = a3_v3(0, 0.6f, 0);
        e->size_start = 0.18f; e->size_end = 0.0f; e->color_start = a3_v4(0.55f, 0.35f, 1.0f, 1); e->color_end = a3_v4(0.2f, 0.8f, 1.0f, 0);
        break;
    case A3_PARTICLES_RAIN:
        e->rate = 900; e->lifetime = 1.2f; e->lifetime_random = 0.1f; e->speed = 14; e->speed_random = 0.1f; e->spread = 3;
        e->shape = A3_EMIT_BOX; e->box_size = a3_v3(30, 0.5f, 30); e->gravity = a3_v3(0, -9.81f, 0); e->drag = 0;
        e->size_start = e->size_end = 0.05f; e->size_random = 0.2f; e->blend = A3_PARTICLE_ALPHA; e->max_particles = 4000;
        e->color_start = a3_v4(0.7f, 0.75f, 0.85f, 0.6f); e->color_end = a3_v4(0.7f, 0.75f, 0.85f, 0.5f);
        break;
    case A3_PARTICLES_SNOW:
        e->rate = 250; e->lifetime = 7; e->speed = 1.2f; e->spread = 20; e->shape = A3_EMIT_BOX; e->box_size = a3_v3(30, 0.5f, 30);
        e->gravity = a3_v3(0.15f, -0.25f, 0); e->drag = 0.4f; e->size_start = e->size_end = 0.08f; e->size_random = 0.5f;
        e->blend = A3_PARTICLE_ALPHA; e->max_particles = 3000; e->color_start = a3_v4(1, 1, 1, 0.95f); e->color_end = a3_v4(1, 1, 1, 0.8f);
        break;
    case A3_PARTICLES_DUST:
        e->rate = 12; e->lifetime = 6; e->speed = 0.15f; e->spread = 180; e->shape = A3_EMIT_BOX; e->box_size = a3_v3(8, 3, 8);
        e->size_start = e->size_end = 0.04f; e->blend = A3_PARTICLE_ALPHA; e->drag = 0.1f;
        e->color_start = a3_v4(0.9f, 0.85f, 0.7f, 0.5f); e->color_end = a3_v4(0.9f, 0.85f, 0.7f, 0);
        break;
    case A3_PARTICLES_EXPLOSION:
        e->rate = 0; e->burst = 250; e->loop = 0; e->duration = 0.1f; e->lifetime = 1.1f; e->speed = 7; e->speed_random = 0.6f;
        e->spread = 180; e->radius = 0.4f; e->gravity = a3_v3(0, -2, 0); e->drag = 2.5f; e->size_start = 0.9f; e->size_end = 0.1f;
        e->color_start = a3_v4(1, 0.75f, 0.3f, 1); e->color_end = a3_v4(0.6f, 0.1f, 0.02f, 0);
        break;
    case A3_PARTICLES_FOUNTAIN:
        e->rate = 150; e->lifetime = 2.2f; e->speed = 7; e->speed_random = 0.1f; e->spread = 8; e->radius = 0.05f;
        e->gravity = a3_v3(0, -9.81f, 0); e->drag = 0.05f; e->size_start = 0.12f; e->size_end = 0.08f; e->blend = A3_PARTICLE_ALPHA;
        e->color_start = a3_v4(0.6f, 0.8f, 1.0f, 0.8f); e->color_end = a3_v4(0.8f, 0.9f, 1.0f, 0.0f); e->max_particles = 2000;
        break;
    case A3_PARTICLES_FIREFLIES:
        e->rate = 6; e->lifetime = 6; e->speed = 0.3f; e->spread = 180; e->shape = A3_EMIT_BOX; e->box_size = a3_v3(10, 2, 10);
        e->gravity = a3_v3(0, 0.05f, 0); e->drag = 0.05f; e->size_start = 0.12f; e->size_end = 0.12f;
        e->color_start = a3_v4(0.9f, 1.0f, 0.4f, 1); e->color_end = a3_v4(0.6f, 1.0f, 0.3f, 0);
        break;
    }
}

static const char *const g_shape_names[] = { "Point", "Sphere", "Box" };
static const char *const g_blend_names[] = { "Glow (Additive)", "Solid (Alpha)" };

void a3_particles_register(void) {
    a3_world_on_destroy(a3_particles_release);
    if (A3_T_PARTICLE_EMITTER != 0xFFFFFFFFu && a3_component_type(A3_T_PARTICLE_EMITTER)) return;
    A3CParticleEmitter d;
    a3_particles_preset(&d, A3_PARTICLES_FIRE);
    u32 t = a3_component_register("ParticleEmitter", "Effects", sizeof(A3CParticleEmitter), 16, &d, A3_COMP_BUILTIN,
        "Sprays particles: fire, smoke, sparks, rain, magic... Use Create > Particles for ready-made effects, then tweak them here.");
    A3_T_PARTICLE_EMITTER = t;
    a3_component_type(t)->icon = "particles";
    a3_component_require(t, "Transform");
    A3FieldDesc *f;
    A3_REFLECT_FIELD(t, A3CParticleEmitter, emitting, A3_FIELD_BOOL, "Emitting", "Spawn new particles. Existing ones finish their life.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, rate, A3_FIELD_F32, "Rate", "Particles per second."), 0, 5000, 1);
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, burst, A3_FIELD_I32, "Burst", "Particles spawned at once when it starts (explosions)."), 0, 5000, 1);
    A3_REFLECT_FIELD(t, A3CParticleEmitter, loop, A3_FIELD_BOOL, "Loop", "Keep emitting forever. Off: stop after Duration.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, duration, A3_FIELD_F32, "Duration", "Seconds of emission when Loop is off."), 0, 600, 0.05f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, lifetime, A3_FIELD_F32, "Lifetime", "How long each particle lives, in seconds."), 0.01f, 60, 0.05f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, lifetime_random, A3_FIELD_F32, "Lifetime Variation", "0 = all the same, 1 = very different."), 0, 0.95f, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER | A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, speed, A3_FIELD_F32, "Speed", "Launch speed in meters per second."), 0, 200, 0.05f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, speed_random, A3_FIELD_F32, "Speed Variation", "0 = all the same, 1 = very different."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER | A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, spread, A3_FIELD_F32, "Spread", "Cone angle around the object's up direction (180 = every direction)."), 0, 180, 0.5f)->flags |= A3_FIELD_FLAG_SLIDER;
    f = A3_REFLECT_FIELD(t, A3CParticleEmitter, shape, A3_FIELD_ENUM, "Emit From", "Where new particles appear.");
    f->enum_names = g_shape_names; f->enum_count = A3_EMIT_SHAPE_COUNT;
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, radius, A3_FIELD_F32, "Radius", "Sphere radius for Emit From: Sphere."), 0, 1000, 0.01f);
    A3_REFLECT_FIELD(t, A3CParticleEmitter, box_size, A3_FIELD_VEC3, "Box Size", "Area for Emit From: Box (rain and snow cover large boxes).");
    A3_REFLECT_FIELD(t, A3CParticleEmitter, gravity, A3_FIELD_VEC3, "Gravity", "Constant push in m/s^2: (0, -9.8, 0) falls, positive Y rises like fire.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, drag, A3_FIELD_F32, "Drag", "Air resistance: particles slow down over time."), 0, 20, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, size_start, A3_FIELD_F32, "Start Size", "Size when born, in meters."), 0, 100, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, size_end, A3_FIELD_F32, "End Size", "Size when it dies."), 0, 100, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, size_random, A3_FIELD_F32, "Size Variation", "0 = all the same."), 0, 0.95f, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER | A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CParticleEmitter, color_start, A3_FIELD_COLOR, "Start Color", "Color when born (alpha = opacity).");
    A3_REFLECT_FIELD(t, A3CParticleEmitter, color_end, A3_FIELD_COLOR, "End Color", "Color when it dies. Alpha 0 fades out smoothly.");
    f = A3_REFLECT_FIELD(t, A3CParticleEmitter, blend, A3_FIELD_ENUM, "Look", "Glow adds light (fire, magic, sparks). Solid covers what is behind (smoke, rain, snow).");
    f->enum_names = g_blend_names; f->enum_count = 2;
    A3_REFLECT_FIELD(t, A3CParticleEmitter, world_space, A3_FIELD_BOOL, "Leave Trail", "On: particles stay where they were spawned when the object moves.")->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CParticleEmitter, max_particles, A3_FIELD_I32, "Max Particles", "Upper limit for performance."), 1, HARD_MAX_PARTICLES, 10)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CParticleEmitter, seed, A3_FIELD_U32, "Random Seed", "Change for a different random pattern.")->flags |= A3_FIELD_FLAG_ADVANCED;
    f = A3_REFLECT_FIELD(t, A3CParticleEmitter, texture, A3_FIELD_ASSET, "Texture", "Optional image for each particle (default: soft dot).");
    f->asset_kind = A3_ASSET_TEXTURE;
    f->flags |= A3_FIELD_FLAG_ADVANCED;
}
