/*
 * ASM3D - a3_physics.c
 * Physics world: ECS sync, broadphase (assembly SAP), narrowphase, solver
 * (assembly sequential impulses), integration (assembly), events, queries.
 */
#include "a3_physics.h"
#include "a3_physics_internal.h"
#include "a3_physics_kernels.h"
#include "../scene/a3_components.h"
#include "../resource/a3_assets.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_hash.h"
#include "../core/a3_sort.h"
#include "../platform/a3_platform.h"

u32 A3_T_RIGIDBODY = 0xFFFFFFFFu;
u32 A3_T_COLLIDER = 0xFFFFFFFFu;
u32 A3_T_CHARACTER = 0xFFFFFFFFu;

#define MAX_WORLDS 16
#define CACHE_POINTS 4

typedef struct PBody {
    A3Entity e;
    u64 guid;
    A3ShapeInstance shape;
    A3Vec3 center_local;     /* collider center * entity scale (entity-local, unrotated) */
    u32 slot;                /* solver slot, 0 = static world */
    i32 type;
    b32 trigger;
    b32 sleeping;
    u32 layer, mask;
    f32 inv_mass;
    A3Vec3 inv_inertia_local;
    A3Mat4 inv_inertia_world; /* rotation part used */
    f32 friction, restitution;
    i32 mesh;                /* index into trimesh cache or -1 */
} PBody;

typedef struct CachedPair {
    u64 key;
    u32 a, b;                /* body indices this step */
    A3Entity ea, eb;
    u32 frame;
    b32 trigger;
    u32 count;
    A3Vec3 local_a[CACHE_POINTS];
    f32 acc_n[CACHE_POINTS], acc_t1[CACHE_POINTS], acc_t2[CACHE_POINTS];
    A3Vec3 point, normal;
    f32 impulse;
} CachedPair;

typedef struct MeshCache {
    u64 guid;
    u64 hash;
    A3TriMesh tm;
    u32 frame;
} MeshCache;

typedef struct ContactRef { u32 pair; u32 point; u32 row; } ContactRef;

typedef struct A3PhysicsWorld {
    A3World *world;
    A3PhysicsSettings settings;
    A3PhysicsStats stats;
    u32 frame;
    u32 built_structure;
    b32 built;
    A3_ARRAY_TYPE(PBody) bodies;
    /* solver slots */
    A3Vec4 *lin_vel, *ang_vel, *accel, *damp, *pos;
    A3Quat *rot;
    u32 *slot_body;          /* slot -> body index */
    u32 slot_count, slot_cap;
    /* broadphase */
    A3Vec4 *mins, *maxs, *centers, *cols, *halves, *smin, *smax;
    A3SortPair *sort, *sort_tmp;
    u32 bp_cap;
    u32 *pairs;
    u32 pair_cap;
    /* contacts */
    A3_ARRAY_TYPE(CachedPair) cache;
    A3HashMap cache_map;     /* pair key -> cache index + 1 */
    A3_ARRAY_TYPE(A3SolverRow) rows;
    A3_ARRAY_TYPE(ContactRef) refs;
    A3_ARRAY_TYPE(A3ContactEvent) events;
    A3_ARRAY_TYPE(MeshCache) meshes;
    A3HashMap body_by_guid;  /* guid -> body index + 1 */
} A3PhysicsWorld;

static struct { A3World *key; A3PhysicsWorld *pw; } g_worlds[MAX_WORLDS];

static const char *const g_body_names[] = { "Static", "Dynamic", "Kinematic" };
static const char *const g_shape_names[] = { "Box", "Sphere", "Capsule", "Mesh" };
static const char *const g_cam_names[] = { "None", "First Person", "Third Person" };

/* ======================================================================== */
/* Registration                                                             */
/* ======================================================================== */

/* Beginner convenience: a new collider matches the entity's visible shape. */
static void collider_on_add(A3World *w, A3Entity e, void *data) {
    A3CCollider *c = (A3CCollider *)data;
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(w, e, A3_T_MESH_RENDERER);
    if (!mr) return;
    if (mr->mesh.path[0]) { c->shape = A3_SHAPE_MESH; return; }
    switch (mr->primitive) {
    case A3_PRIM_SPHERE: c->shape = A3_SHAPE_SPHERE; c->radius = 0.5f; break;
    case A3_PRIM_CAPSULE: c->shape = A3_SHAPE_CAPSULE; c->radius = 0.5f; c->height = 2.0f; break;
    case A3_PRIM_PLANE: c->shape = A3_SHAPE_BOX; c->size = a3_v3(1, 0.02f, 1); c->center = a3_v3(0, -0.01f, 0); break;
    case A3_PRIM_CYLINDER: case A3_PRIM_CONE: case A3_PRIM_CUBE: default: c->shape = A3_SHAPE_BOX; c->size = a3_v3_one(); break;
    }
}

/* A character controller configures everything a playable character needs. */
static void character_on_add(A3World *w, A3Entity e, void *data) {
    A3CCharacterController *cc = (A3CCharacterController *)data;
    /* camera child for first/third person */
    b32 has_cam_child = 0;
    for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c))
        if (a3_component_has(w, c, A3_T_CAMERA)) has_cam_child = 1;
    if (!has_cam_child && cc->camera_mode != A3_CAM_NONE) {
        /* demote other primary cameras so the player's view is used */
        u32 n = 0;
        const A3Entity *ents = 0;
        A3CCamera *cams = (A3CCamera *)a3_component_array(w, A3_T_CAMERA, &n, &ents);
        for (u32 i = 0; i < n; ++i) cams[i].primary = 0;
        A3Entity cam = a3_entity_create(w, "Player Camera");
        a3_entity_set_parent(w, cam, e);
        A3CTransform *t = (A3CTransform *)a3_component_add(w, cam, A3_T_TRANSFORM);
        t->position = a3_v3(0, cc->eye_height, 0);
        A3CCamera *cm = (A3CCamera *)a3_component_add(w, cam, A3_T_CAMERA);
        cm->primary = 1;
        cm->near_plane = 0.05f;
        A3_INFO("physics", "Character Controller added a camera at eye height (%.2f m)", cc->eye_height);
    }
}

