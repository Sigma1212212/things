/*
 * ASM3D - test_script_engine.c : scripts on objects (fields, game API, events, reload, errors)
 */
#include "a3_test.h"
#include "../engine/script/a3_script_engine.h"
#include "../engine/physics/a3_physics.h"
#include "../engine/particles/a3_particles.h"
#include "../engine/anim/a3_anim.h"
#include "../engine/scene/a3_components.h"
#include "../engine/resource/a3_assets.h"
#include "../engine/core/a3_string.h"

static void se_setup(void) {
    a3_register_core_components();
    a3_physics_register();
    a3_particles_register();
    a3_anim_register();
    a3_scripts_register();
    a3_assets_init("");
    a3_scripts_clear_errors();
}

static A3Entity scripted(A3World *w, const char *name, const char *path) {
    A3Entity e = a3_entity_create(w, name);
    a3_component_add(w, e, A3_T_TRANSFORM);
    A3CScript *s = (A3CScript *)a3_component_add(w, e, A3_T_SCRIPT);
    a3_strcpy(s->script.path, sizeof(s->script.path), path);
    return e;
}

static void frame(A3World *w, f32 dt, f64 t) {
    A3ScriptFrame f;
    a3_zero_struct(&f);
    f.dt = dt;
    f.time = t;
    a3_scripts_update(w, &f);
    a3_world_flush(w);
    a3_transform_system_update(w);
}

A3_TEST(script_engine_objects_and_hud) {
    se_setup();
    i64 live = a3s_live_objects();
    a3_scripts_set_source("Scripts/mover.a3script",
        "let speed = 2\n"
        "let ticks = 0\n"
        "fn on_start() { self.name = \"Mover\" }\n"
        "fn on_update(dt) {\n"
        "    ticks += 1\n"
        "    self.position += vec3(speed * dt, 0, 0)\n"
        "    self.rotation = vec3(0, 90, 0)\n"
        "    self.Transform.scale.y = 3\n"
        "    hud_text(\"Ticks: \" + ticks, 640, 20, 2, vec3(1, 1, 0), 1, \"center\")\n"
        "    hud_bar(10, 10, 200, 20, 0.5, vec3(1, 0, 0))\n"
        "    if ticks == 2 {\n"
        "        let c = clone(self, vec3(0, 5, 0))\n"
        "        c.name = \"Copy\"\n"
        "        remove_component(c, \"Script\")\n"
        "        let e = spawn(\"Empty\", vec3(1, 2, 3))\n"
        "        add_component(e, \"Light\")\n"
        "        e.Light.intensity = 7\n"
        "        e.Light.type = \"Point\"\n"
        "    }\n"
        "    if ticks == 3 { destroy(find(\"Empty\")) }\n"
        "}\n");
    A3World *w = a3_world_create("s");
    A3Entity e = scripted(w, "Thing", "Scripts/mover.a3script");
    frame(w, 0.5f, 0);
    A3_CHECK_STR(a3_entity_name(w, e), "Mover");
    A3CTransform *t = a3_transform(w, e);
    A3_CHECK_NEAR(t->position.x, 1, 1e-5);
    A3_CHECK_NEAR(t->scale.y, 3, 1e-6);
    A3Vec3 eu = a3_quat_to_euler(t->rotation);
    A3_CHECK_NEAR(eu.y * 57.29578f, 90, 0.01);
    const A3HudCmd *hud;
    A3_CHECK_EQ_INT(a3_scripts_hud(w, &hud), 3);
    A3_CHECK_STR(hud[0].text, "Ticks: 1");
    A3_CHECK(hud[0].kind == A3_HUD_TEXT && hud[0].h == 1 && hud[0].w == 2);
    A3_CHECK(hud[2].kind == A3_HUD_RECT && hud[2].w == 98);  /* bar fill: (200 - 4) * 0.5 */
    frame(w, 0.5f, 0.5);
    A3_CHECK_EQ_INT(a3_scripts_hud(w, &hud), 3);        /* cleared every frame */
    A3Entity copy = a3_entity_find_by_name(w, "Copy");
    A3_CHECK(!a3_entity_is_null(copy));
    A3_CHECK(!a3_component_has(w, copy, A3_T_SCRIPT));
    A3_CHECK_NEAR(a3_transform(w, copy)->position.y, 5, 1e-5);
    A3Entity empty = a3_entity_find_by_name(w, "Empty");
    A3_CHECK(!a3_entity_is_null(empty));
    A3CLight *l = (A3CLight *)a3_component_get(w, empty, A3_T_LIGHT);
    A3_CHECK(l && l->intensity == 7 && l->type == A3_LIGHT_POINT);
    frame(w, 0.5f, 1.0);
    A3_CHECK(a3_entity_is_null(a3_entity_find_by_name(w, "Empty")));
    A3_CHECK_NEAR(t->position.x, 3, 1e-5);
    A3_CHECK_EQ_INT(a3_scripts_instance_count(w), 1);
    A3_CHECK_EQ_INT(a3_scripts_error_count(), 0);
    a3_scripts_stop(w);
    A3_CHECK_EQ_INT(a3_scripts_instance_count(w), 0);
    a3_world_destroy(w);
    a3_scripts_set_source("Scripts/mover.a3script", 0);
    A3_CHECK_EQ_INT(a3s_live_objects(), live);
}

