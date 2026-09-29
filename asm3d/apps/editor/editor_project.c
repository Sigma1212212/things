/*
 * ASM3D Editor - projects, templates, scenes, autosave and recovery.
 *
 * Project layout (plain text, Git friendly):
 *   MyGame/
 *     project.a3proj          project settings (JSON)
 *     Assets/Scenes/NAME.a3scene scenes (JSON)
 *     Assets/{Models,Textures,Materials,Shaders,Scripts,Audio,Prefabs}/
 *     .asm3d/recovery/        autosave copies (not committed)
 *     .asm3d/layout.json      editor layout
 *     .gitignore
 */
#include "editor.h"
#include "../../engine/scene/a3_scene_io.h"
#include "../../engine/physics/a3_physics.h"
#include "../../engine/audio/a3_audio.h"
#include "../../engine/particles/a3_particles.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/core/a3_json.h"
#include "../../engine/platform/a3_platform.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/core/a3_hash.h"

const char *const g_ed_templates[] = {
    "Empty 3D Game", "First Person", "Third Person", "Platformer", "Racing",
    "Horror", "Shooter", "Sandbox", "Open World", "Small Game",
};
const char *const g_ed_template_desc[] = {
    "A lit ground plane and a camera. Start from scratch.",
    "Walk, run and jump with the mouse and WASD. Includes stairs and a ramp.",
    "A visible character followed by an orbiting camera.",
    "Jump across floating platforms and collect orbs.",
    "A looping track with a physics-driven car body (vehicle handling arrives with the vehicle module).",
    "Dark corridors, thick fog and a flashlight.",
    "First person arena with targets and cover.",
    "A physics playground full of crates, balls and ramps.",
    "A large landscape with scattered props (terrain streaming arrives with the world module).",
    "A tiny first person scene that builds fast. Great for game jams.",
};
const u32 g_ed_template_count = A3_ARRAY_COUNT(g_ed_templates);

void ed_project_path(A3Editor *ed, const char *rel, char *out, usize cap) { a3_path_join(out, cap, ed->project_dir, rel); }

/* ======================================================================== */
/* Template content                                                         */
/* ======================================================================== */

static A3Entity tp_shape(A3World *w, const char *name, A3Primitive prim, A3Vec3 pos, A3Vec3 scale, A3Vec4 color, b32 collider, b32 dynamic) {
    A3Entity e = a3_entity_create(w, name);
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    t->position = pos;
    t->scale = scale;
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, e, A3_T_MESH_RENDERER);
    mr->primitive = prim;
    mr->base_color = color;
    if (collider) a3_component_add(w, e, A3_T_COLLIDER);
    if (dynamic) {
        A3CRigidBody *rb = (A3CRigidBody *)a3_component_add(w, e, A3_T_RIGIDBODY);
        rb->mass = a3_maxf(scale.x * scale.y * scale.z * 50.0f, 1.0f);
    }
    return e;
}

static A3Entity tp_sun(A3World *w, f32 pitch_deg, f32 yaw_deg, A3Vec4 color, f32 intensity) {
    A3Entity sun = a3_entity_create(w, "Sun");
    A3CTransform *t = (A3CTransform *)a3_component_add(w, sun, A3_T_TRANSFORM);
    t->position = a3_v3(0, 10, 0);
    t->rotation = a3_quat_euler(pitch_deg * A3_DEG2RAD, yaw_deg * A3_DEG2RAD, 0);
    A3CLight *l = (A3CLight *)a3_component_add(w, sun, A3_T_LIGHT);
    l->type = A3_LIGHT_DIRECTIONAL;
    l->color = color;
    l->intensity = intensity;
    l->cast_shadows = 1;
    return sun;
}

static A3Entity tp_light(A3World *w, const char *name, A3LightType type, A3Vec3 pos, A3Vec4 color, f32 intensity, f32 range) {
    A3Entity e = a3_entity_create(w, name);
    ((A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM))->position = pos;
    A3CLight *l = (A3CLight *)a3_component_add(w, e, A3_T_LIGHT);
    l->type = type;
    l->color = color;
    l->intensity = intensity;
    l->range = range;
    return e;
}

static A3Entity tp_sound(A3World *w, const char *name, A3Vec3 pos, i32 builtin, f32 volume, b32 spatial, A3Entity parent) {
    A3Entity e = a3_entity_create(w, name);
    if (a3_entity_valid(w, parent)) a3_entity_set_parent(w, e, parent);
    ((A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM))->position = pos;
    A3CAudioSource *s = (A3CAudioSource *)a3_component_add(w, e, A3_T_AUDIO_SOURCE);
    s->builtin = builtin;
    s->volume = volume;
    s->loop = 1;
    s->spatial = spatial;
    s->max_distance = 60.0f;
    return e;
}