void a3_physics_register(void) {
    if (A3_T_RIGIDBODY != 0xFFFFFFFFu) return;
    A3CRigidBody rb;
    a3_zero_struct(&rb);
    rb.type = A3_BODY_DYNAMIC; rb.mass = 1.0f; rb.friction = 0.6f; rb.restitution = 0.1f;
    rb.linear_damping = 0.05f; rb.angular_damping = 0.1f; rb.gravity_scale = 1.0f;
    u32 t = a3_component_register("RigidBody", "Physics", sizeof(A3CRigidBody), 16, &rb, A3_COMP_BUILTIN,
        "Makes the object move with physics: gravity, collisions and forces. Needs a Collider.");
    A3_T_RIGIDBODY = t;
    a3_component_type(t)->icon = "physics";
    a3_component_require(t, "Transform");
    a3_component_require(t, "Collider");
    A3FieldDesc *f = A3_REFLECT_FIELD(t, A3CRigidBody, type, A3_FIELD_ENUM, "Body Type", "Dynamic moves with physics; Static never moves; Kinematic is moved by your code and pushes others.");
    f->enum_names = g_body_names; f->enum_count = 3;
    a3_field_range(A3_REFLECT_FIELD(t, A3CRigidBody, mass, A3_FIELD_F32, "Mass (kg)", "Heavier objects push lighter ones."), 0.01f, 100000, 0.1f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CRigidBody, friction, A3_FIELD_F32, "Friction", "0 = ice, 1 = rubber."), 0, 2, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CRigidBody, restitution, A3_FIELD_F32, "Bounciness", "0 = no bounce, 1 = perfect bounce."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CRigidBody, gravity_scale, A3_FIELD_F32, "Gravity Scale", "1 = normal gravity, 0 = floats."), -10, 10, 0.1f);
    A3_REFLECT_FIELD(t, A3CRigidBody, lock_rotation, A3_FIELD_BOOL, "Lock Rotation", "Prevents tipping over.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CRigidBody, linear_damping, A3_FIELD_F32, "Linear Damping", "Air resistance for movement."), 0, 10, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CRigidBody, angular_damping, A3_FIELD_F32, "Angular Damping", "Air resistance for spinning."), 0, 10, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CRigidBody, velocity, A3_FIELD_VEC3, "Velocity", "Current speed and direction (m/s). Set it to launch objects.")->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CRigidBody, angular_velocity, A3_FIELD_VEC3, "Spin", "Rotation speed (radians/s).")->flags |= A3_FIELD_FLAG_ADVANCED;

    A3CCollider c;
    a3_zero_struct(&c);
    c.shape = A3_SHAPE_BOX; c.size = a3_v3_one(); c.radius = 0.5f; c.height = 2.0f; c.collide_mask = 0xFFFFFFFFu;
    t = a3_component_register("Collider", "Physics", sizeof(A3CCollider), 16, &c, A3_COMP_BUILTIN,
        "Gives the object a physical shape so things can bump into it. Without a RigidBody it never moves (walls, floors).");
    A3_T_COLLIDER = t;
    a3_component_type(t)->icon = "shape";
    a3_component_type(t)->on_add = collider_on_add;
    a3_component_require(t, "Transform");
    f = A3_REFLECT_FIELD(t, A3CCollider, shape, A3_FIELD_ENUM, "Shape", "Box, Sphere and Capsule are fast. Mesh uses the model's triangles (static objects only).");
    f->enum_names = g_shape_names; f->enum_count = A3_SHAPE_COUNT;
    A3_REFLECT_FIELD(t, A3CCollider, size, A3_FIELD_VEC3, "Box Size", "Full size of the box (before the entity's scale).");
    a3_field_range(A3_REFLECT_FIELD(t, A3CCollider, radius, A3_FIELD_F32, "Radius", "Sphere / capsule radius."), 0.01f, 1000, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CCollider, height, A3_FIELD_F32, "Height", "Capsule total height including the rounded ends."), 0.01f, 1000, 0.01f);
    A3_REFLECT_FIELD(t, A3CCollider, center, A3_FIELD_VEC3, "Center", "Offset of the shape from the entity's origin.");
    A3_REFLECT_FIELD(t, A3CCollider, is_trigger, A3_FIELD_BOOL, "Is Trigger", "Detects when things enter it without blocking them (doors, pickups, checkpoints).");
    a3_field_range(A3_REFLECT_FIELD(t, A3CCollider, layer, A3_FIELD_I32, "Layer", "Collision layer 0-31."), 0, 31, 1)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CCollider, collide_mask, A3_FIELD_U32, "Collides With", "Bit mask of layers this collider hits.")->flags |= A3_FIELD_FLAG_ADVANCED;

    A3CCharacterController cc;
    a3_zero_struct(&cc);
    cc.height = 1.8f; cc.radius = 0.35f; cc.step_height = 0.35f; cc.slope_limit = 50.0f;
    cc.walk_speed = 4.5f; cc.sprint_speed = 7.5f; cc.jump_height = 1.2f; cc.gravity_scale = 1.0f;
    cc.air_control = 0.35f; cc.acceleration = 40.0f; cc.camera_mode = A3_CAM_FIRST_PERSON;
    cc.mouse_sensitivity = 0.15f; cc.camera_distance = 4.0f; cc.eye_height = 1.65f; cc.use_input = 1;
    t = a3_component_register("CharacterController", "Gameplay", sizeof(A3CCharacterController), 16, &cc, A3_COMP_BUILTIN,
        "A ready-to-play character: walking, running, jumping, stairs, slopes, mouse look and a camera. Controls: WASD, Space, Shift.");
    A3_T_CHARACTER = t;
    a3_component_type(t)->icon = "person";
    a3_component_type(t)->on_add = character_on_add;
    a3_component_require(t, "Transform");
    a3_component_suggest(t, "Light", "Add a Directional Light so the world is lit, then press Play to walk around.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, walk_speed, A3_FIELD_F32, "Walk Speed", "Meters per second."), 0, 50, 0.1f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, sprint_speed, A3_FIELD_F32, "Sprint Speed", "Speed while holding Sprint (Shift)."), 0, 80, 0.1f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, jump_height, A3_FIELD_F32, "Jump Height", "How high a jump reaches, in meters."), 0, 20, 0.05f);
    f = A3_REFLECT_FIELD(t, A3CCharacterController, camera_mode, A3_FIELD_ENUM, "Camera", "First Person looks through the eyes; Third Person follows behind.");
    f->enum_names = g_cam_names; f->enum_count = 3;
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, mouse_sensitivity, A3_FIELD_F32, "Mouse Sensitivity", "How fast the view turns."), 0.01f, 2, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, camera_distance, A3_FIELD_F32, "Camera Distance", "Third person camera distance."), 0.5f, 30, 0.1f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, height, A3_FIELD_F32, "Height", "Character height in meters."), 0.2f, 10, 0.05f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, radius, A3_FIELD_F32, "Radius", "Character width / 2."), 0.05f, 5, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, eye_height, A3_FIELD_F32, "Eye Height", "Camera height above the feet."), 0.1f, 10, 0.05f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, step_height, A3_FIELD_F32, "Step Height", "Highest ledge the character walks up without jumping."), 0, 2, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, slope_limit, A3_FIELD_F32, "Slope Limit", "Steepest walkable slope, in degrees."), 0, 89, 1)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, air_control, A3_FIELD_F32, "Air Control", "Steering while in the air."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, acceleration, A3_FIELD_F32, "Acceleration", "How quickly the character reaches full speed."), 1, 500, 1)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CCharacterController, gravity_scale, A3_FIELD_F32, "Gravity Scale", "Multiplier for world gravity."), 0, 10, 0.05f)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CCharacterController, use_input, A3_FIELD_BOOL, "Player Controlled", "Read the keyboard/mouse. Turn off to drive it from scripts or AI.");
    A3_REFLECT_FIELD(t, A3CCharacterController, grounded, A3_FIELD_BOOL, "On Ground", "Runtime state.")->flags |= A3_FIELD_FLAG_READONLY | A3_FIELD_FLAG_TRANSIENT;
}

