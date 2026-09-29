/*
 * ASM3D - a3_particles.h
 * Particle effects: ParticleEmitter component, simulation (SSE assembly
 * integration kernel with a bit-exact C reference), and camera-facing
 * billboards batched for the renderer (additive or alpha blended).
 */
#ifndef A3_PARTICLES_H
#define A3_PARTICLES_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"
#include "../ecs/a3_ecs.h"

A3_EXTERN_C_BEGIN

extern u32 A3_T_PARTICLE_EMITTER;

typedef enum A3EmitShape { A3_EMIT_POINT = 0, A3_EMIT_SPHERE, A3_EMIT_BOX, A3_EMIT_SHAPE_COUNT } A3EmitShape;
typedef enum A3ParticleBlend { A3_PARTICLE_ADDITIVE = 0, A3_PARTICLE_ALPHA } A3ParticleBlend;

typedef struct A3CParticleEmitter {
    b32 emitting;             /* spawning new particles */
    f32 rate;                 /* particles per second */
    i32 burst;                /* particles spawned at once when it starts */
    b32 loop;                 /* keep emitting; otherwise stop after `duration` */
    f32 duration;
    f32 lifetime;             /* seconds */
    f32 lifetime_random;      /* 0..1 variation */
    f32 speed;
    f32 speed_random;
    f32 spread;               /* cone half angle in degrees around the entity's up axis */
    i32 shape;                /* A3EmitShape */
    f32 radius;               /* sphere */
    A3Vec3 box_size; f32 _p0; /* box */
    A3Vec3 gravity;           /* acceleration, m/s^2 */
    f32 drag;                 /* air resistance, 1/s */
    f32 size_start, size_end;
    f32 size_random;
    f32 _p1;
    A3Vec4 color_start, color_end; /* alpha fades too */
    i32 blend;                /* A3ParticleBlend */
    b32 world_space;          /* particles stay behind when the emitter moves */
    i32 max_particles;
    u32 seed;
    A3AssetRef texture;       /* optional; default is a soft round dot */
} A3CParticleEmitter;

typedef enum A3ParticlePreset {
    A3_PARTICLES_FIRE = 0, A3_PARTICLES_SMOKE, A3_PARTICLES_SPARKS, A3_PARTICLES_MAGIC, A3_PARTICLES_RAIN, A3_PARTICLES_SNOW,
    A3_PARTICLES_DUST, A3_PARTICLES_EXPLOSION, A3_PARTICLES_FOUNTAIN, A3_PARTICLES_FIREFLIES, A3_PARTICLES_PRESET_COUNT
} A3ParticlePreset;
extern const char *const a3_particle_preset_names[A3_PARTICLES_PRESET_COUNT];
void a3_particles_preset(A3CParticleEmitter *e, u32 preset);

void a3_particles_register(void);                  /* component + world cleanup */
/* Simulates every emitter of the world (runs in the editor too, for preview). */
void a3_particles_update(A3World *w, f32 dt);
void a3_particles_release(A3World *w);             /* frees the world's particle pools */
/* Restarts an emitter (burst + duration timer). */
void a3_particles_restart(A3World *w, A3Entity e);
u32  a3_particles_count(A3World *w);                /* live particles */

/* ---- rendering ---- */
typedef struct A3ParticleInstance { A3Vec3 pos; f32 size; A3Vec4 color; } A3ParticleInstance; /* linear color */
typedef struct A3ParticleBatch {
    u32 first, count;         /* range in the instance array */
    i32 blend;                /* A3ParticleBlend */
    u32 texture;              /* texture asset id, 0 = soft dot */
    f32 distance;             /* emitter distance to the camera (alpha batches draw far to near) */
} A3ParticleBatch;
/* Builds camera-facing instances for all emitters (alpha ones sorted back to
 * front). The returned instance array stays valid until the next call. */
u32 a3_particles_collect(A3World *w, A3Vec3 camera_pos, A3ParticleBatch *batches, u32 max_batches, const A3ParticleInstance **out_instances, u32 *out_count);

/* ---- kernel (see a3_particles_x64.S) ----
 * p = { k (velocity damping factor), gravity*dt x, y, z, dt }
 *   vel.xyz = vel.xyz * k + g*dt          vel.w (lifetime) unchanged
 *   pos.xyz = pos.xyz + vel.xyz * dt      pos.w (age) += dt                */
void a3_particles_integrate(A3Vec4 *pos_age, A3Vec4 *vel_life, u32 count, const f32 *p);
void a3_particles_ref_integrate(A3Vec4 *pos_age, A3Vec4 *vel_life, u32 count, const f32 *p);
const char *a3_particles_kernel_backend(void);

A3_EXTERN_C_END

#endif