static A3Entity tp_effect(A3World *w, const char *name, A3Vec3 pos, u32 preset) {
    A3Entity e = a3_entity_create(w, name);
    ((A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM))->position = pos;
    A3CParticleEmitter *em = (A3CParticleEmitter *)a3_component_add(w, e, A3_T_PARTICLE_EMITTER);
    a3_particles_preset(em, preset);
    return e;
}

/* A campfire: logs, fire, smoke, warm light and a crackle-free hum of wind. */
static void tp_campfire(A3World *w, A3Vec3 at) {
    const A3Vec4 bark = a3_v4(0.35f, 0.22f, 0.12f, 1);
    for (int i = 0; i < 3; ++i) {
        A3Entity log = tp_shape(w, "Log", A3_PRIM_CYLINDER, a3_v3(at.x, at.y + 0.12f, at.z), a3_v3(0.18f, 0.9f, 0.18f), bark, 0, 0);
        ((A3CTransform *)a3_component_get(w, log, A3_T_TRANSFORM))->rotation = a3_quat_euler(80 * A3_DEG2RAD, (f32)i * 60 * A3_DEG2RAD, 0);
    }
    tp_effect(w, "Fire", a3_v3(at.x, at.y + 0.2f, at.z), A3_PARTICLES_FIRE);
    tp_effect(w, "Smoke", a3_v3(at.x, at.y + 1.0f, at.z), A3_PARTICLES_SMOKE);
    tp_light(w, "Fire Light", A3_LIGHT_POINT, a3_v3(at.x, at.y + 0.9f, at.z), a3_v4(1, 0.55f, 0.2f, 1), 2.5f, 8);
}

static A3Entity tp_camera(A3World *w, A3Vec3 pos, f32 pitch_deg, f32 yaw_deg) {
    A3Entity cam = a3_entity_create(w, "Main Camera");
    A3CTransform *t = (A3CTransform *)a3_component_add(w, cam, A3_T_TRANSFORM);
    t->position = pos;
    t->rotation = a3_quat_euler(pitch_deg * A3_DEG2RAD, yaw_deg * A3_DEG2RAD, 0);
    a3_component_add(w, cam, A3_T_CAMERA);
    return cam;
}

static A3Entity tp_player(A3World *w, A3Vec3 pos, A3CameraMode mode, b32 visible_body) {
    A3Entity p = a3_entity_create(w, "Player");
    ((A3CTransform *)a3_component_add(w, p, A3_T_TRANSFORM))->position = pos;
    /* set the camera mode before on_add so the right camera is created */
    A3ComponentType *ct = a3_component_type(A3_T_CHARACTER);
    A3CCharacterController *def = (A3CCharacterController *)ct->defaults;
    i32 saved = def->camera_mode;
    def->camera_mode = mode;
    A3CCharacterController *cc = (A3CCharacterController *)a3_component_add(w, p, A3_T_CHARACTER);
    def->camera_mode = saved;
    if (visible_body) {
        A3Entity body = a3_entity_create(w, "Body");
        a3_entity_set_parent(w, body, p);
        A3CTransform *bt = (A3CTransform *)a3_component_add(w, body, A3_T_TRANSFORM);
        bt->position = a3_v3(0, cc->height * 0.5f, 0);
        bt->scale = a3_v3(cc->radius * 2, cc->height * 0.5f, cc->radius * 2);
        A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, body, A3_T_MESH_RENDERER);
        mr->primitive = A3_PRIM_CAPSULE;
        mr->base_color = a3_v4(0.2f, 0.55f, 0.95f, 1);
    }
    return p;
}