/* ======================================================================== */
/* World registry                                                           */
/* ======================================================================== */

static A3PhysicsWorld *get_pw(A3World *w, b32 create) {
    if (!w) return 0;
    for (int i = 0; i < MAX_WORLDS; ++i) if (g_worlds[i].key == w) return g_worlds[i].pw;
    if (!create) return 0;
    for (int i = 0; i < MAX_WORLDS; ++i) {
        if (!g_worlds[i].key) {
            A3PhysicsWorld *pw = A3_NEW(A3PhysicsWorld, A3_MEM_PHYSICS);
            if (!pw) return 0;
            pw->world = w;
            pw->settings.solver_iterations = 10;
            pw->settings.baumgarte = 0.2f;
            pw->settings.slop = 0.01f;
            pw->settings.sleeping = 1;
            pw->settings.sleep_velocity = 0.08f;
            pw->settings.sleep_time = 0.6f;
            a3_hashmap_init(&pw->cache_map, 256, A3_MEM_PHYSICS);
            a3_hashmap_init(&pw->body_by_guid, 256, A3_MEM_PHYSICS);
            g_worlds[i].key = w;
            g_worlds[i].pw = pw;
            return pw;
        }
    }
    A3_ERROR("physics", "too many physics worlds");
    return 0;
}

void a3_physics_release(A3World *w) {
    for (int i = 0; i < MAX_WORLDS; ++i) {
        if (g_worlds[i].key != w) continue;
        A3PhysicsWorld *pw = g_worlds[i].pw;
        a3_array_free(pw->bodies);
        a3_free(pw->lin_vel); a3_free(pw->ang_vel); a3_free(pw->accel); a3_free(pw->damp); a3_free(pw->pos); a3_free(pw->rot); a3_free(pw->slot_body);
        a3_free(pw->mins); a3_free(pw->maxs); a3_free(pw->centers); a3_free(pw->cols); a3_free(pw->halves);
        a3_free(pw->smin); a3_free(pw->smax); a3_free(pw->sort); a3_free(pw->sort_tmp); a3_free(pw->pairs);
        a3_array_free(pw->cache);
        a3_array_free(pw->rows);
        a3_array_free(pw->refs);
        a3_array_free(pw->events);
        for (u32 m = 0; m < pw->meshes.count; ++m) a3_trimesh_free(&pw->meshes.data[m].tm);
        a3_array_free(pw->meshes);
        a3_hashmap_free(&pw->cache_map);
        a3_hashmap_free(&pw->body_by_guid);
        a3_free(pw);
        g_worlds[i].key = 0;
        g_worlds[i].pw = 0;
    }
}

A3PhysicsSettings *a3_physics_settings(A3World *w) { A3PhysicsWorld *pw = get_pw(w, 1); return pw ? &pw->settings : 0; }
const A3PhysicsStats *a3_physics_stats(A3World *w) { A3PhysicsWorld *pw = get_pw(w, 1); return pw ? &pw->stats : 0; }

/* ======================================================================== */
/* Body sync                                                                */
/* ======================================================================== */

static b32 grow_slots(A3PhysicsWorld *pw, u32 need) {
    if (need <= pw->slot_cap) return 1;
    u32 cap = pw->slot_cap ? pw->slot_cap : 64;
    while (cap < need) cap *= 2;
#define GROW(ptr, T) do { T *n_ = (T *)a3_realloc(ptr, sizeof(T) * cap, A3_MEM_PHYSICS); if (!n_) return 0; ptr = n_; } while (0)
    GROW(pw->lin_vel, A3Vec4); GROW(pw->ang_vel, A3Vec4); GROW(pw->accel, A3Vec4); GROW(pw->damp, A3Vec4);
    GROW(pw->pos, A3Vec4); GROW(pw->rot, A3Quat); GROW(pw->slot_body, u32);
    pw->slot_cap = cap;
    return 1;
}

static b32 grow_bp(A3PhysicsWorld *pw, u32 need) {
    if (need <= pw->bp_cap) return 1;
    u32 cap = pw->bp_cap ? pw->bp_cap : 64;
    while (cap < need) cap *= 2;
    GROW(pw->mins, A3Vec4); GROW(pw->maxs, A3Vec4); GROW(pw->centers, A3Vec4); GROW(pw->halves, A3Vec4);
    GROW(pw->smin, A3Vec4); GROW(pw->smax, A3Vec4); GROW(pw->sort, A3SortPair); GROW(pw->sort_tmp, A3SortPair);
    A3Vec4 *nc = (A3Vec4 *)a3_realloc(pw->cols, sizeof(A3Vec4) * cap * 3, A3_MEM_PHYSICS);
    if (!nc) return 0;
    pw->cols = nc;
    pw->bp_cap = cap;
#undef GROW
    return 1;
}

static i32 get_trimesh(A3PhysicsWorld *pw, A3World *w, A3Entity e, const A3Mat4 *world) {
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(w, e, A3_T_MESH_RENDERER);
    if (!mr) return -1;
    u32 mid = a3_assets_mesh_for_renderer(mr);
    const A3MeshAsset *ma = a3_assets_mesh_get(mid);
    if (!ma || !ma->cpu.vertex_count) return -1;
    u64 guid = a3_entity_guid(w, e);
    u64 h = a3_hash64(world->m, sizeof(world->m), (u64)mid * 7919u + ma->version);
    for (u32 i = 0; i < pw->meshes.count; ++i) {
        MeshCache *mc = &pw->meshes.data[i];
        if (mc->guid != guid) continue;
        mc->frame = pw->frame;
        if (mc->hash == h) return (i32)i;
        a3_trimesh_free(&mc->tm);
        A3Vec3 *pos = A3_NEW_ARRAY(A3Vec3, ma->cpu.vertex_count, A3_MEM_TEMP);
        if (!pos) return -1;
        for (u32 v = 0; v < ma->cpu.vertex_count; ++v) pos[v] = ma->cpu.vertices[v].position;
        b32 ok = a3_trimesh_build(&mc->tm, pos, ma->cpu.indices, ma->cpu.index_count, world);
        a3_free(pos);
        mc->hash = h;
        return ok ? (i32)i : -1;
    }
    MeshCache mc;
    a3_zero_struct(&mc);
    mc.guid = guid;
    mc.hash = h;
    mc.frame = pw->frame;
    A3Vec3 *pos = A3_NEW_ARRAY(A3Vec3, ma->cpu.vertex_count, A3_MEM_TEMP);
    if (!pos) return -1;
    for (u32 v = 0; v < ma->cpu.vertex_count; ++v) pos[v] = ma->cpu.vertices[v].position;
    b32 ok = a3_trimesh_build(&mc.tm, pos, ma->cpu.indices, ma->cpu.index_count, world);
    a3_free(pos);
    if (!ok || !a3_array_push(pw->meshes, mc, A3_MEM_PHYSICS)) { a3_trimesh_free(&mc.tm); return -1; }
    return (i32)pw->meshes.count - 1;
}

