/*
 * ASM3D - a3_components.c
 */
#include "a3_components.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"

u32 A3_T_TRANSFORM = 0xFFFFFFFFu;
u32 A3_T_CAMERA = 0xFFFFFFFFu;
u32 A3_T_LIGHT = 0xFFFFFFFFu;
u32 A3_T_MESH_RENDERER = 0xFFFFFFFFu;
u32 A3_T_WORLD_SETTINGS = 0xFFFFFFFFu;

static const char *const g_projection_names[] = { "Perspective", "Orthographic" };
static const char *const g_light_names[] = { "Directional", "Point", "Spot" };
static const char *const g_prim_names[] = { "None (use Mesh)", "Cube", "Sphere", "Plane", "Cylinder", "Capsule", "Cone" };

static void set_enum(A3FieldDesc *f, const char *const *names, u32 count) {
    if (!f) return;
    f->enum_names = names;
    f->enum_count = count;
}

void a3_register_core_components(void) {
    if (A3_T_TRANSFORM != 0xFFFFFFFFu) return;

    /* Transform */
    A3CTransform td;
    a3_zero_struct(&td);
    td.rotation = a3_quat_identity();
    td.scale = a3_v3_one();
    td.world = a3_mat4_identity();
    u32 t = a3_component_register("Transform", "Core", sizeof(A3CTransform), 16, &td, A3_COMP_BUILTIN,
        "Position, rotation and scale of an entity. Children move with their parent.");
    A3_T_TRANSFORM = t;
    a3_component_type(t)->icon = "move";
    A3_REFLECT_FIELD(t, A3CTransform, position, A3_FIELD_VEC3, "Position", "Location in meters, relative to the parent.");
    A3_REFLECT_FIELD(t, A3CTransform, rotation, A3_FIELD_QUAT, "Rotation", "Rotation in degrees (pitch, yaw, roll), relative to the parent.");
    A3_REFLECT_FIELD(t, A3CTransform, scale, A3_FIELD_VEC3, "Scale", "Size multiplier on each axis. 1 = original size.");

    /* Camera */
    A3CCamera cd;
    a3_zero_struct(&cd);
    cd.fov = 70.0f; cd.near_plane = 0.1f; cd.far_plane = 1000.0f; cd.primary = 1;
    cd.ortho_size = 10.0f; cd.clear_color = a3_v4(0.1f, 0.12f, 0.15f, 1.0f); cd.use_sky = 1; cd.exposure = 1.0f;
    t = a3_component_register("Camera", "Rendering", sizeof(A3CCamera), 16, &cd, A3_COMP_BUILTIN,
        "Shows the world from this entity's point of view. The Primary camera is used when you press Play.");
    A3_T_CAMERA = t;
    a3_component_type(t)->icon = "camera";
    a3_component_require(t, "Transform");
    a3_field_range(A3_REFLECT_FIELD(t, A3CCamera, fov, A3_FIELD_F32, "Field of View", "How wide the camera sees, in degrees. 60-90 is typical."), 10, 150, 1);
    a3_field_range(A3_REFLECT_FIELD(t, A3CCamera, near_plane, A3_FIELD_F32, "Near Clip", "Closest distance that is drawn."), 0.01f, 10, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CCamera, far_plane, A3_FIELD_F32, "Far Clip", "Farthest distance that is drawn."), 1, 100000, 10)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CCamera, primary, A3_FIELD_BOOL, "Primary", "Use this camera for the game view.");
    set_enum(A3_REFLECT_FIELD(t, A3CCamera, projection, A3_FIELD_ENUM, "Projection", "Perspective looks natural; Orthographic has no depth distortion (2D, strategy)."), g_projection_names, 2);
    A3_REFLECT_FIELD(t, A3CCamera, ortho_size, A3_FIELD_F32, "Ortho Size", "Half-height of the view in meters (orthographic only).")->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CCamera, use_sky, A3_FIELD_BOOL, "Draw Sky", "Draw the sky behind everything. Otherwise the clear color is used.");
    A3_REFLECT_FIELD(t, A3CCamera, clear_color, A3_FIELD_COLOR, "Background", "Background color when the sky is off.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CCamera, exposure, A3_FIELD_F32, "Exposure", "Brightness of the final image."), 0.05f, 8, 0.05f)->flags |= A3_FIELD_FLAG_ADVANCED;

    /* Light */
    A3CLight ld;
    a3_zero_struct(&ld);
    ld.type = A3_LIGHT_POINT; ld.intensity = 1.0f; ld.range = 10.0f; ld.spot_angle = 45.0f;
    ld.color = a3_v4(1, 1, 1, 1); ld.cast_shadows = 0;
    t = a3_component_register("Light", "Rendering", sizeof(A3CLight), 16, &ld, A3_COMP_BUILTIN,
        "Illuminates the scene. Directional = sun, Point = bulb, Spot = flashlight.");
    A3_T_LIGHT = t;
    a3_component_type(t)->icon = "light";
    a3_component_require(t, "Transform");
    set_enum(A3_REFLECT_FIELD(t, A3CLight, type, A3_FIELD_ENUM, "Type", "Directional lights shine everywhere from one direction (like the sun)."), g_light_names, 3);
    A3_REFLECT_FIELD(t, A3CLight, color, A3_FIELD_COLOR, "Color", "Color of the light.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CLight, intensity, A3_FIELD_F32, "Intensity", "Brightness. 1 = normal."), 0, 100, 0.05f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CLight, range, A3_FIELD_F32, "Range", "How far point and spot lights reach, in meters."), 0.1f, 1000, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CLight, spot_angle, A3_FIELD_F32, "Spot Angle", "Cone width of a spot light, in degrees."), 1, 179, 1);
    A3_REFLECT_FIELD(t, A3CLight, cast_shadows, A3_FIELD_BOOL, "Cast Shadows", "Objects block this light. Shadows cost performance; directional lights only for now.");

    /* MeshRenderer */
    A3CMeshRenderer md;
    a3_zero_struct(&md);
    md.primitive = A3_PRIM_CUBE; md.cast_shadows = 1; md.receive_shadows = 1; md.visible = 1;
    md.base_color = a3_v4(0.8f, 0.8f, 0.8f, 1.0f); md.metallic = 0.0f; md.roughness = 0.6f;
    md.mesh.kind = A3_ASSET_MESH; md.material.kind = A3_ASSET_MATERIAL; md.texture.kind = A3_ASSET_TEXTURE;
    t = a3_component_register("MeshRenderer", "Rendering", sizeof(A3CMeshRenderer), 16, &md, A3_COMP_BUILTIN,
        "Draws a 3D shape. Pick a built-in shape or drag in a model file.");
    A3_T_MESH_RENDERER = t;
    a3_component_type(t)->icon = "cube";
    a3_component_require(t, "Transform");
    set_enum(A3_REFLECT_FIELD(t, A3CMeshRenderer, primitive, A3_FIELD_ENUM, "Shape", "Built-in shape to draw when no Mesh asset is set."), g_prim_names, A3_PRIM_COUNT);
    A3_REFLECT_FIELD(t, A3CMeshRenderer, mesh, A3_FIELD_ASSET, "Mesh", "A model file (.obj). Overrides Shape.")->asset_kind = A3_ASSET_MESH;
    A3_REFLECT_FIELD(t, A3CMeshRenderer, base_color, A3_FIELD_COLOR, "Color", "Surface color.");
    A3_REFLECT_FIELD(t, A3CMeshRenderer, texture, A3_FIELD_ASSET, "Texture", "Image painted on the surface.")->asset_kind = A3_ASSET_TEXTURE;
    a3_field_range(A3_REFLECT_FIELD(t, A3CMeshRenderer, metallic, A3_FIELD_F32, "Metallic", "0 = plastic/wood/stone, 1 = metal."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CMeshRenderer, roughness, A3_FIELD_F32, "Roughness", "0 = mirror-smooth, 1 = matte."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CMeshRenderer, emissive, A3_FIELD_F32, "Glow", "Makes the surface emit light (not lighting others)."), 0, 20, 0.1f);
    A3_REFLECT_FIELD(t, A3CMeshRenderer, material, A3_FIELD_ASSET, "Material", "Custom material (from the Shader Maker). Overrides the simple color settings.")->asset_kind = A3_ASSET_MATERIAL;
    A3_REFLECT_FIELD(t, A3CMeshRenderer, cast_shadows, A3_FIELD_BOOL, "Cast Shadows", "This object blocks light.")->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CMeshRenderer, receive_shadows, A3_FIELD_BOOL, "Receive Shadows", "Shadows fall on this object.")->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CMeshRenderer, visible, A3_FIELD_BOOL, "Visible", "Uncheck to hide the object while keeping it in the scene.");

    /* WorldSettings */
    A3CWorldSettings wd;
    a3_zero_struct(&wd);
    wd.sky_top = a3_v4(0.32f, 0.55f, 0.88f, 1);
    wd.sky_horizon = a3_v4(0.75f, 0.84f, 0.93f, 1);
    wd.ground_color = a3_v4(0.35f, 0.32f, 0.28f, 1);
    wd.ambient = a3_v4(0.55f, 0.6f, 0.7f, 1);
    wd.fog_color = a3_v4(0.72f, 0.8f, 0.9f, 1);
    wd.fog_density = 0.002f;
    wd.ambient_intensity = 0.45f;
    wd.time_of_day = 14.0f;
    wd.gravity = a3_v3(0, -9.81f, 0);
    wd.day_length_minutes = 20.0f;
    wd.bloom_intensity = 0.35f;
    wd.bloom_threshold = 1.2f;
    wd.fog_height_falloff = 0.0f;
    wd.ao_strength = 0.7f;
    wd.reflection_strength = 1.0f;
    wd.exposure = 1.0f;
    wd.saturation = 1.0f;
    wd.contrast = 1.0f;
    wd.tint = a3_v4(1, 1, 1, 1);
    t = a3_component_register("WorldSettings", "World", sizeof(A3CWorldSettings), 16, &wd, A3_COMP_BUILTIN | A3_COMP_UNIQUE,
        "Sky, ambient light, fog, gravity and time of day for the whole scene.");
    A3_T_WORLD_SETTINGS = t;
    a3_component_type(t)->icon = "globe";
    A3_REFLECT_FIELD(t, A3CWorldSettings, sky_top, A3_FIELD_COLOR, "Sky Color", "Color of the sky overhead.");
    A3_REFLECT_FIELD(t, A3CWorldSettings, sky_horizon, A3_FIELD_COLOR, "Horizon Color", "Color of the sky near the horizon.");
    A3_REFLECT_FIELD(t, A3CWorldSettings, ground_color, A3_FIELD_COLOR, "Ground Color", "Color below the horizon and bounce light from the ground.");
    A3_REFLECT_FIELD(t, A3CWorldSettings, ambient, A3_FIELD_COLOR, "Ambient Color", "Soft light that fills shadows.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, ambient_intensity, A3_FIELD_F32, "Ambient Strength", "How bright shadowed areas are."), 0, 4, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    A3_REFLECT_FIELD(t, A3CWorldSettings, fog_color, A3_FIELD_COLOR, "Fog Color", "Color distant objects fade into.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, fog_density, A3_FIELD_F32, "Fog Density", "0 = no fog. 0.01 = thick fog."), 0, 0.2f, 0.0005f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, time_of_day, A3_FIELD_F32, "Time of Day", "Hour of the day (0-24). Moves the sun when a Directional light exists."), 0, 24, 0.1f)->flags |= A3_FIELD_FLAG_SLIDER;
    A3_REFLECT_FIELD(t, A3CWorldSettings, day_night_cycle, A3_FIELD_BOOL, "Day/Night Cycle", "Advance the time of day automatically while playing.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, day_length_minutes, A3_FIELD_F32, "Day Length (min)", "Real minutes for a full day."), 0.1f, 1440, 0.5f);
    A3_REFLECT_FIELD(t, A3CWorldSettings, gravity, A3_FIELD_VEC3, "Gravity", "Acceleration applied to physics objects, m/s^2.")->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, bloom_intensity, A3_FIELD_F32, "Bloom", "Glow around bright lights, neon and the sun."), 0, 4, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, bloom_threshold, A3_FIELD_F32, "Bloom Threshold", "How bright something must be to glow."), 0, 10, 0.05f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, fog_height_falloff, A3_FIELD_F32, "Fog Height Falloff", "0 = even fog. Higher values keep fog low, near the ground and water."), 0, 1, 0.005f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, ao_strength, A3_FIELD_F32, "Contact Shadows (AO)", "Darkens creases and corners (screen-space ambient occlusion)."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, reflection_strength, A3_FIELD_F32, "Reflections", "Screen-space reflections on smooth surfaces (water, wet roads, cars, glass)."), 0, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, exposure, A3_FIELD_F32, "Exposure", "Overall brightness of the image."), 0.05f, 8, 0.01f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, saturation, A3_FIELD_F32, "Saturation", "Color intensity. 1 = unchanged."), 0, 2, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CWorldSettings, contrast, A3_FIELD_F32, "Contrast", "1 = unchanged."), 0.5f, 2, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CWorldSettings, tint, A3_FIELD_COLOR, "Color Grade", "Tints the final image (warm or cool looks).")->flags |= A3_FIELD_FLAG_ADVANCED;
}

