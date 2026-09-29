/*
 * ASM3D - a3_people.c
 * The walk pose of every person is computed in one batch by the assembly
 * kernel a3_gk_walk_pose (a3_geom_x64.S); this file builds bodies and
 * applies the angles to the joints.
 */
#include "a3_people.h"
#include "a3_geom_kernels.h"
#include "a3_procmeshes.h"
#include "../scene/a3_components.h"
#include "../core/a3_string.h"
#include "../core/a3_memory.h"
#include "../core/a3_hash.h"

u32 A3_T_PERSON = 0xFFFFFFFFu;

void a3_people_register(void) {
    if (A3_T_PERSON != 0xFFFFFFFFu) return;
    A3CPerson p;
    a3_zero_struct(&p);
    p.shirt = a3_v4(0.2f, 0.45f, 0.8f, 1);
    p.skin = 0.3f;
    p.style = 0.3f;
    p.height = 1.75f;
    p.animate = 1;
    u32 t = a3_component_register("Person", "Gameplay", sizeof(A3CPerson), 16, &p, A3_COMP_BUILTIN,
        "A human body (head, torso, arms, legs) that walks and runs when the object moves.");
    A3_T_PERSON = t;
    a3_component_type(t)->icon = "person";
    a3_component_require(t, "Transform");
    A3_REFLECT_FIELD(t, A3CPerson, shirt, A3_FIELD_COLOR, "Shirt", "Shirt color.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CPerson, skin, A3_FIELD_F32, "Skin Tone", "0 light .. 1 dark."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CPerson, style, A3_FIELD_F32, "Style", "Hair color, trousers and shoes."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CPerson, height, A3_FIELD_F32, "Height", "Meters."), 0.5f, 2.5f, 0.01f);
    A3_REFLECT_FIELD(t, A3CPerson, animate, A3_FIELD_BOOL, "Animate", "Swing arms and legs when moving.");
    A3_REFLECT_FIELD(t, A3CPerson, speed, A3_FIELD_F32, "Speed", "Measured speed (m/s).")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
}

static A3Entity child(A3World *w, A3Entity parent, const char *name) {
    for (A3Entity c = a3_entity_first_child(w, parent); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c))
        if (a3_streq(a3_entity_name(w, c), name)) return c;
    return A3_ENTITY_NULL;
}

static A3Entity limb(A3World *w, A3Entity parent, const char *name, A3Vec3 pos, const char *mesh, const A3CPerson *p) {
    A3Entity e = a3_entity_create(w, name);
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    t->position = pos;
    a3_entity_set_parent(w, e, parent);
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, e, A3_T_MESH_RENDERER);
    mr->primitive = A3_PRIM_NONE;
    mr->base_color = p->shirt;
    mr->metallic = p->skin;       /* builtin:human reads skin tone and style from these */
    mr->roughness = p->style;
    a3_strcpy(mr->mesh.path, sizeof(mr->mesh.path), mesh);
    a3_strcpy(mr->material.path, sizeof(mr->material.path), "builtin:human");
    return e;
}

static void build_body(A3World *w, A3Entity e, const A3CPerson *p) {
    a3_procmeshes_register();
    A3Entity hips = limb(w, e, "Hips", a3_v3(0, 0.92f, 0), "builtin:human_torso", p);
    limb(w, hips, "Head", a3_v3(0, 0.6f, 0.0f), "builtin:human_head", p);
    for (int s = -1; s <= 1; s += 2) {
        A3Entity arm = limb(w, hips, s < 0 ? "Arm L" : "Arm R", a3_v3(s * 0.215f, 0.47f, 0.01f), "builtin:human_upper_arm", p);
        limb(w, arm, s < 0 ? "Forearm L" : "Forearm R", a3_v3(0, -0.29f, 0), "builtin:human_forearm", p);
        A3Entity leg = limb(w, e, s < 0 ? "Leg L" : "Leg R", a3_v3(s * 0.095f, 0.92f, 0), "builtin:human_thigh", p);
        limb(w, leg, s < 0 ? "Shin L" : "Shin R", a3_v3(0, -0.44f, 0), "builtin:human_shin", p);
    }
}

static void set_rot(A3World *w, A3Entity e, f32 ax, f32 yaw) {
    A3CTransform *t = a3_transform(w, e);
    if (!t) return;
    t->rotation = yaw != 0.0f ? a3_quat_mul(a3_quat_axis_angle(a3_v3(0, 1, 0), yaw), a3_quat_axis_angle(a3_v3(1, 0, 0), ax))
                              : a3_quat_axis_angle(a3_v3(1, 0, 0), ax);
}