static void compute_world_inertia(PBody *b) {
    const A3Mat4 *R = &b->shape.rot_m;
    A3Vec3 d = b->inv_inertia_local;
    A3Mat4 m = a3_mat4_identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m.m[j * 4 + i] = R->m[0 * 4 + i] * d.x * R->m[0 * 4 + j] + R->m[1 * 4 + i] * d.y * R->m[1 * 4 + j] + R->m[2 * 4 + i] * d.z * R->m[2 * 4 + j];
    b->inv_inertia_world = m;
}

static A3Vec3 inertia_mul(const A3Mat4 *m, A3Vec3 v) { return a3_mat4_mul_dir(m, v); }

static void fill_shape(A3World *w, A3Entity e, const A3CCollider *c, const A3Mat4 *world, A3ShapeInstance *s, A3Vec3 *center_local) {
    A3Vec3 pos, scale;
    A3Quat rot;
    a3_mat4_decompose(world, &pos, &rot, &scale);
    scale = a3_v3_abs(scale);
    a3_zero_struct(s);
    s->type = (A3ShapeType)A3_CLAMP(c->shape, 0, A3_SHAPE_COUNT - 1);
    s->rot = rot;
    s->rot_m = a3_mat4_from_quat(rot);
    A3Vec3 cl = a3_v3_mul(c->center, scale);
    *center_local = cl;
    s->pos = a3_v3_add(pos, a3_quat_rotate(rot, cl));
    switch (s->type) {
    case A3_SHAPE_BOX: s->half = a3_v3_max(a3_v3_scale(a3_v3_mul(c->size, scale), 0.5f), a3_v3s(0.001f)); break;
    case A3_SHAPE_SPHERE: s->radius = a3_maxf(c->radius * a3_v3_max_comp(scale), 0.001f); break;
    case A3_SHAPE_CAPSULE:
        s->radius = a3_maxf(c->radius * a3_maxf(scale.x, scale.z), 0.001f);
        s->half_height = a3_maxf(c->height * scale.y * 0.5f - s->radius, 0.0f);
        break;
    default: break;
    }
    A3_UNUSED(w); A3_UNUSED(e);
}

static void shape_aabb_inputs(const A3ShapeInstance *s, A3Vec4 *center, A3Vec4 cols[3], A3Vec4 *half) {
    *center = a3_v4_from3(s->pos, 0);
    const A3Mat4 *R = &s->rot_m;
    if (s->type == A3_SHAPE_SPHERE) {
        cols[0] = a3_v4(1, 0, 0, 0); cols[1] = a3_v4(0, 1, 0, 0); cols[2] = a3_v4(0, 0, 1, 0);
        *half = a3_v4(s->radius, s->radius, s->radius, 0);
        return;
    }
    cols[0] = a3_v4(R->m[0], R->m[1], R->m[2], 0);
    cols[1] = a3_v4(R->m[4], R->m[5], R->m[6], 0);
    cols[2] = a3_v4(R->m[8], R->m[9], R->m[10], 0);
    if (s->type == A3_SHAPE_CAPSULE) *half = a3_v4(s->radius, s->half_height + s->radius, s->radius, 0);
    else *half = a3_v4_from3(s->half, 0);
}

static void build_bodies(A3PhysicsWorld *pw) {
    A3World *w = pw->world;
    a3_transform_system_update(w);
    a3_array_clear(pw->bodies);
    a3_hashmap_clear(&pw->body_by_guid);
    A3Vec3 gravity = a3_world_settings(w)->gravity;
    u32 slots = 1;
    u32 types[2] = { A3_T_COLLIDER, A3_T_TRANSFORM };
    A3Query q = a3_query_begin(w, types, 2);
    while (a3_query_next(&q)) {
        A3CCollider *c = (A3CCollider *)q.components[0];
        A3CTransform *t = (A3CTransform *)q.components[1];
        PBody b;
        a3_zero_struct(&b);
        b.e = q.entity;
        b.guid = a3_entity_guid(w, q.entity);
        fill_shape(w, q.entity, c, &t->world, &b.shape, &b.center_local);
        b.trigger = c->is_trigger;
        b.layer = (u32)A3_CLAMP(c->layer, 0, 31);
        b.mask = c->collide_mask;
        b.friction = 0.6f;
        b.restitution = 0.0f;
        b.mesh = -1;
        b.type = A3_BODY_STATIC;
        A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, q.entity, A3_T_RIGIDBODY);
        if (rb) {
            b.friction = rb->friction;
            b.restitution = rb->restitution;
            b.type = rb->type;
        }
        if (b.shape.type == A3_SHAPE_MESH) {
            if (b.type == A3_BODY_DYNAMIC) {
                /* dynamic triangle meshes are not supported: fall back to its bounding box */
                A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(w, q.entity, A3_T_MESH_RENDERER);
                const A3MeshAsset *ma = mr ? a3_assets_mesh_get(a3_assets_mesh_for_renderer(mr)) : 0;
                A3Vec3 sc = a3_v3_one();
                a3_mat4_decompose(&t->world, 0, 0, &sc);
                b.shape.type = A3_SHAPE_BOX;
                b.shape.half = ma ? a3_v3_mul(a3_aabb_extents(ma->bounds), a3_v3_abs(sc)) : a3_v3s(0.5f);
            } else {
                b.mesh = get_trimesh(pw, w, q.entity, &t->world);
                if (b.mesh < 0) continue;
                b.shape.aabb = pw->meshes.data[b.mesh].tm.bounds;
            }
        }
        if (b.type != A3_BODY_STATIC && rb && !b.trigger) {
            b.sleeping = rb->sleeping;
            if (!grow_slots(pw, slots + 1)) break;
            b.slot = slots++;
            u32 s = b.slot;
            if (b.type == A3_BODY_DYNAMIC && !b.sleeping) {
                f32 mass = a3_maxf(rb->mass, 0.001f);
                b.inv_mass = 1.0f / mass;
                A3Vec3 I;
                const A3ShapeInstance *sh = &b.shape;
                if (sh->type == A3_SHAPE_SPHERE) { f32 i = 0.4f * mass * sh->radius * sh->radius; I = a3_v3s(i); }
                else if (sh->type == A3_SHAPE_CAPSULE) {
                    f32 r = sh->radius, h = sh->half_height * 2.0f;
                    f32 iy = 0.5f * mass * r * r, ix = mass * (3.0f * r * r + h * h) / 12.0f + mass * r * r * 0.25f;
                    I = a3_v3(ix, iy, ix);
                } else {
                    A3Vec3 e2 = a3_v3_scale(a3_v3_mul(sh->half, sh->half), 4.0f);
                    I = a3_v3(mass * (e2.y + e2.z) / 12.0f, mass * (e2.x + e2.z) / 12.0f, mass * (e2.x + e2.y) / 12.0f);
                }
                b.inv_inertia_local = rb->lock_rotation ? a3_v3_zero() : a3_v3(1.0f / I.x, 1.0f / I.y, 1.0f / I.z);
                compute_world_inertia(&b);
                f32 dt = 1.0f / 60.0f;
                pw->accel[s] = a3_v4_from3(a3_v3_scale(gravity, rb->gravity_scale), 0);
                pw->damp[s] = a3_v4(1.0f / (1.0f + dt * rb->linear_damping), 1.0f / (1.0f + dt * rb->angular_damping), 0, 0);
                pw->lin_vel[s] = a3_v4_from3(rb->velocity, 0);
                pw->ang_vel[s] = rb->lock_rotation ? a3_v4(0, 0, 0, 0) : a3_v4_from3(rb->angular_velocity, 0);
            } else {
                /* kinematic or sleeping: infinite mass, keeps its velocity for contacts */
                b.inv_mass = 0;
                b.inv_inertia_world = a3_mat4_scale(a3_v3_zero());
                pw->accel[s] = a3_v4(0, 0, 0, 0);
                pw->damp[s] = a3_v4(1, 1, 0, 0);
                pw->lin_vel[s] = b.sleeping ? a3_v4(0, 0, 0, 0) : a3_v4_from3(rb->velocity, 0);
                pw->ang_vel[s] = b.sleeping ? a3_v4(0, 0, 0, 0) : a3_v4_from3(rb->angular_velocity, 0);
            }
            pw->pos[s] = a3_v4_from3(b.shape.pos, 0);
            pw->rot[s] = b.shape.rot;
        } else {
            b.inv_inertia_world = a3_mat4_scale(a3_v3_zero());
        }
        if (!a3_array_push(pw->bodies, b, A3_MEM_PHYSICS)) break;
        u32 bi = pw->bodies.count - 1;
        if (b.slot) pw->slot_body[b.slot] = bi;
        a3_hashmap_put(&pw->body_by_guid, b.guid ? b.guid : 1, bi + 1);
    }
    if (grow_slots(pw, 1)) {
        pw->lin_vel[0] = pw->ang_vel[0] = pw->accel[0] = a3_v4(0, 0, 0, 0);
        pw->damp[0] = a3_v4(1, 1, 0, 0);
        pw->pos[0] = a3_v4(0, 0, 0, 0);
        pw->rot[0] = a3_quat_identity();
    }
    pw->slot_count = slots;
    pw->built = 1;
    pw->built_structure = w->structure_version;
    /* drop mesh caches for removed entities */
    for (u32 i = 0; i < pw->meshes.count;) {
        if (pw->meshes.data[i].frame + 2 < pw->frame) {
            a3_trimesh_free(&pw->meshes.data[i].tm);
            a3_array_remove_swap(pw->meshes, i);
            /* indices shift: force body rebuild next time */
            pw->built = 0;
        } else ++i;
    }
}