void ed_build_template_scene(A3World *w, i32 tpl) {
    A3CWorldSettings *ws = a3_world_settings(w);
    const A3Vec4 grass = a3_v4(0.36f, 0.5f, 0.3f, 1), stone = a3_v4(0.6f, 0.6f, 0.62f, 1), wood = a3_v4(0.62f, 0.45f, 0.3f, 1);
    switch (tpl) {
    default:
    case 0: /* Empty */
        tp_sun(w, -50, 35, a3_v4(1, 0.96f, 0.88f, 1), 1.0f);
        tp_shape(w, "Ground", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(40, 1, 40), grass, 1, 0);
        tp_shape(w, "Cube", A3_PRIM_CUBE, a3_v3(0, 0.5f, 0), a3_v3_one(), a3_v4(0.85f, 0.35f, 0.3f, 1), 1, 0);
        tp_camera(w, a3_v3(0, 3, 7), -20, 0);
        break;
    case 1: case 9: { /* First person (and the small game) */
        tp_sun(w, -55, 30, a3_v4(1, 0.95f, 0.85f, 1), 1.0f);
        f32 size = tpl == 9 ? 30.0f : 60.0f;
        tp_shape(w, "Ground", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(size, 1, size), grass, 1, 0);
        for (int i = 0; i < 6; ++i) tp_shape(w, "Stair", A3_PRIM_CUBE, a3_v3(6, 0.15f + i * 0.3f, -4 - i * 0.6f), a3_v3(3, 0.3f + i * 0.6f, 0.6f), stone, 1, 0);
        A3Entity ramp = tp_shape(w, "Ramp", A3_PRIM_CUBE, a3_v3(-6, 0.9f, -6), a3_v3(3, 0.3f, 7), stone, 1, 0);
        ((A3CTransform *)a3_component_get(w, ramp, A3_T_TRANSFORM))->rotation = a3_quat_euler(15 * A3_DEG2RAD, 0, 0);
        for (int i = 0; i < 8; ++i) tp_shape(w, "Crate", A3_PRIM_CUBE, a3_v3(-3.0f + (i % 4) * 1.2f, 0.5f + (i / 4) * 1.0f, -12), a3_v3_one(), wood, 1, 1);
        tp_shape(w, "Wall", A3_PRIM_CUBE, a3_v3(0, 1.5f, -18), a3_v3(20, 3, 1), stone, 1, 0);
        tp_player(w, a3_v3(0, 0, 4), A3_CAM_FIRST_PERSON, 0);
        tp_light(w, "Lamp", A3_LIGHT_POINT, a3_v3(0, 2.5f, -10), a3_v4(1, 0.75f, 0.45f, 1), 1.5f, 10);
        tp_campfire(w, a3_v3(-2.5f, 0, -1));
    } break;
    case 2: /* Third person */
        tp_sun(w, -50, 40, a3_v4(1, 0.96f, 0.88f, 1), 1.0f);
        tp_shape(w, "Ground", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(60, 1, 60), grass, 1, 0);
        for (int i = 0; i < 10; ++i) tp_shape(w, "Pillar", A3_PRIM_CYLINDER, a3_v3(-10.0f + (i % 5) * 5, 1.5f, -6.0f - (i / 5) * 8), a3_v3(1, 3, 1), stone, 1, 0);
        tp_player(w, a3_v3(0, 0, 0), A3_CAM_THIRD_PERSON, 1);
        break;
    case 3: { /* Platformer */
        tp_sun(w, -60, 20, a3_v4(1, 0.95f, 0.9f, 1), 1.1f);
        ws->sky_top = a3_v4(0.35f, 0.6f, 0.95f, 1);
        tp_shape(w, "Start", A3_PRIM_CUBE, a3_v3(0, -0.5f, 0), a3_v3(6, 1, 6), grass, 1, 0);
        A3Rng rng;
        a3_rng_seed(&rng, 7, 1);
        A3Vec3 p = a3_v3(0, 0, 0);
        for (int i = 0; i < 12; ++i) {
            p = a3_v3_add(p, a3_v3(a3_rng_range_f32(&rng, -2.5f, 2.5f), a3_rng_range_f32(&rng, 0.2f, 1.1f), -3.5f));
            tp_shape(w, "Platform", A3_PRIM_CUBE, a3_v3(p.x, p.y - 0.25f, p.z), a3_v3(2.2f, 0.5f, 2.2f), a3_v4(0.8f, 0.55f, 0.3f, 1), 1, 0);
            A3Entity orb = tp_shape(w, "Orb", A3_PRIM_SPHERE, a3_v3(p.x, p.y + 0.8f, p.z), a3_v3s(0.4f), a3_v4(1, 0.85f, 0.2f, 1), 1, 0);
            ((A3CCollider *)a3_component_get(w, orb, A3_T_COLLIDER))->is_trigger = 1;
            ((A3CMeshRenderer *)a3_component_get(w, orb, A3_T_MESH_RENDERER))->emissive = 1.5f;
        }
        A3Entity pl = tp_player(w, a3_v3(0, 0, 1), A3_CAM_THIRD_PERSON, 1);
        ((A3CCharacterController *)a3_component_get(w, pl, A3_T_CHARACTER))->jump_height = 1.8f;
    } break;
    case 4: { /* Racing */
        tp_sun(w, -45, 60, a3_v4(1, 0.93f, 0.82f, 1), 1.0f);
        tp_shape(w, "Ground", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(200, 1, 200), a3_v4(0.4f, 0.5f, 0.3f, 1), 1, 0);
        for (int i = 0; i < 48; ++i) {
            f32 a = A3_TAU * (f32)i / 48.0f, s, c;
            a3_sincosf(a, &s, &c);
            A3Entity seg = tp_shape(w, "Road", A3_PRIM_CUBE, a3_v3(c * 50, 0.02f, s * 35), a3_v3(10, 0.04f, 7.5f), a3_v4(0.22f, 0.22f, 0.24f, 1), 0, 0);
            ((A3CTransform *)a3_component_get(w, seg, A3_T_TRANSFORM))->rotation = a3_quat_axis_angle(a3_v3(0, 1, 0), -a);
        }
        A3Entity car = tp_shape(w, "Car", A3_PRIM_CUBE, a3_v3(50, 0.6f, 0), a3_v3(1.8f, 0.8f, 4), a3_v4(0.9f, 0.15f, 0.15f, 1), 1, 1);
        ((A3CRigidBody *)a3_component_get(w, car, A3_T_RIGIDBODY))->mass = 1200;
        tp_sound(w, "Engine Sound", a3_v3_zero(), A3_SOUND_ENGINE, 0.5f, 1, car);
        tp_camera(w, a3_v3(50, 4, 10), -15, 0);
    } break;
    case 5: /* Horror */
        tp_sun(w, -30, 10, a3_v4(0.4f, 0.45f, 0.6f, 1), 0.08f);
        ws->ambient_intensity = 0.05f;
        ws->fog_density = 0.09f;
        ws->fog_color = a3_v4(0.03f, 0.03f, 0.04f, 1);
        ws->sky_top = ws->sky_horizon = a3_v4(0.02f, 0.02f, 0.03f, 1);
        tp_shape(w, "Floor", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(40, 1, 40), a3_v4(0.25f, 0.23f, 0.22f, 1), 1, 0);
        for (int i = 0; i < 8; ++i) {
            tp_shape(w, "Wall", A3_PRIM_CUBE, a3_v3(-2.5f, 1.5f, -i * 4.0f), a3_v3(0.3f, 3, 4), a3_v4(0.35f, 0.33f, 0.3f, 1), 1, 0);
            tp_shape(w, "Wall", A3_PRIM_CUBE, a3_v3(2.5f, 1.5f, -i * 4.0f), a3_v3(0.3f, 3, 4), a3_v4(0.35f, 0.33f, 0.3f, 1), 1, 0);
        }
        tp_light(w, "Flickering Bulb", A3_LIGHT_POINT, a3_v3(0, 2.6f, -14), a3_v4(1, 0.6f, 0.3f, 1), 0.8f, 6);
        tp_sound(w, "Wind", a3_v3_zero(), A3_SOUND_WIND, 0.35f, 0, A3_ENTITY_NULL);
        tp_effect(w, "Dust", a3_v3(0, 1.5f, -12), A3_PARTICLES_DUST);
        {
            A3Entity pl = tp_player(w, a3_v3(0, 0, 2), A3_CAM_FIRST_PERSON, 0);
            A3Entity cam = a3_entity_first_child(w, pl);
            A3Entity fl = tp_light(w, "Flashlight", A3_LIGHT_SPOT, a3_v3(0.2f, -0.2f, 0), a3_v4(1, 0.95f, 0.8f, 1), 4.0f, 18);
            ((A3CLight *)a3_component_get(w, fl, A3_T_LIGHT))->spot_angle = 40;
            if (a3_entity_valid(w, cam)) a3_entity_set_parent(w, fl, cam);
        }
        break;
    case 6: /* Shooter */
        tp_sun(w, -55, -30, a3_v4(1, 0.96f, 0.9f, 1), 1.0f);
        tp_shape(w, "Arena Floor", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(50, 1, 50), a3_v4(0.45f, 0.45f, 0.48f, 1), 1, 0);
        for (int i = 0; i < 6; ++i) tp_shape(w, "Cover", A3_PRIM_CUBE, a3_v3(-10.0f + i * 4, 0.6f, -8), a3_v3(2, 1.2f, 0.5f), stone, 1, 0);
        for (int i = 0; i < 5; ++i) tp_shape(w, "Target", A3_PRIM_CAPSULE, a3_v3(-8.0f + i * 4, 1, -18), a3_v3(0.8f, 1, 0.8f), a3_v4(0.9f, 0.3f, 0.25f, 1), 1, 0);
        tp_player(w, a3_v3(0, 0, 6), A3_CAM_FIRST_PERSON, 0);
        break;
    case 7: { /* Sandbox */
        tp_sun(w, -50, 35, a3_v4(1, 0.96f, 0.88f, 1), 1.0f);
        tp_shape(w, "Ground", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(80, 1, 80), grass, 1, 0);
        A3Entity ramp = tp_shape(w, "Ramp", A3_PRIM_CUBE, a3_v3(0, 1.5f, -10), a3_v3(6, 0.4f, 10), stone, 1, 0);
        ((A3CTransform *)a3_component_get(w, ramp, A3_T_TRANSFORM))->rotation = a3_quat_euler(18 * A3_DEG2RAD, 0, 0);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5 - i; ++j)
                tp_shape(w, "Crate", A3_PRIM_CUBE, a3_v3(-6.0f + j * 1.05f + i * 0.52f, 0.5f + i * 1.0f, 2), a3_v3_one(), wood, 1, 1);
        for (int i = 0; i < 10; ++i) {
            A3Entity b = tp_shape(w, "Ball", A3_PRIM_SPHERE, a3_v3(-2.0f + (f32)(i % 5), 6.0f + (f32)(i / 5) * 2, -12), a3_v3s(0.8f), a3_v4(0.2f + 0.08f * i, 0.5f, 0.9f - 0.07f * i, 1), 1, 1);
            ((A3CRigidBody *)a3_component_get(w, b, A3_T_RIGIDBODY))->restitution = 0.6f;
        }
        tp_player(w, a3_v3(0, 0, 10), A3_CAM_FIRST_PERSON, 0);
    } break;
    case 8: { /* Open world */
        tp_sun(w, -40, 50, a3_v4(1, 0.93f, 0.8f, 1), 1.1f);
        ws->fog_density = 0.006f;
        tp_shape(w, "Land", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(400, 1, 400), grass, 1, 0);
        A3Rng rng;
        a3_rng_seed(&rng, 1234, 5);
        for (int i = 0; i < 120; ++i) {
            f32 x = a3_rng_range_f32(&rng, -150, 150), z = a3_rng_range_f32(&rng, -150, 150);
            f32 h = a3_rng_range_f32(&rng, 3, 7);
            tp_shape(w, "Tree Trunk", A3_PRIM_CYLINDER, a3_v3(x, h * 0.25f, z), a3_v3(0.4f, h * 0.5f, 0.4f), wood, 1, 0);
            tp_shape(w, "Tree Crown", A3_PRIM_CONE, a3_v3(x, h * 0.75f, z), a3_v3(2.5f, h * 0.6f, 2.5f), a3_v4(0.2f, 0.45f, 0.22f, 1), 0, 0);
        }
        for (int i = 0; i < 20; ++i) {
            f32 x = a3_rng_range_f32(&rng, -120, 120), z = a3_rng_range_f32(&rng, -120, 120);
            tp_shape(w, "Rock", A3_PRIM_SPHERE, a3_v3(x, 0.4f, z), a3_v3(a3_rng_range_f32(&rng, 1, 3), 1.2f, a3_rng_range_f32(&rng, 1, 3)), stone, 1, 0);
        }
        tp_player(w, a3_v3(0, 0, 0), A3_CAM_THIRD_PERSON, 1);
        tp_sound(w, "Wind", a3_v3_zero(), A3_SOUND_WIND, 0.25f, 0, A3_ENTITY_NULL);
    } break;
    }
}

