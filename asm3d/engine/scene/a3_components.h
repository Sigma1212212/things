/*
 * ASM3D - a3_components.h
 * Core built-in components and the transform system.
 * Subsystem components (physics, audio, AI, ...) are declared in their own
 * modules and registered by a3_engine_register_components().
 */
#ifndef A3_COMPONENTS_H
#define A3_COMPONENTS_H

#include "../ecs/a3_ecs.h"

A3_EXTERN_C_BEGIN

/* Component type ids, valid after a3_register_core_components(). */
extern u32 A3_T_TRANSFORM;
extern u32 A3_T_CAMERA;
extern u32 A3_T_LIGHT;
extern u32 A3_T_MESH_RENDERER;
extern u32 A3_T_WORLD_SETTINGS;

typedef struct A3CTransform {
    A3Vec3 position; f32 _pad0;
    A3Quat rotation;
    A3Vec3 scale; f32 _pad1;
    A3Mat4 world;          /* computed by a3_transform_system_update */
} A3CTransform;

typedef enum A3Projection { A3_PROJ_PERSPECTIVE = 0, A3_PROJ_ORTHOGRAPHIC } A3Projection;
typedef struct A3CCamera {
    f32 fov;               /* vertical field of view, degrees */
    f32 near_plane;
    f32 far_plane;
    b32 primary;           /* the camera used to render the game view */
    i32 projection;        /* A3Projection */
    f32 ortho_size;
    f32 _pad[2];
    A3Vec4 clear_color;
    b32 use_sky;
    f32 exposure;
} A3CCamera;

typedef enum A3LightType { A3_LIGHT_DIRECTIONAL = 0, A3_LIGHT_POINT, A3_LIGHT_SPOT } A3LightType;
typedef struct A3CLight {
    i32 type;              /* A3LightType */
    f32 intensity;
    f32 range;
    f32 spot_angle;        /* degrees, full cone */
    A3Vec4 color;
    b32 cast_shadows;
    f32 _pad[3];
} A3CLight;

typedef enum A3Primitive {
    A3_PRIM_NONE = 0, A3_PRIM_CUBE, A3_PRIM_SPHERE, A3_PRIM_PLANE, A3_PRIM_CYLINDER, A3_PRIM_CAPSULE, A3_PRIM_CONE,
    A3_PRIM_COUNT
} A3Primitive;

typedef struct A3CMeshRenderer {
    i32 primitive;         /* A3Primitive; used when mesh is empty */
    b32 cast_shadows;
    b32 receive_shadows;
    b32 visible;
    A3Vec4 base_color;     /* simple material (Beginner Mode) */
    f32 metallic;
    f32 roughness;
    f32 emissive;
    f32 _pad;
    A3AssetRef mesh;       /* optional mesh asset */
    A3AssetRef material;   /* optional material asset (overrides simple material) */
    A3AssetRef texture;    /* optional albedo texture for the simple material */
} A3CMeshRenderer;

typedef struct A3CWorldSettings {
    A3Vec4 sky_top;
    A3Vec4 sky_horizon;
    A3Vec4 ground_color;
    A3Vec4 ambient;
    A3Vec4 fog_color;
    f32 fog_density;
    f32 ambient_intensity;
    f32 time_of_day;       /* hours 0..24, used by the day/night system */
    b32 day_night_cycle;
    A3Vec3 gravity;
    f32 day_length_minutes;
} A3CWorldSettings;

void a3_register_core_components(void);

/* ---- Transform system ---- */
void a3_transform_system_update(A3World *w);
A3CTransform *a3_transform(A3World *w, A3Entity e);          /* NULL if absent */
A3Vec3 a3_transform_world_position(A3World *w, A3Entity e);
A3Quat a3_transform_world_rotation(A3World *w, A3Entity e);
/* Sets the local transform so that the entity ends up at the given world position. */
void   a3_transform_set_world_position(A3World *w, A3Entity e, A3Vec3 p);
A3Vec3 a3_transform_world_forward(A3World *w, A3Entity e);
/* Computes the world matrix of e immediately (walks parents). */
A3Mat4 a3_transform_compute_world(A3World *w, A3Entity e);

/* Finds the primary camera (or the first camera). */
A3Entity a3_find_primary_camera(A3World *w);
/* Returns the WorldSettings of the world (creates a hidden entity if missing). */
A3CWorldSettings *a3_world_settings(A3World *w);

A3_EXTERN_C_END

#endif