A3_TEST(script_engine_messages_reload_errors) {
    se_setup();
    i64 live = a3s_live_objects();
    a3_scripts_set_source("m.a3script", "let score = 0\nfn add_score(n) { score += n }\n");
    a3_scripts_set_source("orb.a3script",
        "fn on_update(dt) {\n"
        "    send(find(\"Manager\"), \"add_score\", 5)\n"
        "    let m = find(\"Manager\")\n"
        "    m.score += 1\n"
        "    self.name = \"score\" + m.score\n"
        "}\n");
    a3_scripts_set_source("bad.a3script", "fn on_update(dt) {\n  let x = nil\n  x.y = 1\n}\n");
    a3_scripts_set_source("broken.a3script", "fn on_update(dt) {\n  prin(dt)\n}\n");
    A3World *w = a3_world_create("s");
    scripted(w, "Manager", "m.a3script");
    A3Entity orb = scripted(w, "Orb", "orb.a3script");
    A3Entity bad = scripted(w, "Bad", "bad.a3script");
    scripted(w, "Broken", "broken.a3script");
    frame(w, 0.1f, 0);
    A3_CHECK_STR(a3_entity_name(w, orb), "score6");
    /* one runtime error and one compile error, each reported once */
    for (int i = 0; i < 3; ++i) frame(w, 0.1f, 0);
    A3_CHECK_EQ_INT(a3_scripts_error_count(), 2);
    const A3ScriptErrorInfo *e0 = a3_scripts_error(0), *e1 = a3_scripts_error(1);
    b32 bad_first = a3_streq(e0->path, "bad.a3script");
    const A3ScriptErrorInfo *rt = bad_first ? e0 : e1, *ce = bad_first ? e1 : e0;
    A3_CHECK_STR(rt->path, "bad.a3script");
    A3_CHECK_STR(rt->object, "Bad");
    A3_CHECK(rt->error.line == 3);
    A3_CHECK_STR(rt->error.message, "tried to set '.y' of nil");
    A3_CHECK_STR(ce->path, "broken.a3script");
    A3_CHECK(ce->error.line == 2);
    A3_CHECK_STR(ce->error.hint, "Did you mean 'print'?");
    A3_CHECK_STR(a3_entity_name(w, orb), "score24");
    A3_CHECK_STR(a3_entity_name(w, bad), "Bad");
    /* hot reload keeps top-level values */
    a3_scripts_set_source("m.a3script", "let score = 0\nlet bonus = 100\nfn add_score(n) { score += n + bonus }\n");
    frame(w, 0.1f, 0);
    A3_CHECK_STR(a3_entity_name(w, orb), "score130");
    /* fixing the file restarts the object that failed */
    a3_scripts_set_source("bad.a3script", "fn on_update(dt) { self.name = \"Fixed\" }\n");
    frame(w, 0.1f, 0);
    A3_CHECK_STR(a3_entity_name(w, bad), "Fixed");
    /* a missing file is one clear error */
    A3Entity lost = scripted(w, "Lost", "Scripts/does_not_exist.a3script");
    frame(w, 0.1f, 0);
    frame(w, 0.1f, 0);
    A3_CHECK_EQ_INT(a3_scripts_error_count(), 3);
    A3_CHECK_STR(a3_scripts_error(2)->error.message, "the script file was not found");
    /* removing the component stops the script */
    a3_component_remove(w, lost, A3_T_SCRIPT);
    a3_component_remove(w, orb, A3_T_SCRIPT);
    frame(w, 0.1f, 0);
    A3_CHECK_EQ_INT(a3_scripts_instance_count(w), 2);          /* Manager and Bad (Broken never compiled) */
    a3_world_destroy(w);                                  /* releases instances too */
    const char *paths[] = { "m.a3script", "orb.a3script", "bad.a3script", "broken.a3script" };
    for (u32 i = 0; i < 4; ++i) a3_scripts_set_source(paths[i], 0);
    a3_scripts_clear_errors();
    A3_CHECK_EQ_INT(a3s_live_objects(), live);
}