/* ======================================================================== */
/* Project files                                                            */
/* ======================================================================== */

static A3Result write_text(const char *path, const char *text) { return a3_file_write_atomic(path, text, a3_strlen(text)); }

static b32 write_project_file(A3Editor *ed, i32 tpl) {
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_EDITOR);
    A3JsonWriter jw;
    a3_jw_init(&jw, &sb, 0);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", "asm3d.project");
    a3_jw_kv_int(&jw, "version", A3_FORMAT_VERSION);
    a3_jw_kv_string(&jw, "name", ed->project_name);
    a3_jw_kv_string(&jw, "engine", A3_VERSION_STRING);
    a3_jw_kv_string(&jw, "startupScene", ed->scene_path);
    if (tpl >= 0) a3_jw_kv_string(&jw, "template", g_ed_templates[tpl]);
    a3_jw_key(&jw, "window");
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "title", ed->project_name);
    a3_jw_kv_int(&jw, "width", 1280);
    a3_jw_kv_int(&jw, "height", 720);
    a3_jw_kv_bool(&jw, "vsync", 1);
    a3_jw_end_object(&jw);
    a3_jw_key(&jw, "buildTargets");
    a3_jw_begin_array(&jw);
    a3_jw_string(&jw, "Desktop");
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    a3_strbuf_append_char(&sb, '\n');
    char path[ED_PATH];
    ed_project_path(ed, "project.a3proj", path, sizeof(path));
    b32 ok = write_text(path, sb.data) == A3_OK;
    a3_strbuf_free(&sb);
    return ok;
}

