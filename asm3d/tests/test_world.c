/*
 * ASM3D - test_world.c : procedural city generator and procedural meshes
 */
#include "a3_test.h"
#include "../engine/world/a3_citygen.h"
#include "../engine/world/a3_procmeshes.h"
#include "../engine/world/a3_geom_kernels.h"
#include "../engine/core/a3_hash.h"
#include "../engine/world/a3_traffic.h"
#include "../engine/physics/a3_vehicle.h"
#include "../engine/physics/a3_vehicle_kernels.h"
#include "../engine/world/a3_weather.h"
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
    A3_CHECK(a3_procmeshes_count() >= 25);
    for (u32 i = 0; i < a3_procmeshes_count(); ++i) {
        const char *name = a3_procmeshes_name(i);
        A3_CHECK(a3_assets_has_mesh_generator(name));
        u32 id = a3_assets_mesh(name);
        const A3MeshAsset *m = a3_assets_mesh_get(id);
        A3_CHECK_MSG(m && m->state == A3_ASSET_STATE_LOADED && a3_streq(m->path, name), "procedural mesh %s did not build", name);
        if (!m) continue;
        A3_CHECK(m->cpu.index_count >= 36 && m->cpu.index_count % 3 == 0);
        A3_CHECK(m->radius > 0.05f && m->radius < 12.0f);
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
    d.density = 0.6f;
    d.seed = 42;
    A3World *w = a3_world_create("city");
    A3StrBuf roads;
    a3_strbuf_init(&roads, A3_MEM_TEMP);
    A3CityStats st;
    A3_CHECK(a3_city_generate(w, &d, &st, &roads));
    A3_CHECK(st.buildings > 100 && st.towers > 5 && st.hotels > 3 && st.palms > 100 && st.lights > 100 && st.props > 200);
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

static f64 g_step_time;
static void step_world(A3World *w, u32 steps) {
    for (u32 i = 0; i < steps; ++i) {
        g_step_time += 1.0 / 60.0;
        a3_city_update(w);
        a3_traffic_update(w, 1.0f / 60.0f, g_step_time);
        a3_vehicle_update_all(w, 1.0f / 60.0f);
        a3_transform_system_update(w);
    }
}

static void vehicle_drive_steer_crash(i32 model) {
    setup();
    A3World *w = a3_world_create("drive");
    ground_box(w, "Ground", a3_v3(0, -0.5f, 0), a3_v3(3000, 1, 3000));
    A3Entity car = a3_vehicle_spawn_car(w, "Car", a3_v3(0, 0, 0), 0, a3_v4(1, 0, 0, 1));
    A3CVehicle *v = (A3CVehicle *)a3_component_get(w, car, A3_T_VEHICLE);
    A3_CHECK(v != 0 && a3_entity_valid(w, a3_entity_find_by_name(w, "Wheel FL")));
    if (!v) { a3_world_destroy(w); return; }
    b32 real = model == A3_VEHICLE_REALISTIC;
    /* every field made it into the reflection table (none dropped at the field limit) */
    const A3ComponentType *vt = a3_component_type(A3_T_VEHICLE);
    A3_CHECK(vt && a3_component_find_field(vt, "roll") && a3_component_find_field(vt, "physics_model") && a3_component_find_field(vt, "gear"));
    for (u32 i = 0; i < a3_component_type_count(); ++i) {
        const A3ComponentType *ct = a3_component_type(i);
        if (ct) A3_CHECK_MSG(ct->field_count < A3_MAX_FIELDS, "component %s is at the field limit", ct->name);
    }
    v->physics_model = model;
    /* at rest the realistic car settles on its springs with the origin on the ground */
    step_world(w, 60);
    A3CTransform *t = a3_transform(w, car);
    A3_CHECK(a3_absf(t->position.y) < 0.03f && a3_absf(t->position.z) < 0.05f);
    if (real) A3_CHECK(v->comp[0] > 0.03f && v->comp[3] > 0.03f && v->gear == 1);
    /* full throttle: accelerates forward (-Z) and stays on the ground */
    v->throttle = 1;
    f32 squat = 0;
    for (u32 i = 0; i < 180; ++i) { step_world(w, 1); if (i == 30) squat = (v->comp[2] + v->comp[3]) - (v->comp[0] + v->comp[1]); }
    A3_CHECK(v->speed > (real ? 10.0f : 15.0f) && v->speed <= v->max_speed + 0.01f);
    A3_CHECK(t->position.z < (real ? -12.0f : -30.0f) && a3_absf(t->position.x) < 0.5f);
    A3_CHECK(v->grounded && a3_absf(t->position.y) < 0.05f);
    if (real) {
        A3_CHECK_MSG(squat > 0.002f, "no squat under acceleration (%.4f)", squat);   /* weight moves to the rear */
        A3_CHECK(v->gear >= 2 && v->rpm > 1000.0f && v->rpm < v->redline * 1.03f);
    }
    /* steering right turns clockwise seen from above: heading swings toward +X */
    v->steer = 1;
    f32 roll = 0;
    for (u32 i = 0; i < 60; ++i) { step_world(w, 1); roll = a3_maxf(roll, v->roll); }
    A3Vec3 fwd = a3_quat_rotate(t->rotation, a3_v3(0, 0, -1));
    A3_CHECK(fwd.x > 0.3f);
    if (real) A3_CHECK_MSG(roll > 0.01f && roll < 0.15f, "body roll %.3f in a right turn", roll);   /* leans out of the turn */
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

A3_TEST(world_vehicle_drive_steer_crash) { vehicle_drive_steer_crash(A3_VEHICLE_REALISTIC); }
A3_TEST(world_vehicle_arcade_drive) { vehicle_drive_steer_crash(A3_VEHICLE_ARCADE); }

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

static b32 same_bits(const void *a, const void *b, usize n) { return a3_memcmp(a, b, n) == 0; }

A3_TEST(world_geometry_kernels_bit_exact) {
    A3Rng rng;
    a3_rng_seed(&rng, 99, 1);
    /* Catmull-Rom: closed and open */
    A3Vec2 ctrl[17], o1[17 * 7 + 1], o2[17 * 7 + 1];
    for (u32 i = 0; i < 17; ++i) ctrl[i] = a3_v2(a3_rng_range_f32(&rng, -5, 5), a3_rng_range_f32(&rng, -5, 5));
    for (u32 closed = 0; closed < 2; ++closed) {
        a3_memset(o1, 0, sizeof(o1)); a3_memset(o2, 0, sizeof(o2));
        u32 n1 = a3_gk_catmull_rom(ctrl, 17, 7, closed, o1), n2 = a3_gk_ref_catmull_rom(ctrl, 17, 7, closed, o2);
        A3_CHECK(n1 == n2 && n1 == (closed ? 17u * 7u : 16u * 7u + 1u));
        A3_CHECK(same_bits(o1, o2, sizeof(A3Vec2) * n1));
    }
    A3_CHECK(o1[0].x == ctrl[0].x && o1[7].y == ctrl[1].y);     /* passes through the control points */
    /* frame placement into a strided vertex array */
    A3Vertex v1[41], v2[41];
    a3_memset(v1, 0, sizeof(v1)); a3_memset(v2, 0, sizeof(v2));
    f32 frame[12];
    for (u32 i = 0; i < 12; ++i) frame[i] = (i % 4 == 3) ? 0.0f : a3_rng_range_f32(&rng, -2, 2);
    a3_gk_frame_points(o1, 41, frame, &v1[0].position, sizeof(A3Vertex));
    a3_gk_ref_frame_points(o1, 41, frame, &v2[0].position, sizeof(A3Vertex));
    A3_CHECK(same_bits(v1, v2, sizeof(v1)));
    /* grid indices */
    u32 i1[6 * 8 * 6], i2[6 * 8 * 6];
    for (u32 wrap = 0; wrap < 2; ++wrap) {
        u32 a = a3_gk_grid_indices(6, 8, wrap, 5, i1), b = a3_gk_ref_grid_indices(6, 8, wrap, 5, i2);
        A3_CHECK(a == b && a == 5u * (wrap ? 8u : 7u) * 6u && same_bits(i1, i2, a * 4));
    }
    /* normals and volume on a random closed-ish surface */
    A3Vertex g1[48], g2[48];
    for (u32 i = 0; i < 48; ++i) {
        g1[i].position = a3_v3(a3_rng_range_f32(&rng, -3, 3), a3_rng_range_f32(&rng, -3, 3), a3_rng_range_f32(&rng, -3, 3));
        g1[i].normal = a3_v3_zero();
        g1[i].uv = a3_v2(0, 0);
    }
    g1[47].position = g1[46].position;   /* a degenerate triangle -> zero normal -> (0,1,0) */
    a3_memcpy(g2, g1, sizeof(g1));
    u32 tris = a3_gk_grid_indices(6, 8, 1, 0, i1) / 3;
    a3_gk_vertex_normals(g1, i1, tris);
    a3_gk_ref_vertex_normals(g2, i1, tris);
    A3_CHECK(same_bits(g1, g2, sizeof(g1)));
    g1[3].normal = g2[3].normal = a3_v3_zero();
    a3_gk_normalize(g1, 48);
    a3_gk_ref_normalize(g2, 48);
    A3_CHECK(same_bits(g1, g2, sizeof(g1)));
    A3_CHECK(g1[3].normal.y == 1.0f && g1[3].normal.x == 0.0f);
    f32 va = a3_gk_signed_volume(g1, i1, tris), vb = a3_gk_ref_signed_volume(g1, i1, tris);
    A3_CHECK(same_bits(&va, &vb, 4));
    /* a unit cube from the car builder's point of view: outward winding is positive */
    A3MeshData cube;
    A3_CHECK(a3_mesh_cube(&cube));
    A3Vertex *cv = cube.vertices;
    A3_CHECK(a3_gk_signed_volume(cv, cube.indices, cube.index_count / 3) > 0.0f);
    a3_mesh_free(&cube);
    A3_CHECK(a3_strlen(a3_gk_backend()) > 0);
}

A3_TEST(vehicle_wheel_kernel_bit_exact) {
    A3Rng rng;
    a3_rng_seed(&rng, 7, 3);
    enum { NQ = 64 };
    static A3WheelQuad q1[NQ], q2[NQ];
    for (u32 n = 0; n < NQ; ++n) {
        for (int i = 0; i < 4; ++i) {
            q1[n].comp[i] = a3_rng_range_f32(&rng, -0.05f, 0.25f);
            q1[n].comp_vel[i] = a3_rng_range_f32(&rng, -3, 3);
            q1[n].stiffness[i] = a3_rng_range_f32(&rng, 20000, 60000);
            q1[n].damping[i] = a3_rng_range_f32(&rng, 1000, 5000);
            q1[n].v_long[i] = a3_rng_range_f32(&rng, -60, 60);
            q1[n].v_lat[i] = a3_rng_range_f32(&rng, -20, 20);
            q1[n].mu[i] = a3_rng_range_f32(&rng, 0.3f, 1.3f);
            q1[n].drive[i] = a3_rng_range_f32(&rng, -12000, 12000);
            q1[n].lock[i] = a3_rng_f32(&rng) < 0.2f ? 1.0f : 0.0f;
        }
    }
    /* edge cases: at rest, exactly zero lateral speed, negative zero, huge slip */
    q1[0].v_long[0] = 0.0f; q1[0].v_lat[0] = 0.0f;
    q1[0].v_long[1] = -0.0f; q1[0].lock[1] = 1.0f;
    q1[0].v_lat[2] = 1e6f; q1[0].comp[3] = 0.0f;
    a3_memcpy(q2, q1, sizeof(q1));
    a3_vk_wheels(q1, NQ);
    a3_vk_ref_wheels(q2, NQ);
    A3_CHECK(same_bits(q1, q2, sizeof(q1)));
    /* physics sanity */
    A3WheelQuad t;
    a3_zero_struct(&t);
    for (int i = 0; i < 4; ++i) { t.comp[i] = 0.1f; t.stiffness[i] = 40000; t.mu[i] = 1.0f; t.v_long[i] = 20.0f; }
    t.v_lat[0] = 2.0f;                  /* sliding right -> force to the left */
    t.v_lat[1] = -2.0f;
    t.drive[2] = 1e6f;                  /* more than the tire can take */
    t.lock[3] = 1.0f;
    a3_vk_wheels(&t, 1);
    A3_CHECK(t.load[0] == 4000.0f);
    A3_CHECK(t.fy[0] < -3000.0f && t.fy[0] >= -4000.0f && t.fy[1] == -t.fy[0]);
    A3_CHECK(t.fx[2] == 4000.0f && t.fy[2] == 0.0f && t.slip[2] == 1.0f);   /* wheelspin: no grip left */
    A3_CHECK(t.fx[3] < -3500.0f && t.slip[3] == 1.0f);                   /* locked: slides against the motion */
    A3_CHECK(t.slip[0] < 0.5f);
    f32 in[4] = { 0.0f, 0.5f, -2.0f, 30.0f }, at[4], sn[4];
    a3_vk_ref_atan4(in, at);
    a3_vk_ref_sin4(in, sn);
    for (int i = 0; i < 4; ++i) {
        A3_CHECK(a3_absf(at[i] - a3_atan2f(in[i], 1.0f)) < 1e-5f);
        A3_CHECK(a3_absf(sn[i] - a3_sinf(in[i])) < 1e-4f);
    }
    A3_CHECK(a3_strlen(a3_vk_backend()) > 0);
}

/* distance a realistic car needs to stop from ~20 m/s */
static f32 braking_distance(f32 wetness) {
    A3World *w = a3_world_create("brake");
    ground_box(w, "Ground", a3_v3(0, -0.5f, 0), a3_v3(3000, 1, 3000));
    a3_world_settings(w)->wetness = wetness;
    A3Entity car = a3_vehicle_spawn_car(w, "Car", a3_v3(0, 0, 0), 0, a3_v4(1, 1, 1, 1));
    A3CVehicle *v = (A3CVehicle *)a3_component_get(w, car, A3_T_VEHICLE);
    v->throttle = 1;
    for (u32 i = 0; i < 900 && v->speed < 20.0f; ++i) step_world(w, 1);
    A3Vec3 start = a3_transform(w, car)->position;
    v->throttle = -1;
    for (u32 i = 0; i < 900 && v->speed > 0.3f; ++i) step_world(w, 1);
    f32 d = a3_v3_len(a3_v3_sub(a3_transform(w, car)->position, start));
    a3_world_destroy(w);
    return d;
}

A3_TEST(world_weather_rain) {
    setup();
    A3World *w = a3_world_create("weather");
    a3_weather_set(w, 0.8f);
    A3CWorldSettings *ws = a3_world_settings(w);
    A3_CHECK(ws->rain == 0.8f && ws->wetness > 0.9f && ws->cloud_cover >= 0.9f);
    a3_weather_set(w, 0.0f);
    A3_CHECK(ws->rain == 0.0f && ws->wetness == 0.0f);
    a3_world_destroy(w);
    /* lightning: only in heavy rain, deterministic, some flashes within a minute */
    f32 flashes = 0, light = 0;
    for (u32 i = 0; i < 60 * 60; ++i) {
        f64 t = i / 60.0;
        flashes = a3_maxf(flashes, a3_weather_lightning(t, 0.9f));
        light = a3_maxf(light, a3_weather_lightning(t, 0.3f));
        A3_CHECK(a3_weather_lightning(t, 0.9f) == a3_weather_lightning(t, 0.9f));
    }
    A3_CHECK(flashes > 0.5f && light == 0.0f);
    A3_CHECK(a3_weather_sun_factor(0.0f) == 1.0f && a3_weather_sun_factor(1.0f) < 0.2f);
    A3_CHECK(a3_weather_grip(1.0f) < a3_weather_grip(0.0f));
    /* wet roads: the realistic car needs clearly longer to stop */
    f32 dry = braking_distance(0.0f), wet = braking_distance(1.0f);
    A3_CHECK_MSG(dry > 10.0f && dry < 40.0f, "dry braking distance %.1f m", dry);
    A3_CHECK_MSG(wet > dry * 1.2f, "wet braking %.1f m vs dry %.1f m", wet, dry);
}
