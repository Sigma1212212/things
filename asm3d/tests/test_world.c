/*
 * ASM3D - test_world.c : procedural city generator and procedural meshes
 */
#include "a3_test.h"
#include "../engine/world/a3_citygen.h"
#include "../engine/world/a3_procmeshes.h"
#include "../engine/world/a3_traffic.h"
#include "../engine/physics/a3_vehicle.h"
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

static A3Entity ground_box(A3World *w, const char *name, A3Vec3 pos, A3Vec3 size) {
    A3Entity e = a3_entity_create(w, name);
    ((A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM))->position = pos;
    A3CCollider *c = (A3CCollider *)a3_component_add(w, e, A3_T_COLLIDER);
    c->shape = A3_SHAPE_BOX;
    c->size = size;
    return e;
}

static void step_world(A3World *w, u32 steps) {
    for (u32 i = 0; i < steps; ++i) {
        a3_city_update(w);
        a3_traffic_update(w, 1.0f / 60.0f);
        a3_vehicle_update_all(w, 1.0f / 60.0f);
        a3_transform_system_update(w);
    }
}

A3_TEST(world_vehicle_drive_steer_crash) {
    setup();
    A3World *w = a3_world_create("drive");
    ground_box(w, "Ground", a3_v3(0, -0.5f, 0), a3_v3(3000, 1, 3000));
    A3Entity car = a3_vehicle_spawn_car(w, "Car", a3_v3(0, 0, 0), 0, a3_v4(1, 0, 0, 1));
    A3CVehicle *v = (A3CVehicle *)a3_component_get(w, car, A3_T_VEHICLE);
    A3_CHECK(v != 0 && a3_entity_valid(w, a3_entity_find_by_name(w, "Wheel FL")));
    if (!v) { a3_world_destroy(w); return; }
    /* full throttle: accelerates forward (-Z) and stays on the ground */
    v->throttle = 1;
    step_world(w, 180);
    A3CTransform *t = a3_transform(w, car);
    A3_CHECK(v->speed > 15.0f && v->speed <= v->max_speed + 0.01f);
    A3_CHECK(t->position.z < -30.0f && a3_absf(t->position.x) < 0.5f);
    A3_CHECK(v->grounded && a3_absf(t->position.y) < 0.05f);
    /* steering right turns clockwise seen from above: heading swings toward +X */
    v->steer = 1;
    step_world(w, 60);
    A3Vec3 fwd = a3_quat_rotate(t->rotation, a3_v3(0, 0, -1));
    A3_CHECK(fwd.x > 0.3f);
    /* brake to a stop */
    v->steer = 0;
    v->throttle = -1;
    for (u32 i = 0; i < 600 && v->speed > 0.1f; ++i) step_world(w, 1);
    A3_CHECK(a3_absf(v->speed) < 0.6f);
    /* a wall ahead stops the car and reports the impact */
    A3Vec3 p = t->position;
    fwd = a3_quat_rotate(t->rotation, a3_v3(0, 0, -1));
    A3Entity wall = ground_box(w, "Wall", a3_v3_add(p, a3_v3_scale(fwd, 25.0f)), a3_v3(30, 6, 30));
    A3_UNUSED(wall);
    v->throttle = 1;
    f32 max_impact = 0;
    for (u32 i = 0; i < 240; ++i) { step_world(w, 1); max_impact = a3_maxf(max_impact, v->last_impact); }
    A3_CHECK(max_impact > 3.0f && v->damage > 0);
    f32 dist_to_wall_center = a3_v3_len(a3_v3_sub(t->position, a3_v3_add(p, a3_v3_scale(fwd, 25.0f))));
    A3_CHECK(dist_to_wall_center > 15.0f);   /* did not drive through the wall */
    a3_world_destroy(w);
}

A3_TEST(world_traffic_follows_roads) {
    setup();
    a3_traffic_register();
    A3World *w = a3_world_create("traffic");
    A3Entity ce = a3_entity_create(w, "City");
    a3_component_add(w, ce, A3_T_TRANSFORM);
    A3CCity *city = (A3CCity *)a3_component_add(w, ce, A3_T_CITY);
    city->density = 0.2f;
    city->street_lights = 0;
    A3Entity te = a3_entity_create(w, "Traffic");
    a3_component_add(w, te, A3_T_TRANSFORM);
    A3CTraffic *tr = (A3CTraffic *)a3_component_add(w, te, A3_T_TRAFFIC);
    a3_strcpy(tr->roads, sizeof(tr->roads), "generated");
    tr->cars = 12;
    tr->pedestrians = 10;
    step_world(w, 2);
    A3_CHECK(tr->spawned && tr->active_cars == 12 && tr->active_pedestrians == 10);
    A3_CHECK(a3_traffic_graph(w) != 0);
    A3Entity car = a3_entity_find_by_name(w, "Traffic Car 1");
    A3_CHECK(a3_entity_valid(w, car));
    A3Vec3 start = a3_transform(w, car)->position;
    f32 worst = 0, travelled = 0;
    A3Vec3 prev = start;
    for (u32 s = 0; s < 20; ++s) {
        step_world(w, 30);
        A3Vec3 p = a3_transform(w, car)->position, q;
        travelled += a3_v3_len(a3_v3_sub(p, prev));
        prev = p;
        if (a3_traffic_nearest_road_point(w, p, &q, 0)) worst = a3_maxf(worst, a3_sqrtf((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z)));
    }
    A3_CHECK_MSG(travelled > 40.0f, "traffic car only moved %.1f m in 10 s", travelled);
    A3_CHECK_MSG(worst < 9.0f, "traffic car left the road (%.1f m from a center line)", worst);
    A3Vec3 rp;
    A3_CHECK(a3_traffic_random_road_point(w, a3_v3_zero(), 100, 300, &rp, 0));
    A3_CHECK(a3_sqrtf(rp.x * rp.x + rp.z * rp.z) >= 99.0f && a3_sqrtf(rp.x * rp.x + rp.z * rp.z) <= 301.0f);
    a3_world_destroy(w);
}