b32 ed_project_create(A3Editor *ed, const char *location, const char *name, i32 tpl) {
    if (!name || !*name) { a3_ui_notify(ed->ui, 0, "Please enter a project name"); return 0; }
    char dir[ED_PATH];
    a3_path_join(dir, sizeof(dir), location, name);
    if (a3_dir_exists(dir)) {
        char probe[ED_PATH];
        a3_path_join(probe, sizeof(probe), dir, "project.a3proj");
        if (a3_file_exists(probe)) { a3_log_hint(A3_LOG_ERROR, "editor", "Choose another name or open the existing project.", "a project already exists at %s", dir); return 0; }
    }
    static const char *folders[] = { "Assets/Scenes", "Assets/Models", "Assets/Textures", "Assets/Materials", "Assets/Shaders",
                                     "Assets/Scripts", "Assets/Audio", "Assets/Prefabs", ".asm3d/recovery", ".asm3d/cache" };
    for (u32 i = 0; i < A3_ARRAY_COUNT(folders); ++i) {
        char p[ED_PATH];
        a3_path_join(p, sizeof(p), dir, folders[i]);
        if (a3_dir_create(p) != A3_OK) { A3_ERROR("editor", "cannot create folder %s (check permissions)", p); return 0; }
    }
    ed_project_close(ed);
    a3_strcpy(ed->project_dir, sizeof(ed->project_dir), dir);
    a3_strcpy(ed->project_name, sizeof(ed->project_name), name);
    a3_strcpy(ed->scene_path, sizeof(ed->scene_path), "Assets/Scenes/Main.a3scene");
    if (tpl < 0 || (u32)tpl >= g_ed_template_count) tpl = 0;
    write_project_file(ed, tpl);
    char p[ED_PATH];
    a3_path_join(p, sizeof(p), dir, ".gitignore");
    write_text(p, "# ASM3D\n.asm3d/cache/\n.asm3d/recovery/\nBuilds/\n");
    a3_path_join(p, sizeof(p), dir, "README.txt");
    {
        char readme[1024];
        a3_snprintf(readme, sizeof(readme),
            "%s - made with ASM3D %s (template: %s)\n\n"
            "Open this folder with the ASM3D editor.\n"
            "  Assets/Scenes    levels (.a3scene, readable JSON)\n"
            "  Assets/Models    3D models (.obj)\n"
            "  Assets/Textures  images (.png, .tga, .bmp)\n"
            "  Assets/Scripts   gameplay scripts\n"
            "  Assets/Shaders   Shader Maker graphs\n"
            "Everything is plain text and works well with Git.\n", name, A3_VERSION_STRING, g_ed_templates[tpl]);
        write_text(p, readme);
    }
    /* scene */
    ed->world = a3_world_create("Main");
    ed_build_template_scene(ed->world, tpl);
    ed->has_project = 1;
    a3_assets_set_root(ed->project_dir);
    ed_scene_save(ed);
    ed_recent_add(ed, dir);
    A3_INFO("editor", "created project '%s' from template '%s' at %s", name, g_ed_templates[tpl], dir);
    a3_ui_notify(ed->ui, a3_ui_theme(ed->ui)->colors[A3_UIC_SUCCESS], "Created project '%s'", name);
    ed->show_welcome = 1;
    return 1;
}