static void compute_aabbs(A3PhysicsWorld *pw) {
    u32 n = pw->bodies.count;
    if (!n || !grow_bp(pw, n)) return;
    for (u32 i = 0; i < n; ++i) shape_aabb_inputs(&pw->bodies.data[i].shape, &pw->centers[i], &pw->cols[i * 3], &pw->halves[i]);
    a3_phys_compute_aabbs(pw->centers, pw->cols, pw->halves, pw->mins, pw->maxs, n); /* assembly */
    for (u32 i = 0; i < n; ++i) {
        PBody *b = &pw->bodies.data[i];
        if (b->mesh >= 0) {
            A3Aabb mb = pw->meshes.data[b->mesh].tm.bounds;
            pw->mins[i] = a3_v4_from3(mb.min, 0);
            pw->maxs[i] = a3_v4_from3(mb.max, 0);
        }
        b->shape.aabb = a3_aabb(a3_v4_xyz(pw->mins[i]), a3_v4_xyz(pw->maxs[i]));
    }
}

static void ensure_built(A3PhysicsWorld *pw) {
    if (!pw->built || pw->built_structure != pw->world->structure_version) {
        build_bodies(pw);
        compute_aabbs(pw);
    }
}

/* ======================================================================== */
/* Step                                                                     */
/* ======================================================================== */