/* The character's sensor capsule must not block rays (it only exists for triggers). */
static b32 a3_raycast_ignores_player_check(A3World *w) {
    A3RaycastHit hit;
    b32 h = a3_physics_raycast(w, a3_v3(8, 5, 0), a3_v3(0, -1, 0), 20, 0xFFFFFFFFu, &hit);
    return h && a3_streq(a3_entity_name(w, hit.entity), "Ground");
}

A3_TEST(script_engine_physics_events) {
    se_setup();
    a3_scripts_set_source("zone.a3script",
        "let hits = 0\n"
        "fn on_trigger_enter(other) {\n"
        "    hits += 1\n"
        "    other.name = \"Caught\"\n"
        "    add_impulse(other, vec3(0, 0.5, 0))\n"
        "}\n");
    a3_scripts_set_source("ball.a3script",
        "let bumps = 0\n"
        "fn on_collision(other) { bumps += 1 }\n"
        "fn on_fixed_update(dt) { if bumps > 0 and self.velocity.y > -100 { self.Collider.radius = 0.5 } }\n"
        "fn on_start() {\n"
        "    let hit = raycast_hit(self.position, vec3(0, -1, 0), 50)\n"
        "    assert(hit != nil, \"ray should hit the ground\")\n"
        "    self.name = hit[0].name + \" \" + round(hit[3], 2)\n"
        "}\n");
    A3World *w = a3_world_create("p");
    A3Entity ground = a3_entity_create(w, "Ground");
    ((A3CTransform *)a3_component_add(w, ground, A3_T_TRANSFORM))->position = a3_v3(0, -0.5f, 0);
    A3CCollider *gc = (A3CCollider *)a3_component_add(w, ground, A3_T_COLLIDER);
    gc->shape = A3_SHAPE_BOX; gc->size = a3_v3(20, 1, 20);
    A3Entity zone = scripted(w, "Zone", "zone.a3script");
    a3_transform(w, zone)->position = a3_v3(0, 2, 0);
    A3CCollider *zc = (A3CCollider *)a3_component_add(w, zone, A3_T_COLLIDER);
    zc->shape = A3_SHAPE_BOX; zc->size = a3_v3(2, 1, 2); zc->is_trigger = 1;
    A3Entity ball = scripted(w, "Ball", "ball.a3script");
    a3_transform(w, ball)->position = a3_v3(0, 4, 0);
    A3CCollider *bc = (A3CCollider *)a3_component_add(w, ball, A3_T_COLLIDER);
    bc->shape = A3_SHAPE_SPHERE; bc->radius = 0.4f;
    a3_component_add(w, ball, A3_T_RIGIDBODY);
    /* a player character (no Collider) standing in a pickup zone */
    a3_scripts_set_source("pickup.a3script", "fn on_trigger_enter(other) { other.name = \"Picked\" }\n");
    A3Entity pickup = scripted(w, "Pickup", "pickup.a3script");
    a3_transform(w, pickup)->position = a3_v3(8, 1, 0);
    A3CCollider *pc = (A3CCollider *)a3_component_add(w, pickup, A3_T_COLLIDER);
    pc->shape = A3_SHAPE_SPHERE; pc->radius = 0.6f; pc->is_trigger = 1;
    A3Entity player = a3_entity_create(w, "Player");
    ((A3CTransform *)a3_component_add(w, player, A3_T_TRANSFORM))->position = a3_v3(8, 0, 0);
    a3_component_add(w, player, A3_T_CHARACTER);
    a3_transform_system_update(w);
    frame(w, 1.0f / 60.0f, 0);                              /* on_start */
    A3_CHECK_EQ_INT(a3_scripts_error_count(), 0);
    b32 caught = 0;
    for (int i = 0; i < 180; ++i) {
        a3_physics_step(w, 1.0f / 60.0f);
        a3_scripts_fixed(w, 1.0f / 60.0f);
        a3_transform_system_update(w);
        if (a3_streq(a3_entity_name(w, ball), "Caught")) caught = 1;
    }
    A3_CHECK(caught);
    A3_CHECK_STR(a3_entity_name(w, player), "Picked");     /* characters are seen by triggers */
    A3_CHECK(a3_raycast_ignores_player_check(w));
    A3_CHECK_EQ_INT(a3_scripts_error_count(), 0);
    A3CCollider *after = (A3CCollider *)a3_component_get(w, ball, A3_T_COLLIDER);
    A3_CHECK(after->radius == 0.5f);                        /* on_collision + on_fixed_update ran */
    a3_physics_release(w);
    a3_world_destroy(w);
    a3_scripts_set_source("zone.a3script", 0);
    a3_scripts_set_source("ball.a3script", 0);
    a3_scripts_set_source("pickup.a3script", 0);
}
