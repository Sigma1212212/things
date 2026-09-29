/*
 * ASM3D - test_world.c : procedural city generator and procedural meshes
 */
#include "a3_test.h"
#include "../engine/world/a3_citygen.h"
#include "../engine/world/a3_procmeshes.h"
#include "../engine/scene/a3_components.h"
#include "../engine/physics/a3_physics.h"
#include "../engine/resource/a3_assets.h"
#include "../engine/core/a3_json.h"
#include "../engine/core/a3_memory.h"
#include "../engine/core/a3_strbuf.h"
#include "../engine/core/a3_string.h"

static void setup(void) {
    a3_register_core_components();
    a3_physics_register();
    a3_assets_init("");
}

A3_TEST(world_procedural_meshes) {
    setup();
    a3_procmeshes_register();
    A3_CHECK_EQ_INT(a3_procmeshes_count(), 4);
    for (u32 i = 0; i < a3_procmeshes_count(); ++i) {
        const char *name = a3_procmeshes_name(i);
        A3_CHECK(a3_assets_has_mesh_generator(name));
        u32 id = a3_assets_mesh(name);
        const A3MeshAsset *m = a3_assets_mesh_get(id);
        A3_CHECK_MSG(m && m->state == A3_ASSET_STATE_LOADED && a3_streq(m->path, name), "procedural mesh %s did not build", name);
        if (!m) continue;
        A3_CHECK(m->cpu.index_count >= 36 && m->cpu.index_count % 3 == 0);
        A3_CHECK(m->radius > 0.05f && m->radius < 5.0f);
        A3_CHECK_EQ_INT(a3_assets_mesh(name), id);     /* cached */
    }
    /* the car sits on the ground and faces -Z */
    const A3MeshAsset *car = a3_assets_mesh_get(a3_assets_mesh("builtin:car_body"));
    A3_CHECK(car && car->bounds.min.y > 0.2f && car->bounds.max.y < 1.6f && car->bounds.max.z - car->bounds.min.z > 4.0f);
    A3_CHECK(!a3_assets_has_mesh_generator("builtin:does_not_exist"));
}

A3_TEST(world_city_generation) {
    setup();
    A3CityDesc d;
    a3_city_desc_default(&d);
    d.density = 0.3f;
    d.seed = 42;
    A3World *w = a3_world_create("city");
    A3StrBuf roads;
    a3_strbuf_init(&roads, A3_MEM_TEMP);
    A3CityStats st;
    A3_CHECK(a3_city_generate(w, &d, &st, &roads));
    A3_CHECK(st.buildings > 100 && st.towers > 5 && st.hotels > 3 && st.palms > 100 && st.lights > 100);
    A3_CHECK(a3_world_entity_count(w) > 2000);
    A3_CHECK(st.road_nodes > 200 && st.road_edges > st.road_nodes);
    /* the road graph parses and is (almost entirely) one connected network */
    A3Arena arena;
    a3_arena_init(&arena, A3_MEM_TEMP, 1 << 16);
    A3Json *root = a3_json_parse(a3_strbuf_cstr(&roads), roads.len, &arena, 0);
    A3_CHECK(root != 0);
    const A3Json *nodes = a3_json_get(root, "nodes"), *edges = a3_json_get(root, "edges");
    A3_CHECK(nodes && edges && a3_json_count(nodes) == st.road_nodes && a3_json_count(edges) == st.road_edges);
    u32 n = st.road_nodes;
    u32 *parent = A3_NEW_ARRAY(u32, n, A3_MEM_TEMP);
    for (u32 i = 0; i < n; ++i) parent[i] = i;
    A3_JSON_FOREACH(ed, edges) {
        u32 a = (u32)a3_json_number(a3_json_at(ed, 0), 0), b = (u32)a3_json_number(a3_json_at(ed, 1), 0);
        A3_CHECK(a < n && b < n && a != b);
        if (a >= n || b >= n) continue;
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        while (parent[b] != b) b = parent[b] = parent[parent[b]];
        parent[a] = b;
    }
    u32 *size = A3_NEW_ARRAY(u32, n, A3_MEM_TEMP), best = 0;
    for (u32 i = 0; i < n; ++i) { u32 r = i; while (parent[r] != r) r = parent[r]; if (++size[r] > best) best = size[r]; }
    A3_CHECK_MSG(best * 100 >= n * 98, "largest road network has %u of %u nodes", best, n);
    a3_free(parent);
    a3_free(size);
    a3_arena_release(&arena);
    /* same seed, same city */
    A3World *w2 = a3_world_create("city2");
    A3CityStats st2;
    A3_CHECK(a3_city_generate(w2, &d, &st2, 0));
    A3_CHECK(st2.buildings == st.buildings && st2.palms == st.palms && a3_world_entity_count(w2) == a3_world_entity_count(w));
    /* time of day changes the sky */
    const A3CWorldSettings *ws = 0;
    for (u32 i = 0; i < w->high_water && !ws; ++i) {
        if (!w->entities[i].alive) continue;
        A3Entity e = { i, w->entities[i].gen };
        ws = (const A3CWorldSettings *)a3_component_get(w, e, A3_T_WORLD_SETTINGS);
    }
    A3_CHECK(ws != 0);
    if (ws) {
        f32 night_top = ws->sky_top.z;
        a3_city_apply_time(w, A3_CITY_DAY);
        A3_CHECK(ws->sky_top.z > night_top && ws->exposure > 0.5f);
    }
    a3_strbuf_free(&roads);
    a3_world_destroy(w2);
    a3_world_destroy(w);
}
