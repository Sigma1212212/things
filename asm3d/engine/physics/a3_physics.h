/*
 * ASM3D - a3_physics.h
 * Rigid body physics: boxes, spheres, capsules and static triangle meshes,
 * sequential-impulse solver with friction, restitution and warm starting,
 * triggers, contact events, raycasts and overlap queries. Fixed timestep.
 * Hot loops run in x86-64 assembly (see a3_physics_kernels.h).
 */
#ifndef A3_PHYSICS_H
#define A3_PHYSICS_H

#include "../ecs/a3_ecs.h"

A3_EXTERN_C_BEGIN

extern u32 A3_T_RIGIDBODY;
extern u32 A3_T_COLLIDER;
extern u32 A3_T_CHARACTER;

typedef enum A3BodyType { A3_BODY_STATIC = 0, A3_BODY_DYNAMIC, A3_BODY_KINEMATIC } A3BodyType;
typedef enum A3ShapeType { A3_SHAPE_BOX = 0, A3_SHAPE_SPHERE, A3_SHAPE_CAPSULE, A3_SHAPE_MESH, A3_SHAPE_COUNT } A3ShapeType;

typedef struct A3CRigidBody {
    i32 type;               /* A3BodyType */
    f32 mass;               /* kg */
    f32 friction;
    f32 restitution;        /* bounciness 0..1 */
    f32 linear_damping;
    f32 angular_damping;
    f32 gravity_scale;
    b32 lock_rotation;      /* keep upright (players, NPC bodies) */
    A3Vec3 velocity;        /* m/s (initial velocity in the editor) */
    f32 _pad0;
    A3Vec3 angular_velocity;
    f32 _pad1;
    b32 sleeping;           /* runtime */
    f32 sleep_timer;        /* runtime */
    f32 _pad2[2];
} A3CRigidBody;

typedef struct A3CCollider {
    i32 shape;              /* A3ShapeType */
    b32 is_trigger;         /* detects overlaps without blocking */
    f32 radius;             /* sphere / capsule */
    f32 height;             /* capsule total height */
    A3Vec3 size;            /* box full size */
    f32 _pad0;
    A3Vec3 center;          /* offset from the entity origin (local space) */
    i32 layer;              /* 0..31 */
    u32 collide_mask;       /* layers this collider interacts with */
    f32 _pad1[3];
} A3CCollider;

typedef enum A3CameraMode { A3_CAM_NONE = 0, A3_CAM_FIRST_PERSON, A3_CAM_THIRD_PERSON } A3CameraMode;

typedef struct A3CCharacterController {
    f32 height;
    f32 radius;
    f32 step_height;
    f32 slope_limit;        /* degrees */
    f32 walk_speed;
    f32 sprint_speed;
    f32 jump_height;
    f32 gravity_scale;
    f32 air_control;        /* 0..1 */
    f32 acceleration;
    i32 camera_mode;        /* A3CameraMode */
    f32 mouse_sensitivity;
    f32 camera_distance;    /* third person */
    f32 eye_height;         /* first person, from the feet */
    b32 use_input;          /* driven by the move/jump actions */
    b32 grounded;           /* runtime */
    A3Vec3 velocity;        /* runtime */
    f32 yaw;                /* runtime, radians */
    f32 pitch;              /* runtime, radians */
    f32 jump_buffer;        /* runtime: seconds a jump request stays valid */
    f32 _pad[2];
} A3CCharacterController;

typedef struct A3RaycastHit {
    A3Entity entity;
    A3Vec3 point;
    A3Vec3 normal;
    f32 distance;
} A3RaycastHit;

typedef enum A3ContactEventType {
    A3_CONTACT_BEGIN = 0, A3_CONTACT_END, A3_TRIGGER_ENTER, A3_TRIGGER_EXIT
} A3ContactEventType;

typedef struct A3ContactEvent {
    A3ContactEventType type;
    A3Entity a, b;          /* for triggers: a = trigger, b = other */
    A3Vec3 point;
    A3Vec3 normal;
    f32 impulse;
} A3ContactEvent;

typedef struct A3PhysicsSettings {
    i32 solver_iterations;
    f32 baumgarte;          /* penetration correction strength */
    f32 slop;               /* allowed penetration (m) */
    b32 sleeping;
    f32 sleep_velocity;
    f32 sleep_time;
} A3PhysicsSettings;

typedef struct A3PhysicsStats {
    u32 bodies, dynamic_bodies, sleeping_bodies;
    u32 broadphase_pairs, contacts, solver_rows;
    f64 step_ms, broadphase_ms, narrowphase_ms, solver_ms;
} A3PhysicsStats;

void a3_physics_register(void);

/* Per-world physics state is created on demand. */
A3PhysicsSettings *a3_physics_settings(A3World *w);
const A3PhysicsStats *a3_physics_stats(A3World *w);
void a3_physics_release(A3World *w);
void a3_physics_step(A3World *w, f32 dt);

b32 a3_physics_raycast(A3World *w, A3Vec3 origin, A3Vec3 dir, f32 max_dist, u32 layer_mask, A3RaycastHit *hit);
/* Like raycast but ignores one entity (e.g. the shooter). */
b32 a3_physics_raycast_ignore(A3World *w, A3Vec3 origin, A3Vec3 dir, f32 max_dist, u32 layer_mask, A3Entity ignore, A3RaycastHit *hit);
u32 a3_physics_overlap_sphere(A3World *w, A3Vec3 center, f32 radius, u32 layer_mask, A3Entity *out, u32 max_out);
u32 a3_physics_events(A3World *w, const A3ContactEvent **out);

void a3_physics_add_impulse(A3World *w, A3Entity e, A3Vec3 impulse);
void a3_physics_set_velocity(A3World *w, A3Entity e, A3Vec3 velocity);
void a3_physics_wake(A3World *w, A3Entity e);

/* Collider world-space shape used by the character controller and editor. */
typedef struct A3ShapeInstance {
    A3ShapeType type;
    A3Vec3 pos;             /* world center */
    A3Quat rot;
    A3Mat4 rot_m;           /* rotation only */
    A3Vec3 half;            /* box half extents */
    f32 radius;
    f32 half_height;        /* capsule segment half length (excl. caps) */
    u32 mesh_bvh;           /* static mesh collision data id */
    A3Aabb aabb;
} A3ShapeInstance;

b32 a3_physics_collider_shape(A3World *w, A3Entity e, A3ShapeInstance *out);
/* Debug drawing of all colliders (editor overlay). */
struct A3Renderer;
void a3_physics_debug_draw(A3World *w, struct A3Renderer *r);

A3_EXTERN_C_END

#endif