static u32 float_key(f32 f) {
    u32 u;
    a3_memcpy(&u, &f, 4);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

static u64 pair_key(u64 ga, u64 gb) { return ga < gb ? a3_hash_combine(ga, gb) | 1 : a3_hash_combine(gb, ga) | 1; }

static void push_event(A3PhysicsWorld *pw, A3ContactEventType type, A3Entity a, A3Entity b, A3Vec3 p, A3Vec3 n, f32 imp) {
    A3ContactEvent ev = { type, a, b, p, n, imp };
    a3_array_push(pw->events, ev, A3_MEM_PHYSICS);
}

static void tangents(A3Vec3 n, A3Vec3 *t1, A3Vec3 *t2) {
    *t1 = a3_absf(n.x) > 0.57735f ? a3_v3_norm(a3_v3(n.y, -n.x, 0)) : a3_v3_norm(a3_v3(0, n.z, -n.y));
    *t2 = a3_v3_cross(n, *t1);
}

static void make_row(A3SolverRow *r, const PBody *a, const PBody *b, A3Vec3 dir, A3Vec3 ra, A3Vec3 rb) {
    a3_zero_struct(r);
    A3Vec3 raxn = a3_v3_cross(ra, dir), rbxn = a3_v3_cross(rb, dir);
    A3Vec3 ia = inertia_mul(&a->inv_inertia_world, raxn), ib = inertia_mul(&b->inv_inertia_world, rbxn);
    r->lin = a3_v4_from3(dir, 0);
    r->ang_a = a3_v4_from3(raxn, 0);
    r->ang_b = a3_v4_from3(rbxn, 0);
    r->ia = a3_v4_from3(ia, 0);
    r->ib = a3_v4_from3(ib, 0);
    r->inv_mass_a = a->inv_mass;
    r->inv_mass_b = b->inv_mass;
    f32 k = a->inv_mass + b->inv_mass + a3_v3_dot(raxn, ia) + a3_v3_dot(rbxn, ib);
    r->eff_mass = k > 1e-9f ? 1.0f / k : 0.0f;
    r->body_a = (i32)a->slot;
    r->body_b = (i32)b->slot;
    r->friction_of = -1;
    r->lo = 0;
    r->hi = 1e30f;
}

static void apply_row_impulse(A3PhysicsWorld *pw, const A3SolverRow *r, f32 imp) {
    if (imp == 0.0f) return;
    A3Vec4 *va = &pw->lin_vel[r->body_a], *vb = &pw->lin_vel[r->body_b];
    A3Vec4 *wa = &pw->ang_vel[r->body_a], *wb = &pw->ang_vel[r->body_b];
    *va = a3_v4_add(*va, a3_v4_scale(r->lin, -imp * r->inv_mass_a));
    *vb = a3_v4_add(*vb, a3_v4_scale(r->lin, imp * r->inv_mass_b));
    *wa = a3_v4_add(*wa, a3_v4_scale(r->ia, -imp));
    *wb = a3_v4_add(*wb, a3_v4_scale(r->ib, imp));
}

static A3Vec3 slot_vel_at(A3PhysicsWorld *pw, u32 slot, A3Vec3 r) {
    return a3_v3_add(a3_v4_xyz(pw->lin_vel[slot]), a3_v3_cross(a3_v4_xyz(pw->ang_vel[slot]), r));
}

static CachedPair *cache_get(A3PhysicsWorld *pw, u64 key, b32 create) {
    u64 idx;
    if (a3_hashmap_get(&pw->cache_map, key, &idx)) return &pw->cache.data[idx - 1];
    if (!create) return 0;
    CachedPair cp;
    a3_zero_struct(&cp);
    cp.key = key;
    cp.frame = 0xFFFFFFFFu; /* marks "new this frame" */
    if (!a3_array_push(pw->cache, cp, A3_MEM_PHYSICS)) return 0;
    a3_hashmap_put(&pw->cache_map, key, pw->cache.count);
    return &a3_array_last(pw->cache);
}

void a3_physics_step(A3World *w, f32 dt) {
    A3PhysicsWorld *pw = get_pw(w, 1);
    if (!pw || dt <= 0) return;
    u64 t0 = a3_time_ns();
    pw->frame++;
    a3_array_clear(pw->events);
    build_bodies(pw);
    compute_aabbs(pw);
    u32 n = pw->bodies.count;
    a3_zero_struct(&pw->stats);
    pw->stats.bodies = n;
    for (u32 i = 0; i < n; ++i) {
        if (pw->bodies.data[i].type == A3_BODY_DYNAMIC) {
            pw->stats.dynamic_bodies++;
            if (pw->bodies.data[i].sleeping) pw->stats.sleeping_bodies++;
        }
    }
    /* recompute damping for the actual dt */
    for (u32 i = 0; i < n; ++i) {
        PBody *b = &pw->bodies.data[i];
        if (!b->slot || b->type != A3_BODY_DYNAMIC || b->sleeping) continue;
        A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, b->e, A3_T_RIGIDBODY);
        if (rb) pw->damp[b->slot] = a3_v4(1.0f / (1.0f + dt * rb->linear_damping), 1.0f / (1.0f + dt * rb->angular_damping), 0, 0);
    }

    /* 1. integrate velocities (assembly) */
    if (pw->slot_count > 1) a3_phys_integrate_velocities(pw->lin_vel + 1, pw->ang_vel + 1, pw->accel + 1, pw->damp + 1, dt, pw->slot_count - 1);

    /* 2. broadphase: sort by min.x, sweep and prune (assembly) */
    u64 tb = a3_time_ns();
    u32 npairs = 0;
    if (n > 1) {
        for (u32 i = 0; i < n; ++i) { pw->sort[i].key = float_key(pw->mins[i].x); pw->sort[i].value = i; }
        a3_radix_sort_pairs(pw->sort, pw->sort_tmp, n);
        for (u32 i = 0; i < n; ++i) { pw->smin[i] = pw->mins[pw->sort[i].value]; pw->smax[i] = pw->maxs[pw->sort[i].value]; }
        for (;;) {
            if (!pw->pair_cap) { pw->pair_cap = 1024; pw->pairs = A3_NEW_ARRAY(u32, pw->pair_cap * 2, A3_MEM_PHYSICS); if (!pw->pairs) { pw->pair_cap = 0; break; } }
            npairs = a3_phys_sap_pairs(pw->smin, pw->smax, n, pw->pairs, pw->pair_cap);
            if (npairs < pw->pair_cap) break;
            u32 nc = pw->pair_cap * 2;
            u32 *np_ = (u32 *)a3_realloc(pw->pairs, sizeof(u32) * nc * 2, A3_MEM_PHYSICS);
            if (!np_) break;
            pw->pairs = np_;
            pw->pair_cap = nc;
        }
    }
    pw->stats.broadphase_pairs = npairs;
    pw->stats.broadphase_ms = (f64)(a3_time_ns() - tb) / 1e6;

    /* 3. narrowphase -> manifolds -> solver rows */
    u64 tn = a3_time_ns();
    a3_array_clear(pw->rows);
    a3_array_clear(pw->refs);
    f32 inv_dt = 1.0f / dt;
    for (u32 p = 0; p < npairs; ++p) {
        u32 ia = pw->sort[pw->pairs[p * 2]].value, ib = pw->sort[pw->pairs[p * 2 + 1]].value;
        PBody *a = &pw->bodies.data[ia], *b = &pw->bodies.data[ib];
        b32 a_moves = a->slot && a->inv_mass > 0, b_moves = b->slot && b->inv_mass > 0;
        b32 any_trigger = a->trigger || b->trigger;
        if (!a_moves && !b_moves && !any_trigger && !(a->slot || b->slot)) continue; /* static-static */
        if (!any_trigger && !a_moves && !b_moves && !(a->sleeping || b->sleeping)) {
            /* kinematic vs static/kinematic: no response needed */
            if (!(a->type == A3_BODY_KINEMATIC || b->type == A3_BODY_KINEMATIC) || (!a->slot && !b->slot)) continue;
        }
        if (!((a->mask >> b->layer) & 1u) || !((b->mask >> a->layer) & 1u)) continue;
        if (a->mesh >= 0 && b->mesh >= 0) continue;
        if (a->sleeping && b->sleeping) continue;
        /* canonical order: mesh (static) first; otherwise keep */
        if (b->mesh >= 0) { PBody *t = a; a = b; b = t; u32 ti = ia; ia = ib; ib = ti; }
        A3Manifold m;
        b32 hit = a->mesh >= 0 ? a3_collide_mesh(&pw->meshes.data[a->mesh].tm, &b->shape, &m) : a3_collide_convex(&a->shape, &b->shape, &m);
        if (!hit || !m.count) continue;
        u64 key = pair_key(a->guid, b->guid);
        CachedPair *cp = cache_get(pw, key, 1);
        if (!cp) continue;
        b32 is_new = cp->frame != pw->frame - 1 && cp->frame != pw->frame;
        cp->a = ia; cp->b = ib;
        cp->ea = a->e; cp->eb = b->e;
        cp->trigger = any_trigger;
        cp->point = m.pts[0].pos;
        cp->normal = m.normal;
        if (any_trigger) {
            if (is_new) {
                PBody *trg = a->trigger ? a : b, *oth = a->trigger ? b : a;
                push_event(pw, A3_TRIGGER_ENTER, trg->e, oth->e, m.pts[0].pos, m.normal, 0);
            }
            cp->frame = pw->frame;
            continue;
        }
        /* wake sleeping bodies touched by moving ones */
        if (a->sleeping && b_moves) { A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, a->e, A3_T_RIGIDBODY); if (rb) rb->sleeping = 0; }
        if (b->sleeping && a_moves) { A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, b->e, A3_T_RIGIDBODY); if (rb) rb->sleeping = 0; }
        /* warm start data from the previous frame, matched by local position */
        f32 prev_n[CACHE_POINTS] = { 0 }, prev_t1[CACHE_POINTS] = { 0 }, prev_t2[CACHE_POINTS] = { 0 };
        A3Vec3 prev_local[CACHE_POINTS];
        u32 prev_count = is_new ? 0 : cp->count;
        for (u32 k = 0; k < prev_count; ++k) { prev_n[k] = cp->acc_n[k]; prev_t1[k] = cp->acc_t1[k]; prev_t2[k] = cp->acc_t2[k]; prev_local[k] = cp->local_a[k]; }
        if (is_new) push_event(pw, A3_CONTACT_BEGIN, a->e, b->e, m.pts[0].pos, m.normal, 0);
        cp->frame = pw->frame;
        cp->count = m.count;
        f32 mu = a3_sqrtf(a3_maxf(a->friction, 0) * a3_maxf(b->friction, 0));
        f32 e = a3_maxf(a->restitution, b->restitution);
        A3Vec3 t1, t2;
        tangents(m.normal, &t1, &t2);
        A3Quat inv_ra = a3_quat_conjugate(a->shape.rot);
        for (u32 k = 0; k < m.count; ++k) {
            A3Vec3 pt = m.pts[k].pos;
            A3Vec3 ra = a3_v3_sub(pt, a->shape.pos), rb = a3_v3_sub(pt, b->shape.pos);
            A3Vec3 local = a3_quat_rotate(inv_ra, ra);
            cp->local_a[k] = local;
            cp->acc_n[k] = cp->acc_t1[k] = cp->acc_t2[k] = 0;
            f32 wn = 0, wt1 = 0, wt2 = 0;
            for (u32 j = 0; j < prev_count; ++j) {
                if (a3_v3_dist_sq(prev_local[j], local) < 0.02f * 0.02f) { wn = prev_n[j]; wt1 = prev_t1[j]; wt2 = prev_t2[j]; break; }
            }
            u32 base = pw->rows.count;
            A3SolverRow rn, rf1, rf2;
            make_row(&rn, a, b, m.normal, ra, rb);
            f32 vn = a3_v3_dot(a3_v3_sub(slot_vel_at(pw, b->slot, rb), slot_vel_at(pw, a->slot, ra)), m.normal);
            f32 bounce = vn < -1.0f ? e * vn : 0.0f;
            f32 pen_bias = -pw->settings.baumgarte * inv_dt * a3_maxf(m.pts[k].depth - pw->settings.slop, 0.0f);
            rn.bias = a3_minf(bounce, 0.0f) + pen_bias;
            if (bounce < pen_bias) rn.bias = bounce; /* bounce and push-out: use the stronger */
            rn.acc = wn;
            make_row(&rf1, a, b, t1, ra, rb);
            make_row(&rf2, a, b, t2, ra, rb);
            rf1.friction_of = rf2.friction_of = (i32)base;
            rf1.mu = rf2.mu = mu;
            rf1.acc = wt1;
            rf2.acc = wt2;
            a3_array_push(pw->rows, rn, A3_MEM_PHYSICS);
            a3_array_push(pw->rows, rf1, A3_MEM_PHYSICS);
            a3_array_push(pw->rows, rf2, A3_MEM_PHYSICS);
            ContactRef ref = { (u32)(cp - pw->cache.data), k, base };
            a3_array_push(pw->refs, ref, A3_MEM_PHYSICS);
            pw->stats.contacts++;
        }
    }
    pw->stats.narrowphase_ms = (f64)(a3_time_ns() - tn) / 1e6;
    pw->stats.solver_rows = pw->rows.count;

    /* 4. warm start + solve (assembly) */
    u64 ts = a3_time_ns();
    for (u32 i = 0; i < pw->rows.count; ++i) apply_row_impulse(pw, &pw->rows.data[i], pw->rows.data[i].acc * 0.85f);
    for (u32 i = 0; i < pw->rows.count; ++i) pw->rows.data[i].acc *= 0.85f;
    for (i32 it = 0; it < pw->settings.solver_iterations && pw->rows.count; ++it)
        a3_phys_solve_rows(pw->rows.data, pw->rows.count, pw->lin_vel, pw->ang_vel);
    for (u32 i = 0; i < pw->refs.count; ++i) {
        ContactRef *r = &pw->refs.data[i];
        CachedPair *cp = &pw->cache.data[r->pair];
        cp->acc_n[r->point] = pw->rows.data[r->row].acc;
        cp->acc_t1[r->point] = pw->rows.data[r->row + 1].acc;
        cp->acc_t2[r->point] = pw->rows.data[r->row + 2].acc;
        if (r->point == 0) cp->impulse = 0;
        cp->impulse += pw->rows.data[r->row].acc;
    }
    pw->stats.solver_ms = (f64)(a3_time_ns() - ts) / 1e6;

    /* keep slot 0 (static world) at rest */
    pw->lin_vel[0] = pw->ang_vel[0] = a3_v4(0, 0, 0, 0);

    /* 5. integrate transforms (assembly) */
    if (pw->slot_count > 1) a3_phys_integrate_transforms(pw->pos + 1, pw->rot + 1, pw->lin_vel + 1, pw->ang_vel + 1, dt, pw->slot_count - 1);

    /* 6. write back to the ECS */
    for (u32 s = 1; s < pw->slot_count; ++s) {
        PBody *b = &pw->bodies.data[pw->slot_body[s]];
        A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, b->e, A3_T_RIGIDBODY);
        A3CTransform *t = (A3CTransform *)a3_component_get(w, b->e, A3_T_TRANSFORM);
        if (!rb || !t || b->sleeping) continue;
        A3Vec3 v = a3_v4_xyz(pw->lin_vel[s]), av = a3_v4_xyz(pw->ang_vel[s]);
        rb->velocity = v;
        rb->angular_velocity = av;
        A3Quat rot = pw->rot[s];
        if (rb->lock_rotation) rot = b->shape.rot;
        A3Vec3 pos = a3_v3_sub(a3_v4_xyz(pw->pos[s]), a3_quat_rotate(rot, b->center_local));
        A3Entity parent = a3_entity_parent(w, b->e);
        if (a3_entity_valid(w, parent)) {
            A3Mat4 pm = a3_transform_compute_world(w, parent), inv;
            if (a3_mat4_inverse(&pm, &inv)) {
                pos = a3_mat4_mul_point(&inv, pos);
                A3Quat pr = a3_transform_world_rotation(w, parent);
                rot = a3_quat_mul(a3_quat_conjugate(pr), rot);
            }
        }
        t->position = pos;
        t->rotation = a3_quat_normalize(rot);
        b->shape.pos = a3_v4_xyz(pw->pos[s]);
        b->shape.rot = pw->rot[s];
        /* sleeping */
        if (b->type == A3_BODY_DYNAMIC && pw->settings.sleeping) {
            f32 sv = pw->settings.sleep_velocity;
            if (a3_v3_len_sq(v) < sv * sv && a3_v3_len_sq(av) < sv * sv * 4.0f) {
                rb->sleep_timer += dt;
                if (rb->sleep_timer > pw->settings.sleep_time) { rb->sleeping = 1; rb->velocity = rb->angular_velocity = a3_v3_zero(); }
            } else {
                rb->sleep_timer = 0;
            }
        }
    }

    /* 7. end events for pairs not touching anymore */
    for (u32 i = 0; i < pw->cache.count;) {
        CachedPair *cp = &pw->cache.data[i];
        if (cp->frame != pw->frame) {
            push_event(pw, cp->trigger ? A3_TRIGGER_EXIT : A3_CONTACT_END, cp->ea, cp->eb, cp->point, cp->normal, 0);
            a3_hashmap_remove(&pw->cache_map, cp->key);
            u32 last = pw->cache.count - 1;
            if (i != last) {
                pw->cache.data[i] = pw->cache.data[last];
                a3_hashmap_put(&pw->cache_map, pw->cache.data[i].key, i + 1);
            }
            pw->cache.count--;
        } else ++i;
    }
    pw->built_structure = w->structure_version;
    pw->stats.step_ms = (f64)(a3_time_ns() - t0) / 1e6;
}

