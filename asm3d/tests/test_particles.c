/*
 * ASM3D - test_particles.c : integration kernel (asm vs C), emitters, batches
 */
#include "a3_test.h"
#include "../engine/particles/a3_particles.h"
#include "../engine/scene/a3_components.h"
#include "../engine/core/a3_hash.h"
#include "../engine/core/a3_string.h"

static void particles_setup(void) {
    a3_register_core_components();
    a3_particles_register();
}

A3_TEST(particles_kernel_bit_exact) {
    A3Rng rng;
    a3_rng_seed(&rng, 5, 5);
    static A3Vec4 p1[257], v1[257], p2[257], v2[257];
    for (u32 n = 0; n <= 257; n += (n < 20 ? 1 : 59)) {
        for (u32 i = 0; i < n; ++i) {
            p1[i] = p2[i] = a3_v4(a3_rng_range_f32(&rng, -50, 50), a3_rng_range_f32(&rng, -50, 50), a3_rng_range_f32(&rng, -50, 50), a3_rng_f32(&rng));
            v1[i] = v2[i] = a3_v4(a3_rng_range_f32(&rng, -9, 9), a3_rng_range_f32(&rng, -9, 9), a3_rng_range_f32(&rng, -9, 9), a3_rng_range_f32(&rng, 0.1f, 5));
        }
        f32 dt = 1.0f / 60.0f;
        f32 prm[5] = { 1.0f - 0.7f * dt, 0.1f * dt, -9.81f * dt, 0.0f, dt };
        for (int step = 0; step < 5; ++step) {
            a3_particles_integrate(p1, v1, n, prm);
            a3_particles_ref_integrate(p2, v2, n, prm);
        }
        A3_CHECK_MSG(a3_memcmp(p1, p2, sizeof(A3Vec4) * n) == 0 && a3_memcmp(v1, v2, sizeof(A3Vec4) * n) == 0,
                     "particle kernel differs for %u particles (%s)", n, a3_particles_kernel_backend());
    }
    /* lifetime (vel.w) is untouched and age (pos.w) advances by dt */
    A3Vec4 p = a3_v4(0, 10, 0, 0), v = a3_v4(0, 0, 0, 2.5f);
    f32 prm[5] = { 1, 0, -1, 0, 0.5f };
    a3_particles_integrate(&p, &v, 1, prm);
    A3_CHECK(v.w == 2.5f && p.w == 0.5f && v.y == -1.0f && p.y == 9.5f);
}

A3_TEST(particles_emitter_lifecycle) {
    particles_setup();
    A3World *w = a3_world_create("fx");
    A3Entity e = a3_entity_create(w, "Emitter");
    a3_component_add(w, e, A3_T_TRANSFORM);
    A3CParticleEmitter *em = (A3CParticleEmitter *)a3_component_add(w, e, A3_T_PARTICLE_EMITTER);
    a3_particles_preset(em, A3_PARTICLES_FIRE);
    em->rate = 100;
    em->lifetime = 1.0f;
    em->lifetime_random = 0;
    em->burst = 0;
    a3_transform_system_update(w);
    for (int i = 0; i < 30; ++i) a3_particles_update(w, 1.0f / 60.0f);   /* 0.5 s */
    u32 n = a3_particles_count(w);
    A3_CHECK_MSG(n >= 45 && n <= 55, "expected ~50 particles after 0.5 s, got %u", n);
    for (int i = 0; i < 120; ++i) a3_particles_update(w, 1.0f / 60.0f);  /* steady state */
    n = a3_particles_count(w);
    A3_CHECK_MSG(n >= 90 && n <= 105, "expected ~100 particles at steady state, got %u", n);
    /* stop emitting: everything dies within one lifetime */
    em = (A3CParticleEmitter *)a3_component_get(w, e, A3_T_PARTICLE_EMITTER);
    em->emitting = 0;
    for (int i = 0; i < 70; ++i) a3_particles_update(w, 1.0f / 60.0f);
    A3_CHECK_EQ_INT(a3_particles_count(w), 0);
    /* burst-only explosion */
    a3_particles_preset(em, A3_PARTICLES_EXPLOSION);
    em->lifetime_random = 0;
    a3_particles_update(w, 1.0f / 60.0f);
    A3_CHECK_EQ_INT(a3_particles_count(w), 250);
    /* gravity pulls sparks down */
    a3_particles_preset(em, A3_PARTICLES_SPARKS);
    em->emitting = 1;
    a3_particles_restart(w, e);
    for (int i = 0; i < 20; ++i) a3_particles_update(w, 1.0f / 60.0f);
    const A3ParticleInstance *inst = 0;
    u32 count = 0;
    A3ParticleBatch batches[8];
    u32 nb = a3_particles_collect(w, a3_v3(0, 0, 10), batches, 8, &inst, &count);
    A3_CHECK(nb == 1 && count > 0 && batches[0].count == count);
    /* removing the component frees its pool; destroying the world frees everything */
    a3_component_remove(w, e, A3_T_PARTICLE_EMITTER);
    a3_particles_update(w, 1.0f / 60.0f);
    A3_CHECK_EQ_INT(a3_particles_count(w), 0);
    a3_world_destroy(w);
}

A3_TEST(particles_alpha_sorting) {
    particles_setup();
    A3World *w = a3_world_create("fx2");
    A3Entity e = a3_entity_create(w, "Smoke");
    a3_component_add(w, e, A3_T_TRANSFORM);
    A3CParticleEmitter *em = (A3CParticleEmitter *)a3_component_add(w, e, A3_T_PARTICLE_EMITTER);
    a3_particles_preset(em, A3_PARTICLES_DUST);
    em->rate = 0;
    em->burst = 200;
    a3_transform_system_update(w);
    a3_particles_update(w, 0.5f);
    const A3ParticleInstance *inst = 0;
    u32 count = 0;
    A3ParticleBatch batches[4];
    A3Vec3 cam = a3_v3(0, 1, 12);
    u32 nb = a3_particles_collect(w, cam, batches, 4, &inst, &count);
    A3_CHECK(nb == 1 && count == 200 && batches[0].blend == A3_PARTICLE_ALPHA);
    b32 sorted = 1;
    for (u32 i = 1; i < count; ++i)
        if (a3_v3_dist(inst[i].pos, cam) > a3_v3_dist(inst[i - 1].pos, cam) + 0.01f) sorted = 0;
    A3_CHECK_MSG(sorted, "alpha particles must be drawn back to front");
    /* the world cleanup callback releases pools */
    a3_world_destroy(w);
    A3World *w2 = a3_world_create("fx3");
    A3_CHECK_EQ_INT(a3_particles_count(w2), 0);
    a3_world_destroy(w2);
}
