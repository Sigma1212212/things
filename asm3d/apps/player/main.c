/*
 * ASM3D - Game Player
 * Runs a game project (or a built-in demo scene) in a desktop window.
 *
 *   asm3d_player [--project DIR] [--scene FILE] [--demo]
 *                [--frames N] [--screenshot FILE.png] [--hidden] [--size WxH]
 *
 * --frames/--screenshot/--hidden make it usable for automated render tests.
 */
#include "../../engine/runtime/a3_engine.h"
#include "../../engine/scene/a3_components.h"
#include "../../engine/scene/a3_scene_io.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/platform/a3_platform.h"
#include <stdlib.h>

static A3Entity spawn(A3World *w, const char *name, A3Primitive prim, A3Vec3 pos, A3Vec3 scale, A3Vec4 color) {
    A3Entity e = a3_entity_create(w, name);
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    t->position = pos;
    t->scale = scale;
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, e, A3_T_MESH_RENDERER);
    mr->primitive = prim;
    mr->base_color = color;
    return e;
}

static void build_demo(A3World *w) {
    A3CWorldSettings *ws = a3_world_settings(w);
    ws->fog_density = 0.004f;
    spawn(w, "Ground", A3_PRIM_PLANE, a3_v3(0, 0, 0), a3_v3(60, 1, 60), a3_v4(0.36f, 0.42f, 0.33f, 1));
    const A3Vec4 palette[5] = { { 0.85f, 0.3f, 0.25f, 1 }, { 0.25f, 0.55f, 0.85f, 1 }, { 0.95f, 0.8f, 0.3f, 1 }, { 0.4f, 0.8f, 0.45f, 1 }, { 0.8f, 0.8f, 0.85f, 1 } };
    const A3Primitive prims[5] = { A3_PRIM_CUBE, A3_PRIM_SPHERE, A3_PRIM_CYLINDER, A3_PRIM_CAPSULE, A3_PRIM_CONE };
    for (int i = 0; i < 5; ++i) {
        A3Entity e = spawn(w, "Shape", prims[i], a3_v3((f32)(i - 2) * 2.2f, prims[i] == A3_PRIM_CAPSULE ? 1.0f : 0.5f, 0), a3_v3_one(), palette[i]);
        A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(w, e, A3_T_MESH_RENDERER);
        mr->metallic = i == 4 ? 1.0f : 0.0f;
        mr->roughness = i == 4 ? 0.25f : 0.55f;
    }
    /* a field of instanced crates to exercise batching and culling */
    for (int z = 0; z < 20; ++z)
        for (int x = 0; x < 20; ++x) {
            f32 h = 0.3f + (f32)((x * 7 + z * 13) % 10) * 0.12f;
            spawn(w, "Crate", A3_PRIM_CUBE, a3_v3(-20.0f + x * 2.0f, h * 0.5f, -8.0f - z * 2.0f), a3_v3(0.8f, h, 0.8f), a3_v4(0.55f, 0.42f, 0.3f, 1));
        }
    A3Entity sun = a3_entity_create(w, "Sun");
    A3CTransform *st = (A3CTransform *)a3_component_add(w, sun, A3_T_TRANSFORM);
    st->rotation = a3_quat_euler(-50 * A3_DEG2RAD, 35 * A3_DEG2RAD, 0);
    A3CLight *sl = (A3CLight *)a3_component_add(w, sun, A3_T_LIGHT);
    sl->type = A3_LIGHT_DIRECTIONAL;
    sl->color = a3_v4(1.0f, 0.95f, 0.85f, 1);
    sl->intensity = 1.0f;
    sl->cast_shadows = 1;
    A3Entity lamp = a3_entity_create(w, "Lamp");
    ((A3CTransform *)a3_component_add(w, lamp, A3_T_TRANSFORM))->position = a3_v3(-2.2f, 1.6f, 1.5f);
    A3CLight *pl = (A3CLight *)a3_component_add(w, lamp, A3_T_LIGHT);
    pl->type = A3_LIGHT_POINT;
    pl->color = a3_v4(1.0f, 0.5f, 0.2f, 1);
    pl->intensity = 2.0f;
    pl->range = 6.0f;
    A3Entity cam = a3_entity_create(w, "Main Camera");
    A3CTransform *ct = (A3CTransform *)a3_component_add(w, cam, A3_T_TRANSFORM);
    ct->position = a3_v3(0, 3.2f, 7.5f);
    ct->rotation = a3_quat_euler(-18 * A3_DEG2RAD, 0, 0);
    a3_component_add(w, cam, A3_T_CAMERA);
}

int main(int argc, char **argv) {
    const char *project = "", *scene = 0, *shot = 0;
    int frames = -1, width = 1280, height = 720;
    b32 hidden = 0, demo = 0;
    for (int i = 1; i < argc; ++i) {
        if (!a3_strcmp(argv[i], "--project") && i + 1 < argc) project = argv[++i];
        else if (!a3_strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!a3_strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!a3_strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!a3_strcmp(argv[i], "--hidden")) hidden = 1;
        else if (!a3_strcmp(argv[i], "--demo")) demo = 1;
        else if (!a3_strcmp(argv[i], "--size") && i + 1 < argc) {
            const char *s = argv[++i];
            width = atoi(s);
            const char *x = a3_strchr(s, 'x');
            if (x) height = atoi(x + 1);
        }
    }
    A3EngineDesc d = { "ASM3D Player", width, height, 1, hidden, 1, project, 60 };
    A3Engine *eng = a3_engine_create(&d);
    if (!eng) {
        A3_FATAL("player", "could not start the engine (see errors above)");
        return 1;
    }
    A3World *w = a3_world_create("Game");
    if (scene) {
        char path[1024];
        if (project[0]) a3_path_join(path, sizeof(path), project, scene); else a3_strcpy(path, sizeof(path), scene);
        A3SceneLoadReport rep;
        if (a3_scene_load_file(w, path, &rep) != A3_OK) {
            A3_ERROR("player", "%s", rep.error);
            if (rep.hint[0]) A3_ERROR("player", "%s", rep.hint);
            demo = 1;
        }
    } else {
        demo = 1;
    }
    if (demo && a3_world_entity_count(w) == 0) build_demo(w);
    a3_engine_start_play(eng, w);
    int frame = 0;
    f32 dt;
    while (a3_engine_begin_frame(eng, &dt)) {
        if (frames >= 0) dt = 1.0f / 60.0f; /* deterministic stepping for tests */
        a3_engine_simulate(eng, w, dt, 0, 0);
        a3_engine_render_world(eng, w, 0);
        ++frame;
        if (frames >= 0 && frame >= frames) {
            if (shot) a3_engine_screenshot(eng, shot);
            const A3RenderFrameInfo *fi = a3_renderer_frame_info(a3_engine_renderer(eng));
            const A3RhiStats *rs = a3_rhi_stats();
            A3_INFO("player", "frame %d: %u renderables, %u visible, %u culled, %u batches, %u shadow casters, %u lights, cpu %.2f ms",
                    frame, fi->renderables, fi->visible, fi->culled, fi->batches, fi->shadow_casters, fi->lights, fi->cpu_ms);
            A3_INFO("player", "rhi: %u draw calls, %u triangles, %u instances", rs->draw_calls, rs->triangles, rs->instances);
            a3_engine_end_frame(eng);
            break;
        }
        a3_engine_end_frame(eng);
    }
    a3_engine_stop_play(eng, w);
    a3_world_destroy(w);
    a3_engine_destroy(eng);
    return 0;
}