/* ======================================================================== */
/* Queries                                                                  */
/* ======================================================================== */

u32 a3_physics_events(A3World *w, const A3ContactEvent **out) {
    A3PhysicsWorld *pw = get_pw(w, 0);
    if (!pw) { if (out) *out = 0; return 0; }
    if (out) *out = pw->events.data;
    return pw->events.count;
}

b32 a3_physics_raycast_ignore(A3World *w, A3Vec3 origin, A3Vec3 dir, f32 max_dist, u32 mask, A3Entity ignore, A3RaycastHit *hit) {
    A3PhysicsWorld *pw = get_pw(w, 1);
    if (!pw) return 0;
    ensure_built(pw);
    compute_aabbs(pw);
    u32 n = pw->bodies.count;
    if (!n) return 0;
    dir = a3_v3_norm(dir);
    if (a3_v3_len_sq(dir) < 0.5f) return 0;
    A3Vec4 o = a3_v4_from3(origin, 0);
    A3Vec4 inv = a3_v4(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z, 0);
    f32 *ts = A3_NEW_ARRAY(f32, n, A3_MEM_TEMP);
    if (!ts) return 0;
    a3_phys_ray_aabbs(&o, &inv, max_dist, pw->mins, pw->maxs, n, ts); /* assembly broadphase */
    f32 best = max_dist;
    b32 found = 0;
    for (u32 i = 0; i < n; ++i) {
        if (ts[i] < 0 || ts[i] > best) continue;
        PBody *b = &pw->bodies.data[i];
        if (b->trigger || !((mask >> b->layer) & 1u) || a3_entity_eq(b->e, ignore)) continue;
        f32 t;
        A3Vec3 nrm;
        b32 h = b->mesh >= 0 ? a3_trimesh_raycast(&pw->meshes.data[b->mesh].tm, origin, dir, best, &t, &nrm)
                             : a3_raycast_shape(&b->shape, origin, dir, best, &t, &nrm);
        if (h && t <= best) {
            best = t;
            found = 1;
            if (hit) { hit->entity = b->e; hit->distance = t; hit->point = a3_v3_madd(origin, dir, t); hit->normal = nrm; }
        }
    }
    a3_free(ts);
    return found;
}

