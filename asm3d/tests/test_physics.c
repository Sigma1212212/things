/*
 * ASM3D - test_physics.c : rigid body simulation + character controller
 */
#include "a3_test.h"
#include "../engine/physics/a3_physics.h"
#include "../engine/physics/a3_character.h"
#include "../engine/scene/a3_components.h"
#include "../engine/resource/a3_assets.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_hash.h"

static void setup(void) {
    a3_register_core_components();
    a3_physics_register();
    a3_assets_init("");
}

static A3Entity make_box(A3World *w, const char *name, A3Vec3 pos, A3Vec3 size, b32 dynamic) {
    A3Entity e = a3_entity_create(w, name);
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    t->position = pos;
    A3CCollider *c = (A3CCollider *)a3_component_add(w, e, A3_T_COLLIDER);
    c->shape = A3_SHAPE_BOX;
    c->size = size;
    if (dynamic) ((A3CRigidBody *)a3_component_add(w, e, A3_T_RIGIDBODY))->mass = size.x * size.y * size.z * 100.0f;
    return e;
}

static A3Entity make_sphere(A3World *w, A3Vec3 pos, f32 r) {
    A3Entity e = a3_entity_create(w, "Ball");
    ((A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM))->position = pos;
    A3CCollider *c = (A3CCollider *)a3_component_add(w, e, A3_T_COLLIDER);
    c->shape = A3_SHAPE_SPHERE;
    c->radius = r;
    a3_component_add(w, e, A3_T_RIGIDBODY);
    return e;
}

static void run(A3World *w, int steps) {
    for (int i = 0; i < steps; ++i) { a3_physics_step(w, 1.0f / 60.0f); a3_transform_system_update(w); }
}

A3_TEST(physics_sphere_rests_on_ground) {
    setup();
    A3World *w = a3_world_create("t");
    make_box(w, "Ground", a3_v3(0, -0.5f, 0), a3_v3(20, 1, 20), 0);
    A3Entity ball = make_sphere(w, a3_v3(0, 3, 0), 0.5f);
    run(w, 240);
    A3Vec3 p = a3_transform(w, ball)->position;
    A3_CHECK_NEAR(p.y, 0.5, 0.03);
    A3_CHECK_NEAR(p.x, 0.0, 0.01);
    const A3CRigidBody *rb = (const A3CRigidBody *)a3_component_get(w, ball, A3_T_RIGIDBODY);
    A3_CHECK(a3_v3_len(rb->velocity) < 0.1f);
    A3_CHECK(rb->sleeping); /* came to rest and fell asleep */
    a3_physics_release(w);
    a3_world_destroy(w);
}

A3_TEST(physics_box_stack_stable) {
    setup();
    A3World *w = a3_world_create("t");
    make_box(w, "Ground", a3_v3(0, -0.5f, 0), a3_v3(20, 1, 20), 0);
    A3Entity boxes[6];
    for (int i = 0; i < 6; ++i) boxes[i] = make_box(w, "Crate", a3_v3(0.02f * (i & 1), 0.5f + i * 1.0f, 0), a3_v3(1, 1, 1), 1);
    run(w, 360);
    for (int i = 0; i < 6; ++i) {
        A3Vec3 p = a3_transform(w, boxes[i])->position;
        A3_CHECK_MSG(a3_absf(p.y - (0.5f + i)) < 0.08f, "box %d height %.3f (expected %.1f)", i, p.y, 0.5f + i);
        A3_CHECK_MSG(a3_absf(p.x) < 0.1f && a3_absf(p.z) < 0.1f, "box %d drifted to (%.3f, %.3f)", i, p.x, p.z);
    }
    const A3PhysicsStats *st = a3_physics_stats(w);
    A3_CHECK(st->contacts > 0 || st->sleeping_bodies == 6);
    a3_physics_release(w);
    a3_world_destroy(w);
}