b32 ed_project_open(A3Editor *ed, const char *dir) {
    char path[ED_PATH];
    a3_path_join(path, sizeof(path), dir, "project.a3proj");
    A3FileData fd;
    if (a3_file_read_all(path, A3_MEM_EDITOR, &fd) != A3_OK) {
        a3_log_hint(A3_LOG_ERROR, "editor", "Pick the folder that contains project.a3proj.", "no ASM3D project found in %s", dir);
        return 0;
    }
    A3Arena arena;
    a3_arena_init(&arena, A3_MEM_TEMP, A3_KB(16));
    A3JsonError err;
    A3Json *root = a3_json_parse((const char *)fd.data, fd.size, &arena, &err);
    if (!root || !a3_streq(a3_json_get_string(root, "format", ""), "asm3d.project")) {
        A3_ERROR("editor", "project file is damaged (line %d: %s)", err.line, err.message);
        a3_arena_release(&arena);
        a3_free(fd.data);
        return 0;
    }
    i32 ver = (i32)a3_json_get_number(root, "version", 1);
    if (ver > A3_FORMAT_VERSION) {
        a3_log_hint(A3_LOG_ERROR, "editor", "Update ASM3D to open this project. It was not modified.",
                    "project format %d is newer than this editor (%d)", ver, A3_FORMAT_VERSION);
        a3_arena_release(&arena);
        a3_free(fd.data);
        return 0;
    }
    ed_project_close(ed);
    a3_strcpy(ed->project_dir, sizeof(ed->project_dir), dir);
    a3_strcpy(ed->project_name, sizeof(ed->project_name), a3_json_get_string(root, "name", a3_path_filename(dir)));
    a3_strcpy(ed->scene_path, sizeof(ed->scene_path), a3_json_get_string(root, "startupScene", "Assets/Scenes/Main.a3scene"));
    a3_arena_release(&arena);
    a3_free(fd.data);
    ed->has_project = 1;
    a3_assets_set_root(ed->project_dir);
    ed_custom_components_load(ed); /* custom component types must exist before the scene loads */
    if (!ed_scene_open(ed, ed->scene_path)) ed_scene_new(ed);
    ed_recent_add(ed, dir);
    /* editor layout of this project */
    char lp[ED_PATH];
    ed_project_path(ed, ".asm3d/layout.json", lp, sizeof(lp));
    A3FileData lf;
    if (a3_file_read_all(lp, A3_MEM_TEMP, &lf) == A3_OK) { a3_dock_load(&ed->dock, (const char *)lf.data, lf.size); a3_free(lf.data); }
    A3_INFO("editor", "opened project '%s'", ed->project_name);
    return 1;
}