void a3_people_update(A3World *w, f32 dt) {
    if (A3_T_PERSON == 0xFFFFFFFFu) return;
    u32 n = 0;
    const A3Entity *ents = 0;
    a3_component_array(w, A3_T_PERSON, &n, &ents);
    if (!n) return;
    f32 *phase = A3_NEW_ARRAY(f32, n * 12, A3_MEM_TEMP);
    if (!phase) return;
    f32 *amp = phase + n, *pose = phase + 2 * n;
    A3Entity *list = A3_NEW_ARRAY(A3Entity, n, A3_MEM_TEMP);
    if (!list) { a3_free(phase); return; }
    for (u32 i = 0; i < n; ++i) list[i] = ents[i];     /* building bodies may move the component array */
    u32 m = 0;
    for (u32 i = 0; i < n; ++i) {
        A3Entity e = list[i];
        A3CPerson *p = (A3CPerson *)a3_component_get(w, e, A3_T_PERSON);
        if (!p) continue;
        if (a3_entity_is_null(child(w, e, "Hips"))) {
            A3CPerson copy = *p;
            build_body(w, e, &copy);
            p = (A3CPerson *)a3_component_get(w, e, A3_T_PERSON);
            if (!p) continue;
        }
        A3CTransform *t = a3_transform(w, e);
        if (t) t->scale = a3_v3s(a3_clampf(p->height, 0.3f, 3.0f) / 1.75f);
        A3Vec3 pos = a3_transform_world_position(w, e);
        f32 measured = 0;
        if (p->has_last && dt > 0) {
            A3Vec3 d = a3_v3_sub(pos, p->last_position);
            measured = a3_sqrtf(d.x * d.x + d.z * d.z) / dt;
            if (measured > 30.0f) measured = 0;               /* teleport */
        }
        p->last_position = pos;
        p->has_last = 1;
        p->speed = a3_lerpf(p->speed, measured, a3_minf(1.0f, dt * 8.0f));
        f32 sp = p->animate ? p->speed : 0.0f;
        p->phase = a3_wrap_angle(p->phase + sp * 4.4f * dt);
        phase[m] = p->phase;
        amp[m] = a3_clampf(sp / 1.4f, 0.0f, 1.9f);
        list[m++] = e;
    }
    a3_gk_walk_pose(phase, amp, m, pose);                    /* assembly */
    for (u32 i = 0; i < m; ++i) {
        A3Entity e = list[i];
        const f32 *o = pose + i * 10;
        A3Entity hips = child(w, e, "Hips");
        A3Entity ll = child(w, e, "Leg L"), lr = child(w, e, "Leg R");
        set_rot(w, ll, o[0], 0);
        set_rot(w, lr, o[1], 0);
        set_rot(w, child(w, ll, "Shin L"), o[2], 0);
        set_rot(w, child(w, lr, "Shin R"), o[3], 0);
        if (a3_entity_valid(w, hips)) {
            A3CTransform *ht = a3_transform(w, hips);
            if (ht) ht->position.y = 0.92f + o[8];
            set_rot(w, hips, 0.04f * amp[i], o[9]);          /* lean forward a little when running */
            A3Entity al = child(w, hips, "Arm L"), ar = child(w, hips, "Arm R");
            set_rot(w, al, o[4], 0);
            set_rot(w, ar, o[5], 0);
            set_rot(w, child(w, al, "Forearm L"), o[6], 0);
            set_rot(w, child(w, ar, "Forearm R"), o[7], 0);
        }
        A3CTransform *lt = a3_transform(w, ll), *rt = a3_transform(w, lr);
        if (lt) lt->position.y = 0.92f + o[8];
        if (rt) rt->position.y = 0.92f + o[8];
    }
    a3_free(list);
    a3_free(phase);
}

static const A3Vec4 k_shirt_colors[] = {
    { 0.95f, 0.95f, 0.95f, 1 }, { 0.92f, 0.35f, 0.5f, 1 }, { 0.2f, 0.62f, 0.85f, 1 }, { 0.95f, 0.78f, 0.2f, 1 },
    { 0.08f, 0.08f, 0.1f, 1 }, { 0.35f, 0.72f, 0.4f, 1 }, { 0.88f, 0.45f, 0.18f, 1 }, { 0.5f, 0.32f, 0.75f, 1 },
    { 0.7f, 0.1f, 0.12f, 1 }, { 0.55f, 0.78f, 0.9f, 1 }, { 0.9f, 0.85f, 0.7f, 1 },
};

A3Entity a3_person_spawn(A3World *w, const char *name, A3Vec3 feet, f32 yaw_deg, u64 seed) {
    a3_people_register();
    A3Rng rng;
    a3_rng_seed(&rng, seed ? seed : 1, 5);
    A3Entity e = a3_entity_create(w, name ? name : "Person");
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    t->position = feet;
    t->rotation = a3_quat_axis_angle(a3_v3(0, 1, 0), yaw_deg * A3_DEG2RAD);
    A3CPerson *p = (A3CPerson *)a3_component_add(w, e, A3_T_PERSON);
    if (p) {
        p->shirt = k_shirt_colors[a3_rng_range_u32(&rng, A3_ARRAY_COUNT(k_shirt_colors))];
        p->skin = a3_rng_f32(&rng);
        p->style = a3_rng_f32(&rng);
        p->height = a3_rng_range_f32(&rng, 1.58f, 1.92f);
    }
    return e;
}