A3_TEST(physics_bounce_and_friction) {
    setup();
    A3World *w = a3_world_create("t");
    make_box(w, "Ground", a3_v3(0, -0.5f, 0), a3_v3(40, 1, 40), 0);
    A3Entity ball = make_sphere(w, a3_v3(0, 4, 0), 0.5f);
    ((A3CRigidBody *)a3_component_get(w, ball, A3_T_RIGIDBODY))->restitution = 0.8f;
    f32 max_after_bounce = 0;
    b32 touched = 0;
    for (int i = 0; i < 180; ++i) {
        run(w, 1);
        f32 y = a3_transform(w, ball)->position.y;
        if (y < 0.55f) touched = 1;
        if (touched) max_after_bounce = a3_maxf(max_after_bounce, y);
    }
    A3_CHECK(touched);
    A3_CHECK_MSG(max_after_bounce > 2.0f, "bounce height %.2f", max_after_bounce); /* ~0.64 * 3.5 m + radius */
    /* a sliding box slows down because of friction */
    A3Entity slider = make_box(w, "Slider", a3_v3(5, 0.5f, 0), a3_v3(1, 1, 1), 1);
    ((A3CRigidBody *)a3_component_get(w, slider, A3_T_RIGIDBODY))->velocity = a3_v3(6, 0, 0);
    run(w, 120);
    const A3CRigidBody *rb = (const A3CRigidBody *)a3_component_get(w, slider, A3_T_RIGIDBODY);
    A3_CHECK(a3_absf(rb->velocity.x) < 0.5f);
    A3_CHECK(a3_transform(w, slider)->position.x > 6.0f);
    a3_physics_release(w);
    a3_world_destroy(w);
}

A3_TEST(physics_raycast_and_triggers) {
    setup();
    A3World *w = a3_world_create("t");
    A3Entity near_box = make_box(w, "Near", a3_v3(0, 0, -5), a3_v3(1, 1, 1), 0);
    make_box(w, "Far", a3_v3(0, 0, -10), a3_v3(1, 1, 1), 0);
    A3RaycastHit hit;
    A3_CHECK(a3_physics_raycast(w, a3_v3(0, 0, 0), a3_v3(0, 0, -1), 100, 0xFFFFFFFFu, &hit));
    A3_CHECK(a3_entity_eq(hit.entity, near_box));
    A3_CHECK_NEAR(hit.distance, 4.5, 1e-4);
    A3_CHECK(a3_v3_nearly_equal(hit.normal, a3_v3(0, 0, 1), 1e-4f));
    A3_CHECK(!a3_physics_raycast(w, a3_v3(0, 0, 0), a3_v3(0, 0, 1), 100, 0xFFFFFFFFu, &hit));
    A3_CHECK(!a3_physics_raycast(w, a3_v3(0, 0, 0), a3_v3(0, 0, -1), 3, 0xFFFFFFFFu, &hit)); /* too short */
    A3_CHECK(a3_physics_raycast_ignore(w, a3_v3(0, 0, 0), a3_v3(0, 0, -1), 100, 0xFFFFFFFFu, near_box, &hit) && hit.distance > 9);
    /* trigger volume: ball falls through it and generates enter + exit */
    A3Entity zone = make_box(w, "Zone", a3_v3(3, 2, 0), a3_v3(2, 1, 2), 0);
    ((A3CCollider *)a3_component_get(w, zone, A3_T_COLLIDER))->is_trigger = 1;
    make_sphere(w, a3_v3(3, 5, 0), 0.3f);
    int enters = 0, exits = 0;
    for (int i = 0; i < 90; ++i) {
        run(w, 1);
        const A3ContactEvent *ev;
        u32 n = a3_physics_events(w, &ev);
        for (u32 k = 0; k < n; ++k) {
            if (ev[k].type == A3_TRIGGER_ENTER && a3_entity_eq(ev[k].a, zone)) enters++;
            if (ev[k].type == A3_TRIGGER_EXIT) exits++;
        }
    }
    A3_CHECK_EQ_INT(enters, 1);
    A3_CHECK_EQ_INT(exits, 1);
    A3Entity found[4];
    A3_CHECK_EQ_INT(a3_physics_overlap_sphere(w, a3_v3(0, 0, -5), 0.3f, 0xFFFFFFFFu, found, 4), 1);
    a3_physics_release(w);
    a3_world_destroy(w);
}