void ed_project_close(A3Editor *ed) {
    if (ed->mode != ED_EDIT) ed_stop(ed);
    if (ed->has_project) {
        char lp[ED_PATH];
        ed_project_path(ed, ".asm3d/layout.json", lp, sizeof(lp));
        A3StrBuf sb;
        a3_strbuf_init(&sb, A3_MEM_TEMP);
        a3_dock_save(&ed->dock, &sb);
        a3_file_write_atomic(lp, sb.data, sb.len);
        a3_strbuf_free(&sb);
    }
    if (ed->world) { a3_physics_release(ed->world); a3_world_destroy(ed->world); }
    ed->world = 0;
    ed->has_project = 0;
    ed->selected = 0;
    ed->dirty = 0;
    ed_undo_clear(&ed->undo);
}

/* ======================================================================== */
/* Scenes                                                                   */
/* ======================================================================== */

void ed_scene_new(A3Editor *ed) {
    if (ed->world) a3_world_destroy(ed->world);
    ed->world = a3_world_create("Untitled");
    ed_build_template_scene(ed->world, 0);
    ed->selected = 0;
    ed->dirty = 1;
    ed_undo_clear(&ed->undo);
}

b32 ed_scene_save(A3Editor *ed) {
    if (!ed->has_project || !ed->world) return 0;
    char path[ED_PATH];
    ed_project_path(ed, ed->scene_path, path, sizeof(path));
    char dir[ED_PATH];
    a3_path_dirname(path, dir, sizeof(dir));
    a3_dir_create(dir);
    if (a3_scene_save_file(ed->world, path, 0) != A3_OK) {
        a3_ui_notify(ed->ui, a3_ui_theme(ed->ui)->colors[A3_UIC_ERROR], "Saving failed - see Console");
        return 0;
    }
    ed->dirty = 0;
    /* a successful save supersedes the recovery copy */
    char rp[ED_PATH];
    ed_project_path(ed, ".asm3d/recovery/autosave.a3scene", rp, sizeof(rp));
    a3_file_delete(rp);
    a3_ui_notify(ed->ui, a3_ui_theme(ed->ui)->colors[A3_UIC_SUCCESS], "Saved %s", a3_path_filename(ed->scene_path));
    return 1;
}