/* ---- Transform system ---- */

A3CTransform *a3_transform(A3World *w, A3Entity e) { return (A3CTransform *)a3_component_get(w, e, A3_T_TRANSFORM); }


static void xform_rec(A3World *w, A3Entity e, const A3Mat4 *parent, u32 depth) {
    while (a3_entity_valid(w, e)) {
        A3EntityRecord *r = &w->entities[e.index];
        A3Entity next = r->next_sibling;
        A3CTransform *t = a3_transform(w, e);
        A3Mat4 world_m;
        if (t) {
            A3Mat4 local = a3_mat4_trs(t->position, t->rotation, t->scale);
            if (parent) a3_mat4_mul_batch(&world_m, parent, &local, 1);  /* SIMD / asm kernel */
            else world_m = local;
            t->world = world_m;
        } else {
            world_m = parent ? *parent : a3_mat4_identity();
        }
        if (a3_entity_valid(w, r->first_child) && depth < 256) xform_rec(w, r->first_child, &world_m, depth + 1);
        e = next;
    }
}

void a3_transform_system_update(A3World *w) {
    if (!w || A3_T_TRANSFORM == 0xFFFFFFFFu) return;
    xform_rec(w, w->first_root, 0, 0);
}

A3Mat4 a3_transform_compute_world(A3World *w, A3Entity e) {
    A3Entity chain[256];
    u32 n = 0;
    for (A3Entity c = e; a3_entity_valid(w, c) && n < 256; c = a3_entity_parent(w, c)) chain[n++] = c;
    A3Mat4 m = a3_mat4_identity();
    while (n--) {
        A3CTransform *t = a3_transform(w, chain[n]);
        if (!t) continue;
        A3Mat4 local = a3_mat4_trs(t->position, t->rotation, t->scale);
        m = a3_mat4_mul(&m, &local);
    }
    return m;
}

