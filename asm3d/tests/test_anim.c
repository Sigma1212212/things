/*
 * ASM3D - test_anim.c : keyframe sampling, clip JSON, Animator, Motion
 */
#include "a3_test.h"
#include "../engine/anim/a3_anim.h"
#include "../engine/scene/a3_components.h"
#include "../engine/core/a3_string.h"

static void anim_setup(void) {
    a3_register_core_components();
    a3_anim_register();
}

A3_TEST(anim_track_sampling) {
    A3AnimClip c;
    a3_anim_clip_init(&c, "test");
    i32 ti = a3_anim_track_add(&c, "", "Transform", "position");
    A3_CHECK(ti == 0);
    A3AnimTrack *t = &c.tracks[ti];
    f32 v0[4] = { 0, 0, 0, 0 }, v1[4] = { 10, 0, 0, 0 }, v2[4] = { 10, 10, 0, 0 };
    a3_anim_key_set(t, 2.0f, v2, A3_INTERP_LINEAR);   /* out of order on purpose */
    a3_anim_key_set(t, 0.0f, v0, A3_INTERP_LINEAR);
    a3_anim_key_set(t, 1.0f, v1, A3_INTERP_LINEAR);
    A3_CHECK_EQ_INT(t->keys.count, 3);
    A3_CHECK(t->keys.data[0].time == 0 && t->keys.data[1].time == 1 && t->keys.data[2].time == 2);
    f32 o[4];
    a3_anim_track_sample(t, 0.5f, 0, o);
    A3_CHECK_NEAR(o[0], 5, 1e-5);
    a3_anim_track_sample(t, 1.5f, 0, o);
    A3_CHECK_NEAR(o[0], 10, 1e-5); A3_CHECK_NEAR(o[1], 5, 1e-5);
    a3_anim_track_sample(t, -1.0f, 0, o);
    A3_CHECK(o[0] == 0);
    a3_anim_track_sample(t, 9.0f, 0, o);
    A3_CHECK(o[1] == 10);
    /* replacing a key at the same time keeps the count */
    f32 v3[4] = { 3, 3, 3, 0 };
    a3_anim_key_set(t, 1.0005f, v3, A3_INTERP_STEP);
    A3_CHECK_EQ_INT(t->keys.count, 3);
    a3_anim_track_sample(t, 1.9f, 0, o);
    A3_CHECK(o[0] == 3);                                /* step holds the value */
    /* smooth: passes through the keys, eases at the ends */
    t->keys.data[0].interp = t->keys.data[1].interp = A3_INTERP_SMOOTH;
    a3_anim_track_sample(t, 1.0f, 0, o);
    A3_CHECK_NEAR(o[0], 3, 1e-5);
    a3_anim_track_sample(t, 0.05f, 0, o);
    A3_CHECK(o[0] > 0 && o[0] < 0.15f);                 /* flat start: slow */
    /* rotations take the short way and stay normalized */
    i32 ri = a3_anim_track_add(&c, "", "Transform", "rotation");
    A3Quat q0 = a3_quat_identity(), q1 = a3_quat_axis_angle(a3_v3(0, 1, 0), A3_PI * 0.5f);
    a3_anim_key_set(&c.tracks[ri], 0, &q0.x, A3_INTERP_LINEAR);
    a3_anim_key_set(&c.tracks[ri], 1, &q1.x, A3_INTERP_LINEAR);
    a3_anim_track_sample(&c.tracks[ri], 0.5f, 1, o);
    A3Quat mid = a3_quat_axis_angle(a3_v3(0, 1, 0), A3_PI * 0.25f);
    A3_CHECK_NEAR(o[1], mid.y, 1e-4); A3_CHECK_NEAR(o[3], mid.w, 1e-4);
    A3_CHECK_NEAR(a3_anim_clip_length(&c), 2.0, 1e-6);
    a3_anim_clip_free(&c);
}