A3_TEST(physics_mesh_collider_and_determinism) {
    setup();
    f32 results[2][3];
    for (int run_i = 0; run_i < 2; ++run_i) {
        A3World *w = a3_world_create("t");
        /* ground from a (static) plane mesh */
        A3Entity g = a3_entity_create(w, "Terrain");
        A3CTransform *gt = (A3CTransform *)a3_component_add(w, g, A3_T_TRANSFORM);
        gt->scale = a3_v3(30, 1, 30);
        gt->rotation = a3_quat_axis_angle(a3_v3(0, 0, 1), 10 * A3_DEG2RAD); /* gentle slope */
        ((A3CMeshRenderer *)a3_component_add(w, g, A3_T_MESH_RENDERER))->primitive = A3_PRIM_PLANE;
        ((A3CCollider *)a3_component_add(w, g, A3_T_COLLIDER))->shape = A3_SHAPE_MESH;
        A3Entity b = make_box(w, "Crate", a3_v3(0, 3, 0), a3_v3(1, 1, 1), 1);
        A3Entity s = make_sphere(w, a3_v3(2, 3, 1), 0.4f);
        run(w, 180);
        A3Vec3 pb = a3_transform(w, b)->position, ps = a3_transform(w, s)->position;
        /* both resting on/above the slope, not fallen through */
        f32 ground_b = a3_tanf(10 * A3_DEG2RAD) * pb.x; /* plane rotated +10 deg about z rises towards +x */
        A3_CHECK_MSG(pb.y > ground_b + 0.3f && pb.y < ground_b + 0.9f, "crate y %.3f ground %.3f", pb.y, ground_b);
        A3_CHECK_MSG(ps.y > -3.0f, "sphere fell through the mesh (y %.3f)", ps.y);
        results[run_i][0] = pb.x; results[run_i][1] = pb.y; results[run_i][2] = ps.x;
        a3_physics_release(w);
        a3_world_destroy(w);
    }
    /* identical input -> identical output, bit for bit */
    A3_CHECK(a3_memcmp(results[0], results[1], sizeof(results[0])) == 0);
}

A3_TEST(physics_character_controller) {
    setup();
    A3World *w = a3_world_create("t");
    make_box(w, "Ground", a3_v3(0, -0.5f, 0), a3_v3(40, 1, 40), 0);
    make_box(w, "Step", a3_v3(0, 0.15f, -4), a3_v3(4, 0.3f, 2), 0);      /* 30 cm step: walkable */
    make_box(w, "Wall", a3_v3(0, 1.0f, -10), a3_v3(4, 2.0f, 1), 0);      /* 2 m wall: blocks */
    A3Entity p = a3_entity_create(w, "Player");
    ((A3CTransform *)a3_component_add(w, p, A3_T_TRANSFORM))->position = a3_v3(0, 0.05f, 0);
    A3CCharacterController *cc = (A3CCharacterController *)a3_component_add(w, p, A3_T_CHARACTER);
    A3_CHECK(a3_entity_child_count(w, p) == 1); /* camera created automatically */
    const f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) a3_character_move(w, p, cc, a3_v2(0, 0), 0, 0, dt);
    A3_CHECK(cc->grounded);
    A3_CHECK_NEAR(a3_transform_world_position(w, p).y, 0.0, 0.02);
    /* walk forward (-Z): up the step, then into the wall */
    f32 max_y = 0;
    for (int i = 0; i < 180; ++i) {
        a3_character_move(w, p, cc, a3_v2(0, 1), 0, 0, dt);
        max_y = a3_maxf(max_y, a3_transform_world_position(w, p).y);
    }
    A3Vec3 pos = a3_transform_world_position(w, p);
    A3_CHECK_MSG(max_y > 0.25f, "did not climb the step (max y %.3f)", max_y);
    A3_CHECK_MSG(pos.z > -9.5f + cc->radius - 0.05f && pos.z < -8.0f, "wall did not stop the player (z %.3f)", pos.z);
    A3_CHECK(cc->grounded);
    /* jump: apex close to jump_height */
    f32 start_y = pos.y, apex = start_y;
    a3_character_move(w, p, cc, a3_v2(0, 0), 1, 0, dt);
    for (int i = 0; i < 90; ++i) { a3_character_move(w, p, cc, a3_v2(0, 0), 0, 0, dt); apex = a3_maxf(apex, a3_transform_world_position(w, p).y); }
    A3_CHECK_MSG(a3_absf((apex - start_y) - cc->jump_height) < 0.12f, "jump apex %.3f (expected %.2f)", apex - start_y, cc->jump_height);
    A3_CHECK(cc->grounded); /* landed again */
    /* look: yaw 90 deg left turns forward toward -X */
    cc->yaw = A3_HALF_PI;
    a3_character_look(w, p, cc, a3_v2(0, 0));
    A3Vec3 before = a3_transform_world_position(w, p);
    for (int i = 0; i < 30; ++i) a3_character_move(w, p, cc, a3_v2(0, 1), 0, 0, dt);
    A3Vec3 after = a3_transform_world_position(w, p);
    A3_CHECK(after.x < before.x - 0.5f);
    a3_physics_release(w);
    a3_world_destroy(w);
}