b32 a3_physics_raycast(A3World *w, A3Vec3 origin, A3Vec3 dir, f32 max_dist, u32 mask, A3RaycastHit *hit) {
    return a3_physics_raycast_ignore(w, origin, dir, max_dist, mask, A3_ENTITY_NULL, hit);
}

u32 a3_physics_overlap_sphere(A3World *w, A3Vec3 c, f32 r, u32 mask, A3Entity *out, u32 max_out) {
    A3PhysicsWorld *pw = get_pw(w, 1);
    if (!pw) return 0;
    ensure_built(pw);
    A3ShapeInstance s;
    a3_zero_struct(&s);
    s.type = A3_SHAPE_SPHERE;
    s.pos = c;
    s.radius = r;
    s.rot = a3_quat_identity();
    s.rot_m = a3_mat4_identity();
    s.aabb = a3_aabb(a3_v3_sub(c, a3_v3s(r)), a3_v3_add(c, a3_v3s(r)));
    u32 count = 0;
    for (u32 i = 0; i < pw->bodies.count && count < max_out; ++i) {
        PBody *b = &pw->bodies.data[i];
        if (!((mask >> b->layer) & 1u) || !a3_aabb_overlap(b->shape.aabb, s.aabb)) continue;
        A3Manifold m;
        b32 h = b->mesh >= 0 ? a3_collide_mesh(&pw->meshes.data[b->mesh].tm, &s, &m) : a3_collide_convex(&b->shape, &s, &m);
        if (h) out[count++] = b->e;
    }
    return count;
}

static A3CRigidBody *dyn_body(A3World *w, A3Entity e) {
    A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, e, A3_T_RIGIDBODY);
    return rb && rb->type == A3_BODY_DYNAMIC ? rb : 0;
}

void a3_physics_add_impulse(A3World *w, A3Entity e, A3Vec3 impulse) {
    A3CRigidBody *rb = dyn_body(w, e);
    if (!rb) return;
    rb->velocity = a3_v3_madd(rb->velocity, impulse, 1.0f / a3_maxf(rb->mass, 0.001f));
    rb->sleeping = 0;
    rb->sleep_timer = 0;
}

void a3_physics_set_velocity(A3World *w, A3Entity e, A3Vec3 v) {
    A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, e, A3_T_RIGIDBODY);
    if (!rb) return;
    rb->velocity = v;
    rb->sleeping = 0;
    rb->sleep_timer = 0;
}

void a3_physics_wake(A3World *w, A3Entity e) {
    A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, e, A3_T_RIGIDBODY);
    if (rb) { rb->sleeping = 0; rb->sleep_timer = 0; }
}

b32 a3_physics_collider_shape(A3World *w, A3Entity e, A3ShapeInstance *out) {
    A3CCollider *c = (A3CCollider *)a3_component_get(w, e, A3_T_COLLIDER);
    if (!c || !out) return 0;
    A3Mat4 world = a3_transform_compute_world(w, e);
    A3Vec3 cl;
    fill_shape(w, e, c, &world, out, &cl);
    A3Vec4 center, cols[3], half, mn, mx;
    shape_aabb_inputs(out, &center, cols, &half);
    a3_phys_compute_aabbs(&center, cols, &half, &mn, &mx, 1);
    out->aabb = a3_aabb(a3_v4_xyz(mn), a3_v4_xyz(mx));
    return 1;
}

/* Exposed for the character controller: iterate colliders near a box. */
typedef b32 (*A3PhysBodyVisit)(const A3ShapeInstance *shape, const A3TriMesh *mesh, A3Entity e, b32 dynamic, void *user);
void a3__physics_visit_near(A3World *w, A3Aabb box, A3Entity ignore, A3PhysBodyVisit fn, void *user);
void a3__physics_visit_near(A3World *w, A3Aabb box, A3Entity ignore, A3PhysBodyVisit fn, void *user) {
    A3PhysicsWorld *pw = get_pw(w, 1);
    if (!pw) return;
    ensure_built(pw);
    for (u32 i = 0; i < pw->bodies.count; ++i) {
        PBody *b = &pw->bodies.data[i];
        if (b->trigger || a3_entity_eq(b->e, ignore) || !a3_aabb_overlap(b->shape.aabb, box)) continue;
        if (!fn(&b->shape, b->mesh >= 0 ? &pw->meshes.data[b->mesh].tm : 0, b->e, b->type == A3_BODY_DYNAMIC, user)) return;
    }
}