A3Vec3 a3_transform_world_position(A3World *w, A3Entity e) {
    A3Mat4 m = a3_transform_compute_world(w, e);
    return a3_mat4_get_translation(&m);
}

A3Quat a3_transform_world_rotation(A3World *w, A3Entity e) {
    A3Quat q = a3_quat_identity();
    for (A3Entity c = e; a3_entity_valid(w, c); c = a3_entity_parent(w, c)) {
        A3CTransform *t = a3_transform(w, c);
        if (t) q = a3_quat_mul(t->rotation, q);
    }
    return a3_quat_normalize(q);
}

A3Vec3 a3_transform_world_forward(A3World *w, A3Entity e) {
    return a3_quat_rotate(a3_transform_world_rotation(w, e), a3_v3(0, 0, -1));
}

void a3_transform_set_world_position(A3World *w, A3Entity e, A3Vec3 p) {
    A3CTransform *t = a3_transform(w, e);
    if (!t) return;
    A3Entity parent = a3_entity_parent(w, e);
    if (!a3_entity_valid(w, parent)) { t->position = p; return; }
    A3Mat4 pm = a3_transform_compute_world(w, parent);
    A3Mat4 inv;
    if (a3_mat4_inverse(&pm, &inv)) t->position = a3_mat4_mul_point(&inv, p);
}

A3Entity a3_find_primary_camera(A3World *w) {
    u32 count = 0;
    const A3Entity *ents = 0;
    A3CCamera *cams = (A3CCamera *)a3_component_array(w, A3_T_CAMERA, &count, &ents);
    A3Entity fallback = A3_ENTITY_NULL;
    for (u32 i = 0; i < count; ++i) {
        if (!a3_entity_active(w, ents[i])) continue;
        if (cams[i].primary) return ents[i];
        if (a3_entity_is_null(fallback)) fallback = ents[i];
    }
    return fallback;
}

A3CWorldSettings *a3_world_settings(A3World *w) {
    u32 count = 0;
    const A3Entity *ents = 0;
    A3CWorldSettings *ws = (A3CWorldSettings *)a3_component_array(w, A3_T_WORLD_SETTINGS, &count, &ents);
    if (count) return &ws[0];
    A3Entity e = a3_entity_create(w, "World Settings");
    return (A3CWorldSettings *)a3_component_add(w, e, A3_T_WORLD_SETTINGS);
}