A3_TEST(anim_json_and_apply) {
    anim_setup();
    A3AnimClip c, d;
    a3_anim_clip_init(&c, "door");
    c.duration = 1.0f;
    i32 ti = a3_anim_track_add(&c, "Hinge", "Transform", "position");
    f32 a[4] = { 0, 0, 0, 0 }, b[4] = { 0, 2, 0, 0 };
    a3_anim_key_set(&c.tracks[ti], 0, a, A3_INTERP_LINEAR);
    a3_anim_key_set(&c.tracks[ti], 1, b, A3_INTERP_LINEAR);
    i32 li = a3_anim_track_add(&c, "", "Light", "intensity");
    f32 i0[4] = { 0 }, i1[4] = { 4 };
    a3_anim_key_set(&c.tracks[li], 0, i0, A3_INTERP_LINEAR);
    a3_anim_key_set(&c.tracks[li], 1, i1, A3_INTERP_LINEAR);
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    a3_anim_save_json(&c, &sb);
    char err[128];
    A3_CHECK_MSG(a3_anim_load_json(&d, sb.data, sb.len, err, sizeof(err)), "load: %s", err);
    A3_CHECK_EQ_INT(d.track_count, 2);
    A3_CHECK_STR(d.tracks[0].target, "Hinge");
    A3_CHECK_EQ_INT(d.tracks[0].keys.count, 2);
    /* apply to an entity with a named child */
    A3World *w = a3_world_create("anim");
    A3Entity e = a3_entity_create(w, "Door");
    a3_component_add(w, e, A3_T_TRANSFORM);
    A3CLight *l = (A3CLight *)a3_component_add(w, e, A3_T_LIGHT);
    A3Entity h = a3_entity_create(w, "Hinge");
    a3_entity_set_parent(w, h, e);
    a3_component_add(w, h, A3_T_TRANSFORM);
    a3_anim_apply(&d, w, e, 0.5f);
    A3_CHECK_NEAR(a3_transform(w, h)->position.y, 1.0, 1e-5);
    l = (A3CLight *)a3_component_get(w, e, A3_T_LIGHT);
    A3_CHECK_NEAR(l->intensity, 2.0, 1e-5);
    f32 rv[4];
    A3_CHECK(a3_anim_read_field(w, e, "Light", "intensity", rv) && a3_absf(rv[0] - 2.0f) < 1e-5f);
    A3_CHECK(!a3_anim_read_field(w, e, "Light", "nope", rv));
    A3_CHECK(!a3_anim_load_json(&c, "{\"format\":\"x\"}", 14, err, sizeof(err)));
    a3_strbuf_free(&sb);
    a3_anim_clip_free(&c);
    a3_anim_clip_free(&d);
    a3_world_destroy(w);
}

A3_TEST(anim_motion_component) {
    anim_setup();
    A3World *w = a3_world_create("motion");
    A3Entity e = a3_entity_create(w, "Coin");
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    t->position = a3_v3(1, 2, 3);
    A3CMotion *m = (A3CMotion *)a3_component_add(w, e, A3_T_MOTION);
    m->spin = a3_v3(0, 90, 0);
    m->bob_height = 0.5f;
    m->bob_speed = 1.0f;
    for (int i = 0; i < 15; ++i) a3_anim_update(w, 1.0f / 60.0f);  /* 0.25 s: quarter bob, 22.5 degrees */
    t = a3_transform(w, e);
    A3_CHECK_NEAR(t->position.y, 2.5, 1e-3);
    A3_CHECK_NEAR(t->position.x, 1.0, 1e-6);
    A3Vec3 fwd = a3_quat_rotate(t->rotation, a3_v3(0, 0, -1));
    f32 ang = a3_atan2f(-fwd.x, -fwd.z) * A3_RAD2DEG;
    A3_CHECK_NEAR(ang, 22.5, 0.05);
    a3_world_destroy(w);
}