/* Cross-target determinism: the desktop build runs the assembly kernels, the
 * WebAssembly build runs the C reference; both must end in exactly the same
 * state. The golden hash was recorded from the native build. */
#ifndef A3_PHYSICS_GOLDEN
#define A3_PHYSICS_GOLDEN 0xa5238c68caaa26c5ull
#endif
A3_TEST(physics_cross_target_golden) {
    setup();
    A3World *w = a3_world_create("golden");
    make_box(w, "Ground", a3_v3(0, -0.5f, 0), a3_v3(30, 1, 30), 0);
    for (int i = 0; i < 4; ++i) make_box(w, "Crate", a3_v3(0.1f * i, 0.5f + i * 1.01f, 0.05f * i), a3_v3(1, 1, 1), 1);
    for (int i = 0; i < 6; ++i) {
        A3Entity s = make_sphere(w, a3_v3(-3.0f + i * 1.1f, 2.0f + (f32)i * 0.7f, 1.5f), 0.3f + 0.05f * i);
        ((A3CRigidBody *)a3_component_get(w, s, A3_T_RIGIDBODY))->velocity = a3_v3(1.5f - i * 0.5f, 0, -0.7f);
    }
    A3Entity cap = a3_entity_create(w, "Capsule");
    A3CTransform *ct = (A3CTransform *)a3_component_add(w, cap, A3_T_TRANSFORM);
    ct->position = a3_v3(2, 3, -1);
    ct->rotation = a3_quat_euler(0.3f, 0.2f, 1.1f);
    A3CCollider *cc = (A3CCollider *)a3_component_add(w, cap, A3_T_COLLIDER);
    cc->shape = A3_SHAPE_CAPSULE;
    a3_component_add(w, cap, A3_T_RIGIDBODY);
    run(w, 150);
    u64 h = 0;
    u32 n = 0;
    const A3Entity *ents = 0;
    A3CTransform *ts = (A3CTransform *)a3_component_array(w, A3_T_TRANSFORM, &n, &ents);
    for (u32 i = 0; i < n; ++i) {
        h = a3_hash_combine(h, a3_hash64(&ts[i].position, sizeof(A3Vec3), 0));
        h = a3_hash_combine(h, a3_hash64(&ts[i].rotation, sizeof(A3Quat), 0));
    }
    char hex[17];
    a3_hash_to_hex(h, hex);
    A3_CHECK_MSG(h == A3_PHYSICS_GOLDEN, "physics state hash %s differs from golden", hex);
    a3_physics_release(w);
    a3_world_destroy(w);
}