b32 ed_scene_open(A3Editor *ed, const char *rel) {
    char path[ED_PATH];
    ed_project_path(ed, rel, path, sizeof(path));
    A3World *w = a3_world_create("Scene");
    A3SceneLoadReport rep;
    if (a3_scene_load_file(w, path, &rep) != A3_OK) {
        a3_log_hint(A3_LOG_ERROR, "editor", rep.hint, "could not open scene '%s': %s", rel, rep.error);
        a3_world_destroy(w);
        return 0;
    }
    if (ed->world) { a3_physics_release(ed->world); a3_world_destroy(ed->world); }
    ed->world = w;
    a3_strcpy(ed->scene_path, sizeof(ed->scene_path), rel);
    ed->selected = 0;
    ed->dirty = 0;
    ed_undo_clear(&ed->undo);
    for (u32 i = 0; i < rep.warning_count; ++i) A3_WARN("editor", "%s", rep.warnings[i]);
    /* crash recovery: a newer autosave than the scene means unsaved work was lost */
    char rp[ED_PATH];
    ed_project_path(ed, ".asm3d/recovery/autosave.a3scene", rp, sizeof(rp));
    A3FileInfo si, ri;
    if (a3_file_info(rp, &ri) && a3_file_info(path, &si) && ri.mtime_ns > si.mtime_ns) ed_open_modal(ed, "recover");
    return 1;
}

void ed_autosave_tick(A3Editor *ed) {
    if (!ed->has_project || !ed->dirty || !ed->world) return;
    u64 now = a3_time_ns();
    if (now - ed->last_autosave_ns < 30ull * 1000000000ull) return;
    ed->last_autosave_ns = now;
    char rp[ED_PATH];
    ed_project_path(ed, ".asm3d/recovery/autosave.a3scene", rp, sizeof(rp));
    if (a3_scene_save_file(ed->world, rp, 0) == A3_OK) A3_DEBUG("editor", "autosaved to %s", rp);
}

/* ======================================================================== */
/* Recent projects                                                          */
/* ======================================================================== */

static void recent_path(char *out, usize cap) {
    char dir[ED_PATH];
    if (!a3_get_user_data_dir(dir, sizeof(dir))) a3_strcpy(dir, sizeof(dir), ".");
    a3_path_join(out, cap, dir, "recent.json");
}

void ed_recent_load(A3Editor *ed) {
    char p[ED_PATH];
    recent_path(p, sizeof(p));
    A3FileData fd;
    ed->recent_count = 0;
    if (a3_file_read_all(p, A3_MEM_TEMP, &fd) != A3_OK) return;
    A3Arena arena;
    a3_arena_init(&arena, A3_MEM_TEMP, 4096);
    A3Json *root = a3_json_parse((const char *)fd.data, fd.size, &arena, 0);
    A3_JSON_FOREACH(it, a3_json_get(root, "projects")) {
        if (ed->recent_count >= 8) break;
        const char *s = a3_json_string(it, 0);
        if (s && a3_dir_exists(s)) a3_strcpy(ed->recent[ed->recent_count++], ED_PATH, s);
    }
    a3_arena_release(&arena);
    a3_free(fd.data);
}

void ed_recent_add(A3Editor *ed, const char *dir) {
    if (ed->no_recent) return;
    char list[8][ED_PATH];
    u32 n = 0;
    a3_strcpy(list[n++], ED_PATH, dir);
    for (u32 i = 0; i < ed->recent_count && n < 8; ++i) if (!a3_streq(ed->recent[i], dir)) a3_strcpy(list[n++], ED_PATH, ed->recent[i]);
    for (u32 i = 0; i < n; ++i) a3_strcpy(ed->recent[i], ED_PATH, list[i]);
    ed->recent_count = n;
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    A3JsonWriter jw;
    a3_jw_init(&jw, &sb, 0);
    a3_jw_begin_object(&jw);
    a3_jw_key(&jw, "projects");
    a3_jw_begin_array(&jw);
    for (u32 i = 0; i < n; ++i) a3_jw_string(&jw, list[i]);
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    char p[ED_PATH];
    recent_path(p, sizeof(p));
    a3_file_write_atomic(p, sb.data, sb.len);
    a3_strbuf_free(&sb);
}
