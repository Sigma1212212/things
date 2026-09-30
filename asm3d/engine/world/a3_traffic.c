/*
 * ASM3D - a3_traffic.c
 */
#include "a3_traffic.h"
#include "a3_procmeshes.h"
#include "a3_citygen.h"
#include "a3_people.h"
#include "../physics/a3_vehicle.h"
#include "../physics/a3_physics.h"
#include "../scene/a3_components.h"
#include "../resource/a3_assets.h"
#include "../platform/a3_platform.h"
#include "../core/a3_json.h"
#include "../core/a3_hash.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"
#include "../core/a3_memory.h"
#include "../core/a3_format.h"

u32 A3_T_TRAFFIC = 0xFFFFFFFFu;
u32 A3_T_TRAFFIC_AGENT = 0xFFFFFFFFu;

#define KIND_CAR 0
#define KIND_PED 1

/* ======================================================================== */
/* Road graph                                                               */
/* ======================================================================== */

void a3_road_graph_free(A3RoadGraph *g) {
    a3_free(g->nodes);
    a3_free(g->adj_start);
    a3_free(g->adj);
    a3_free(g->adj_lanes);
    a3_free(g->signal);
    a3_zero_struct(g);
}

b32 a3_road_graph_parse(const char *json, usize len, A3RoadGraph *g) {
    a3_zero_struct(g);
    A3Arena arena;
    a3_arena_init(&arena, A3_MEM_TEMP, 1 << 16);
    A3Json *root = a3_json_parse(json, len, &arena, 0);
    const A3Json *nodes = root ? a3_json_get(root, "nodes") : 0, *edges = root ? a3_json_get(root, "edges") : 0;
    if (!nodes || !edges || !a3_streq(a3_json_get_string(root, "format", ""), "asm3d.roads")) { a3_arena_release(&arena); return 0; }
    u32 n = a3_json_count(nodes), m = a3_json_count(edges);
    g->road_y = (f32)a3_json_get_number(root, "road_y", 0.0);
    g->walk_y = (f32)a3_json_get_number(root, "walk_y", g->road_y);
    g->half_width = (f32)a3_json_get_number(root, "road_half_width", 6.0);
    g->nodes = A3_NEW_ARRAY(A3Vec2, n ? n : 1, A3_MEM_WORLD);
    g->adj_start = A3_NEW_ARRAY(u32, n + 1, A3_MEM_WORLD);
    u32 *ea = A3_NEW_ARRAY(u32, m * 2 + 1, A3_MEM_TEMP);
    u8 *el = A3_NEW_ARRAY(u8, m + 1, A3_MEM_TEMP);
    if (!g->nodes || !g->adj_start || !ea || !el) { a3_free(ea); a3_free(el); a3_road_graph_free(g); a3_arena_release(&arena); return 0; }
    u32 i = 0;
    A3_JSON_FOREACH(nd, nodes) { f32 p[2] = { 0, 0 }; a3_json_get_floats(nd, p, 2); g->nodes[i++] = a3_v2(p[0], p[1]); }
    g->node_count = n;
    u32 valid = 0;
    A3_JSON_FOREACH(ed, edges) {
        u32 a = (u32)a3_json_number(a3_json_at(ed, 0), -1), b = (u32)a3_json_number(a3_json_at(ed, 1), -1);
        u32 lanes = (u32)a3_json_number(a3_json_at(ed, 2), 1);
        if (a >= n || b >= n || a == b) continue;
        ea[valid * 2] = a; ea[valid * 2 + 1] = b; el[valid] = (u8)a3_clampi((i32)lanes, 1, 4);
        g->adj_start[a + 1]++; g->adj_start[b + 1]++;
        valid++;
    }
    for (u32 k = 0; k < n; ++k) g->adj_start[k + 1] += g->adj_start[k];
    g->adj = A3_NEW_ARRAY(u32, valid * 2 + 1, A3_MEM_WORLD);
    g->adj_lanes = A3_NEW_ARRAY(u8, valid * 2 + 1, A3_MEM_WORLD);
    u32 *fill = A3_NEW_ARRAY(u32, n + 1, A3_MEM_TEMP);
    if (!g->adj || !g->adj_lanes || !fill) { a3_free(ea); a3_free(el); a3_free(fill); a3_road_graph_free(g); a3_arena_release(&arena); return 0; }
    for (u32 e = 0; e < valid; ++e) {
        u32 a = ea[e * 2], b = ea[e * 2 + 1];
        u32 ia = g->adj_start[a] + fill[a]++, ib = g->adj_start[b] + fill[b]++;
        g->adj[ia] = b; g->adj_lanes[ia] = el[e];
        g->adj[ib] = a; g->adj_lanes[ib] = el[e];
    }
    a3_free(ea);
    a3_free(el);
    a3_free(fill);
    g->signal = A3_NEW_ARRAY(u8, n + 1, A3_MEM_WORLD);
    const A3Json *sig = a3_json_get(root, "signals");
    if (g->signal && sig) A3_JSON_FOREACH(sn, sig) { u32 k = (u32)a3_json_number(sn, -1); if (k < n) g->signal[k] = 1; }
    a3_arena_release(&arena);
    return 1;
}

static u32 degree(const A3RoadGraph *g, u32 n) { return g->adj_start[n + 1] - g->adj_start[n]; }

static u32 lanes_between(const A3RoadGraph *g, u32 a, u32 b) {
    for (u32 k = g->adj_start[a]; k < g->adj_start[a + 1]; ++k) if (g->adj[k] == b) return g->adj_lanes[k];
    return 1;
}

/* One cached graph per world (loaded from the Traffic component's path). */
typedef struct GraphCache { A3World *world; char path[128]; A3RoadGraph g; u32 *comp; u32 main_comp, main_size; b32 ok; A3Rng rng; } GraphCache;
static GraphCache g_cache[4];

static void cache_release(A3World *w) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_cache); ++i) {
        if (g_cache[i].world != w) continue;
        a3_road_graph_free(&g_cache[i].g);
        a3_free(g_cache[i].comp);
        a3_zero_struct(&g_cache[i]);
    }
}

static GraphCache *graph_for(A3World *w, const char *path) {
    GraphCache *slot = 0;
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_cache); ++i) {
        if (g_cache[i].world == w && a3_streq(g_cache[i].path, path)) return g_cache[i].ok ? &g_cache[i] : 0;
        if (!slot && !g_cache[i].world) slot = &g_cache[i];
    }
    if (!slot) { cache_release(g_cache[0].world); slot = &g_cache[0]; }
    slot->world = w;
    a3_strcpy(slot->path, sizeof(slot->path), path);
    if (!path[0] || a3_streq(path, "generated")) {
        usize len = 0;
        const char *json = a3_city_generated_roads(w, &len);
        if (!json) { slot->world = 0; return 0; }     /* the City component has not generated yet: try again next step */
        slot->ok = a3_road_graph_parse(json, len, &slot->g);
    } else {
        char full[1024];
        a3_assets_path(path, full, sizeof(full));
        A3FileData fd;
        if (a3_file_read_all(full, A3_MEM_TEMP, &fd) != A3_OK) {
            a3_log_hint(A3_LOG_ERROR, "traffic", "Generate a city with 'asm3d_cli world city <project>', add a City component, or fix the Roads path of the Traffic component.", "road graph '%s' not found", path);
            return 0;
        }
        slot->ok = a3_road_graph_parse((const char *)fd.data, fd.size, &slot->g);
        a3_free(fd.data);
    }
    if (!slot->ok) { A3_ERROR("traffic", "road graph '%s' is not a valid asm3d.roads file", path); return 0; }
    /* connected components: traffic only uses the largest network */
    u32 n = slot->g.node_count;
    slot->comp = A3_NEW_ARRAY(u32, n + 1, A3_MEM_WORLD);
    u32 *stack = A3_NEW_ARRAY(u32, n + 1, A3_MEM_TEMP), *sizes = A3_NEW_ARRAY(u32, n + 1, A3_MEM_TEMP);
    if (!slot->comp || !stack || !sizes) { a3_free(stack); a3_free(sizes); slot->ok = 0; return 0; }
    for (u32 i = 0; i < n; ++i) slot->comp[i] = 0xFFFFFFFFu;
    u32 ncomp = 0, best = 0;
    for (u32 i = 0; i < n; ++i) {
        if (slot->comp[i] != 0xFFFFFFFFu) continue;
        u32 sp = 0;
        stack[sp++] = i;
        slot->comp[i] = ncomp;
        while (sp) {
            u32 x = stack[--sp];
            sizes[ncomp]++;
            for (u32 k = slot->g.adj_start[x]; k < slot->g.adj_start[x + 1]; ++k) {
                u32 y = slot->g.adj[k];
                if (slot->comp[y] == 0xFFFFFFFFu) { slot->comp[y] = ncomp; stack[sp++] = y; }
            }
        }
        if (sizes[ncomp] > sizes[best]) best = ncomp;
        ncomp++;
    }
    slot->main_comp = best;
    slot->main_size = n ? sizes[best] : 0;
    a3_free(stack);
    a3_free(sizes);
    a3_rng_seed(&slot->rng, 12345, 17);
    A3_INFO("traffic", "road graph '%s': %u nodes, main network %u nodes", path, n, slot->main_size);
    return slot;
}

/* ======================================================================== */
/* Components                                                               */
/* ======================================================================== */

void a3_traffic_register(void) {
    a3_world_on_destroy(cache_release);
    a3_city_register();
    a3_people_register();
    if (A3_T_TRAFFIC != 0xFFFFFFFFu) return;
    a3_vehicle_register();
    A3CTraffic tr;
    a3_zero_struct(&tr);
    a3_strcpy(tr.roads, sizeof(tr.roads), "Assets/City/roads.json");
    tr.cars = 40; tr.pedestrians = 60; tr.seed = 7; tr.speed_limit = 14.0f; tr.spawn_radius = 0;
    u32 t = a3_component_register("Traffic", "Gameplay", sizeof(A3CTraffic), 16, &tr, A3_COMP_BUILTIN,
        "Fills a city with AI cars and pedestrians that follow its road graph (made by 'world city').");
    A3_T_TRAFFIC = t;
    a3_component_type(t)->icon = "car";
    A3_REFLECT_FIELD(t, A3CTraffic, roads, A3_FIELD_STRING, "Roads", "Road graph JSON (Assets/City/roads.json), or \"generated\" for the City component's roads.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CTraffic, cars, A3_FIELD_I32, "Cars", "How many AI cars drive around."), 0, 400, 1);
    a3_field_range(A3_REFLECT_FIELD(t, A3CTraffic, pedestrians, A3_FIELD_I32, "Pedestrians", "How many people walk the sidewalks."), 0, 1000, 1);
    a3_field_range(A3_REFLECT_FIELD(t, A3CTraffic, speed_limit, A3_FIELD_F32, "Speed Limit", "Cruising speed of AI cars (m/s)."), 1, 60, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CTraffic, spawn_radius, A3_FIELD_F32, "Spawn Radius", "Spawn near the camera (m); 0 = anywhere in the city."), 0, 5000, 1);
    A3_REFLECT_FIELD(t, A3CTraffic, seed, A3_FIELD_U32, "Seed", "Same seed = same traffic.")->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CTraffic, spawned, A3_FIELD_BOOL, "Spawned", "Runtime.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CTraffic, active_cars, A3_FIELD_I32, "Active Cars", "Runtime.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CTraffic, active_pedestrians, A3_FIELD_I32, "Active Pedestrians", "Runtime.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;

    A3CTrafficAgent ag;
    a3_zero_struct(&ag);
    t = a3_component_register("TrafficAgent", "Gameplay", sizeof(A3CTrafficAgent), 16, &ag, A3_COMP_BUILTIN,
        "A car or pedestrian driven by the Traffic system (added automatically).");
    A3_T_TRAFFIC_AGENT = t;
    A3_REFLECT_FIELD(t, A3CTrafficAgent, kind, A3_FIELD_I32, "Kind", "0 = car, 1 = pedestrian.")->flags |= A3_FIELD_FLAG_TRANSIENT;
    A3_REFLECT_FIELD(t, A3CTrafficAgent, from, A3_FIELD_I32, "From", "Road node.")->flags |= A3_FIELD_FLAG_TRANSIENT;
    A3_REFLECT_FIELD(t, A3CTrafficAgent, to, A3_FIELD_I32, "To", "Road node.")->flags |= A3_FIELD_FLAG_TRANSIENT;
    A3_REFLECT_FIELD(t, A3CTrafficAgent, wait, A3_FIELD_F32, "Waiting", "Seconds stopped.")->flags |= A3_FIELD_FLAG_TRANSIENT;
}

/* ======================================================================== */
/* Spawning                                                                 */
/* ======================================================================== */

static const A3Vec4 k_paints[] = {
    { 0.85f, 0.1f, 0.12f, 1 }, { 0.95f, 0.95f, 0.95f, 1 }, { 0.05f, 0.05f, 0.06f, 1 }, { 0.1f, 0.3f, 0.75f, 1 },
    { 0.95f, 0.75f, 0.1f, 1 }, { 0.2f, 0.75f, 0.75f, 1 }, { 0.6f, 0.62f, 0.65f, 1 }, { 0.95f, 0.45f, 0.65f, 1 },
    { 0.15f, 0.45f, 0.2f, 1 }, { 0.45f, 0.2f, 0.6f, 1 },
};
static A3Vec3 v2to3(A3Vec2 p, f32 y) { return a3_v3(p.x, y, p.y); }

static A3Vec2 edge_dir(const A3RoadGraph *g, u32 a, u32 b) {
    A3Vec2 d = a3_v2(g->nodes[b].x - g->nodes[a].x, g->nodes[b].y - g->nodes[a].y);
    f32 l = a3_sqrtf(d.x * d.x + d.y * d.y);
    return l > 1e-4f ? a3_v2(d.x / l, d.y / l) : a3_v2(0, -1);
}
static A3Vec2 right_of(A3Vec2 d) { return a3_v2(-d.y, d.x); }
static f32 edge_len(const A3RoadGraph *g, u32 a, u32 b) {
    A3Vec2 d = a3_v2(g->nodes[b].x - g->nodes[a].x, g->nodes[b].y - g->nodes[a].y);
    return a3_sqrtf(d.x * d.x + d.y * d.y);
}

/* next node after arriving at `at` from `from`: random, no U-turn unless forced */
static u32 pick_next(const A3RoadGraph *g, A3Rng *rng, u32 from, u32 at) {
    u32 deg = degree(g, at);
    if (!deg) return from;
    u32 choices[16], nc = 0;
    for (u32 k = g->adj_start[at]; k < g->adj_start[at + 1] && nc < 16; ++k) if (g->adj[k] != from) choices[nc++] = g->adj[k];
    if (!nc) return from;
    return choices[a3_rng_range_u32(rng, nc)];
}

static A3Entity spawn_pedestrian(A3World *w, A3Vec3 pos, A3Rng *rng) {
    return a3_person_spawn(w, "Pedestrian", pos, 0, ((u64)a3_rng_u32(rng) << 32) | a3_rng_u32(rng));   /* jointed body, walk cycle (a3_people.c) */
}

static b32 spot_free(A3World *w, A3Vec3 p, f32 min_dist) {
    u32 n = 0;
    const A3Entity *ents = 0;
    a3_component_array(w, A3_T_TRAFFIC_AGENT, &n, &ents);
    for (u32 i = 0; i < n; ++i) {
        A3CTransform *t = a3_transform(w, ents[i]);
        if (t && a3_v3_len_sq(a3_v3_sub(t->position, p)) < min_dist * min_dist) return 0;
    }
    return 1;
}

static void spawn_all(A3World *w, A3CTraffic *tr, GraphCache *gc, A3Entity manager) {
    const A3RoadGraph *g = &gc->g;
    A3Rng rng;
    a3_rng_seed(&rng, tr->seed ? tr->seed : 1, 991);
    /* optional spawn area around the primary camera */
    A3Vec3 center = a3_v3_zero();
    b32 limit = tr->spawn_radius > 0;
    if (limit) {
        u32 n = 0;
        const A3Entity *ents = 0;
        A3CCamera *cams = (A3CCamera *)a3_component_array(w, A3_T_CAMERA, &n, &ents);
        for (u32 i = 0; i < n; ++i) if (cams[i].primary) { center = a3_transform_world_position(w, ents[i]); break; }
    }
    /* candidate edges: both ends in the main network (and inside the radius) */
    u32 total = 0;
    for (u32 a = 0; a < g->node_count; ++a) total += degree(g, a);
    u32 *cand = A3_NEW_ARRAY(u32, total + 1, A3_MEM_TEMP);
    if (!cand) return;
    u32 nc = 0;
    for (u32 a = 0; a < g->node_count; ++a) {
        if (gc->comp[a] != gc->main_comp) continue;
        if (limit) {
            f32 dx = g->nodes[a].x - center.x, dz = g->nodes[a].y - center.z;
            if (dx * dx + dz * dz > tr->spawn_radius * tr->spawn_radius) continue;
        }
        for (u32 k = g->adj_start[a]; k < g->adj_start[a + 1]; ++k) if (edge_len(g, a, g->adj[k]) > 20.0f) cand[nc++] = k;
    }
    if (!nc) { a3_free(cand); A3_WARN("traffic", "no roads to spawn traffic on"); return; }
    /* edge index k -> from node */
    for (i32 kind = 0; kind < 2; ++kind) {
        i32 want = kind == KIND_CAR ? tr->cars : tr->pedestrians;
        i32 made = 0;
        for (i32 attempt = 0; attempt < want * 8 && made < want; ++attempt) {
            u32 k = cand[a3_rng_range_u32(&rng, nc)];
            u32 from = 0;
            while (from + 1 < g->node_count && g->adj_start[from + 1] <= k) ++from;
            u32 to = g->adj[k];
            A3Vec2 d = edge_dir(g, from, to), r = right_of(d);
            f32 len = edge_len(g, from, to);
            f32 s = a3_rng_range_f32(&rng, g->half_width + 4.0f, a3_maxf(len - g->half_width - 4.0f, g->half_width + 4.1f));
            f32 lane;
            if (kind == KIND_CAR) {
                u32 lanes = lanes_between(g, from, to);
                lane = lanes > 1 && a3_rng_f32(&rng) < 0.5f ? g->half_width * 0.75f : g->half_width * (lanes > 1 ? 0.28f : 0.5f);
            } else {
                lane = (a3_rng_f32(&rng) < 0.5f ? -1.0f : 1.0f) * (g->half_width + a3_rng_range_f32(&rng, 1.2f, 2.8f));
            }
            A3Vec2 p2 = a3_v2(g->nodes[from].x + d.x * s + r.x * lane, g->nodes[from].y + d.y * s + r.y * lane);
            A3Vec3 p = v2to3(p2, kind == KIND_CAR ? g->road_y : g->walk_y);
            if (!spot_free(w, p, kind == KIND_CAR ? 12.0f : 2.0f)) continue;
            f32 yaw = a3_atan2f(-d.x, -d.y) * A3_RAD2DEG;
            A3Entity e;
            if (kind == KIND_CAR) {
                char name[32];
                a3_snprintf(name, sizeof(name), "Traffic Car %d", made + 1);
                /* a realistic mix: sedans, SUVs, hatchbacks, a few sports cars and yellow taxis */
                f32 pick = a3_rng_f32(&rng);
                const char *style = pick < 0.34f ? "sedan" : pick < 0.6f ? "suv" : pick < 0.8f ? "hatch" : pick < 0.88f ? "sports" : "taxi";
                A3Vec4 paint = k_paints[a3_rng_range_u32(&rng, A3_ARRAY_COUNT(k_paints))];
                if (a3_streq(style, "taxi")) paint = a3_v4(1.0f, 0.74f, 0.05f, 1);
                e = a3_vehicle_spawn_car_style(w, name, p, yaw, paint, style);
                A3CVehicle *v = (A3CVehicle *)a3_component_get(w, e, A3_T_VEHICLE);
                v->physics_model = A3_VEHICLE_ARCADE;    /* the traffic AI steers the simple model */
                /* dozens of cars: no engine / tire sound loops or smoke emitters for traffic */
                for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c);) {
                    A3Entity next = a3_entity_next_sibling(w, c);
                    const char *cn = a3_entity_name(w, c);
                    if (a3_streq(cn, "Engine Sound") || a3_streq(cn, "Tire Sound") || a3_str_starts_with(cn, "Tire Smoke")) a3_entity_destroy(w, c);
                    c = next;
                }
                v->ground_probe = 0;
                v->collide_world = 0;
                v->max_speed = tr->speed_limit * 1.6f;
                v->acceleration = 6.0f + a3_rng_f32(&rng) * 3.0f;
            } else {
                e = spawn_pedestrian(w, p, &rng);
                a3_transform(w, e)->rotation = a3_quat_axis_angle(a3_v3(0, 1, 0), yaw * A3_DEG2RAD);
            }
            a3_entity_set_parent(w, e, manager);
            A3CTrafficAgent *ag = (A3CTrafficAgent *)a3_component_add(w, e, A3_T_TRAFFIC_AGENT);
            ag->kind = kind;
            ag->from = (i32)from;
            ag->to = (i32)to;
            ag->next = (i32)pick_next(g, &rng, from, to);
            ag->lane = lane;
            ag->phase = a3_rng_f32(&rng) * 6.28f;
            ag->speed = a3_rng_range_f32(&rng, 1.1f, 1.7f);
            made++;
        }
        if (kind == KIND_CAR) tr->active_cars = made; else tr->active_pedestrians = made;
    }
    a3_free(cand);
    tr->spawned = 1;
}

/* ======================================================================== */
/* Driving                                                                  */
/* ======================================================================== */

typedef struct Obstacle { A3Vec3 p; A3Entity e; f32 radius; A3Vec3 vel; } Obstacle;   /* vel: vehicles only */

static A3Vec2 lane_point(const A3RoadGraph *g, u32 a, u32 b, f32 lane, f32 s) {
    A3Vec2 d = edge_dir(g, a, b), r = right_of(d);
    return a3_v2(g->nodes[a].x + d.x * s + r.x * lane, g->nodes[a].y + d.y * s + r.y * lane);
}

/* project p onto the lane line a->b: distance along the edge */
static f32 along_edge(const A3RoadGraph *g, u32 a, u32 b, A3Vec3 p) {
    A3Vec2 d = edge_dir(g, a, b);
    return (p.x - g->nodes[a].x) * d.x + (p.z - g->nodes[a].y) * d.y;
}

static void advance(const A3RoadGraph *g, A3Rng *rng, A3CTrafficAgent *ag) {
    u32 from = (u32)ag->to, to = (u32)ag->next;
    ag->from = (i32)from;
    ag->to = (i32)to;
    ag->next = (i32)pick_next(g, rng, from, to);
    ag->stage = 0;
    if (ag->kind == KIND_CAR) {
        u32 lanes = lanes_between(g, from, to);
        if (lanes <= 1) ag->lane = g->half_width * 0.5f;
        else if (ag->lane < g->half_width * 0.4f || ag->lane > g->half_width * 0.6f) { /* keep the lane it was in */ }
        else ag->lane = g->half_width * 0.28f;
    }
}

static void drive_car(A3World *w, const A3RoadGraph *g, A3Rng *rng, A3Entity e, A3CTrafficAgent *ag, A3CVehicle *v,
                      const A3CTraffic *tr, const Obstacle *obs, u32 nobs, f32 dt, f64 time) {
    A3CTransform *t = a3_transform(w, e);
    if (!t || (u32)ag->from >= g->node_count || (u32)ag->to >= g->node_count || (u32)ag->next >= g->node_count) return;
    u32 a = (u32)ag->from, b = (u32)ag->to, c = (u32)ag->next;
    f32 len = edge_len(g, a, b);
    f32 stop = g->half_width + 1.5f;
    f32 s = along_edge(g, a, b, t->position);
    A3Vec2 target;
    A3Vec2 d1 = edge_dir(g, a, b), d2 = edge_dir(g, b, c);
    f32 turn_cos = d1.x * d2.x + d1.y * d2.y;
    if (ag->stage == 0) {
        f32 look = 7.0f + a3_absf(v->speed) * 0.45f;
        target = lane_point(g, a, b, ag->lane, a3_minf(s + look, len - stop));
        if (s >= len - stop - 1.5f) ag->stage = 1;
    } else {
        f32 lane2 = lanes_between(g, b, c) > 1 && ag->lane > g->half_width * 0.6f ? ag->lane : (lanes_between(g, b, c) > 1 ? g->half_width * 0.28f : g->half_width * 0.5f);
        f32 enter = turn_cos > 0.7f ? stop : stop + 2.0f;
        target = lane_point(g, b, c, lane2, enter + 3.0f);
        f32 s2 = along_edge(g, b, c, t->position);
        if (s2 >= enter || (turn_cos < -0.9f && s2 >= 0)) { advance(g, rng, ag); }
    }
    /* steering: toward the target point */
    A3Vec2 to_t = a3_v2(target.x - t->position.x, target.y - t->position.z);
    f32 desired = a3_atan2f(-to_t.x, -to_t.y);
    f32 delta = a3_wrap_angle(desired - v->yaw);
    f32 range = a3_maxf(v->steer_angle * A3_DEG2RAD * (1.0f - 0.65f * a3_clampf(a3_absf(v->speed) / a3_maxf(v->max_speed, 1), 0, 1)), 0.05f);
    v->steer = a3_clampf(-delta / range, -1, 1);
    /* speed: limit, corners, obstacles ahead */
    f32 want = tr->speed_limit;
    f32 dist_to_b = len - s;
    if (turn_cos < 0.7f && (ag->stage == 1 || dist_to_b < 30.0f)) want = a3_minf(want, turn_cos < -0.5f ? 4.0f : 7.5f);
    if (a3_absf(delta) > 0.6f) want = a3_minf(want, 5.0f);
    A3Vec3 fwd = a3_v3(-a3_sinf(v->yaw), 0, -a3_cosf(v->yaw)), right = a3_v3(-fwd.z, 0, fwd.x);
    f32 gap = 1e9f;
    if (ag->ignore <= 0) {
        for (u32 i = 0; i < nobs; ++i) {
            if (a3_entity_eq(obs[i].e, e)) continue;
            A3Vec3 rel = a3_v3_sub(obs[i].p, t->position);
            f32 along = a3_v3_dot(rel, fwd);
            if (along <= 0 || along > 32.0f) continue;
            f32 lat = a3_absf(a3_v3_dot(rel, right)) - obs[i].radius;
            if (lat < 1.3f + a3_clampf(v->steer * v->steer, 0, 1) * 1.5f) gap = a3_minf(gap, along - obs[i].radius);
        }
    } else ag->ignore -= dt;
    if (gap < 1e8f) want = a3_minf(want, a3_maxf((gap - 6.5f) * 0.8f, 0.0f));
    /* traffic signals at the next intersection: stop at the line on red (and on amber when there is room) */
    if (ag->stage == 0 && g->signal && g->signal[b]) {
        f32 to_line = (len - stop - 1.0f) - s;
        if (to_line > -0.5f && to_line < 45.0f) {
            i32 group = a3_absf(d1.x) > a3_absf(d1.y) ? 1 : 0;
            i32 state = a3_city_signal_state(time, a3_city_signal_phase(g->nodes[b].x, g->nodes[b].y), group);
            if (state == 2 || (state == 1 && to_line > 8.0f + v->speed * 0.3f))
                want = a3_minf(want, a3_maxf((to_line - 1.0f) * 0.55f, 0.0f));
        }
    }
    f32 err = want - v->speed;
    v->throttle = err > 0 ? a3_clampf(err * 0.35f, 0, 1) : a3_clampf(err * 0.5f, -1, 0);
    if (want < 0.1f && v->speed < 0.5f) v->throttle = 0;
    v->handbrake = 0;
    /* jam breaker: after waiting a while, creep through */
    if (v->speed < 0.3f && gap < 12.0f) {
        ag->wait += dt;
        if (ag->wait > 4.0f) { ag->ignore = 1.5f; ag->wait = 0; }
    } else ag->wait = 0;
}

/* A car that will pass within ~2.5 m in the next 1.5 s: returns the direction
 * to jump out of its way (away from its path). */
static b32 ped_threat(A3Vec3 p, const Obstacle *obs, u32 nobs, A3Vec2 *away) {
    for (u32 i = 0; i < nobs; ++i) {
        A3Vec2 v = a3_v2(obs[i].vel.x, obs[i].vel.z);
        f32 v2 = v.x * v.x + v.y * v.y;
        if (v2 < 16.0f) continue;                                  /* slower than ~15 km/h: no panic */
        A3Vec2 rel = a3_v2(p.x - obs[i].p.x, p.z - obs[i].p.z);
        f32 ahead = rel.x * v.x + rel.y * v.y;
        if (ahead <= 0) continue;                                  /* behind the car */
        f32 tca = a3_minf(ahead / v2, 1.5f);
        A3Vec2 cl = a3_v2(rel.x - v.x * tca, rel.y - v.y * tca);
        f32 d = a3_sqrtf(cl.x * cl.x + cl.y * cl.y);
        if (d > 2.6f) continue;
        f32 vl = a3_sqrtf(v2);
        *away = d > 0.2f ? a3_v2(cl.x / d, cl.y / d) : a3_v2(-v.y / vl, v.x / vl);
        return 1;
    }
    return 0;
}

static void walk_ped(A3World *w, const A3RoadGraph *g, A3Rng *rng, A3Entity e, A3CTrafficAgent *ag, const Obstacle *obs, u32 nobs, f32 dt) {
    A3CTransform *t = a3_transform(w, e);
    if (!t || (u32)ag->from >= g->node_count || (u32)ag->to >= g->node_count || (u32)ag->next >= g->node_count) return;
    A3Vec2 away;
    if (ped_threat(t->position, obs, nobs, &away)) {
        /* run sideways out of the car's path (the Person body switches to a run) */
        t->position.x += away.x * 5.5f * dt;
        t->position.z += away.y * 5.5f * dt;
        t->rotation = a3_quat_slerp(t->rotation, a3_quat_axis_angle(a3_v3(0, 1, 0), a3_atan2f(-away.x, -away.y)), a3_minf(1.0f, dt * 12.0f));
        t->position.y = g->walk_y;
        return;
    }
    u32 a = (u32)ag->from, b = (u32)ag->to, c = (u32)ag->next;
    f32 len = edge_len(g, a, b);
    f32 corner = a3_absf(ag->lane);
    A3Vec2 target;
    if (ag->stage == 0) {
        target = lane_point(g, a, b, ag->lane, len - corner);
        A3Vec2 dd = a3_v2(target.x - t->position.x, target.y - t->position.z);
        if (dd.x * dd.x + dd.y * dd.y < 0.36f) ag->stage = 1;
    } else {
        target = lane_point(g, b, c, ag->lane, corner);
        A3Vec2 dd = a3_v2(target.x - t->position.x, target.y - t->position.z);
        if (dd.x * dd.x + dd.y * dd.y < 0.36f) advance(g, rng, ag);
    }
    A3Vec2 dd = a3_v2(target.x - t->position.x, target.y - t->position.z);
    f32 dl = a3_sqrtf(dd.x * dd.x + dd.y * dd.y);
    if (dl > 1e-3f) {
        f32 step = a3_minf(ag->speed * dt, dl);
        t->position.x += dd.x / dl * step;
        t->position.z += dd.y / dl * step;
        f32 yaw = a3_atan2f(-dd.x, -dd.y);
        t->rotation = a3_quat_slerp(t->rotation, a3_quat_axis_angle(a3_v3(0, 1, 0), yaw), a3_minf(1.0f, dt * 8.0f));
    }
    t->position.y = g->walk_y;      /* the Person component animates the body from this motion */
}

void a3_traffic_update(A3World *w, f32 dt, f64 time) {
    if (A3_T_TRAFFIC == 0xFFFFFFFFu || dt <= 0) return;
    u32 nm = 0;
    const A3Entity *mgrs = 0;
    a3_component_array(w, A3_T_TRAFFIC, &nm, &mgrs);
    for (u32 mi = 0; mi < nm; ++mi) {
        A3Entity me = mgrs[mi];
        A3CTraffic *tr = (A3CTraffic *)a3_component_get(w, me, A3_T_TRAFFIC);
        if (!tr || !a3_entity_active(w, me)) continue;
        GraphCache *gc = graph_for(w, tr->roads);
        if (!gc) continue;
        if (!tr->spawned) { spawn_all(w, tr, gc, me); if (!tr->spawned) { tr->spawned = 1; continue; } }
        /* obstacles: every vehicle, pedestrian and character */
        u32 nv = 0, na = 0, nc = 0;
        const A3Entity *vents = 0, *aents = 0, *cents = 0;
        a3_component_array(w, A3_T_VEHICLE, &nv, &vents);
        a3_component_array(w, A3_T_TRAFFIC_AGENT, &na, &aents);
        a3_component_array(w, A3_T_CHARACTER, &nc, &cents);
        Obstacle *obs = A3_NEW_ARRAY(Obstacle, nv + na + nc + 1, A3_MEM_TEMP);
        if (!obs) continue;
        u32 no = 0;
        for (u32 i = 0; i < nv; ++i) {
            A3CTransform *t = a3_transform(w, vents[i]);
            const A3CVehicle *vv = (const A3CVehicle *)a3_component_get(w, vents[i], A3_T_VEHICLE);
            if (t) { obs[no].p = t->position; obs[no].e = vents[i]; obs[no].radius = 1.0f; obs[no].vel = vv ? vv->velocity : a3_v3_zero(); no++; }
        }
        for (u32 i = 0; i < na; ++i) {
            A3CTrafficAgent *ag = (A3CTrafficAgent *)a3_component_get(w, aents[i], A3_T_TRAFFIC_AGENT);
            A3CTransform *t = a3_transform(w, aents[i]);
            if (ag && ag->kind == KIND_PED && t) { obs[no].p = t->position; obs[no].e = aents[i]; obs[no].radius = 0.4f; obs[no].vel = a3_v3_zero(); no++; }
        }
        for (u32 i = 0; i < nc; ++i) { if (!a3_entity_active(w, cents[i])) continue; obs[no].p = a3_transform_world_position(w, cents[i]); obs[no].e = cents[i]; obs[no].radius = 0.4f; obs[no].vel = a3_v3_zero(); no++; }
        A3Rng *rng = &gc->rng;
        for (u32 i = 0; i < na; ++i) {
            A3Entity e = aents[i];
            if (!a3_entity_active(w, e) || !a3_entity_eq(a3_entity_parent(w, e), me)) continue;
            A3CTrafficAgent *ag = (A3CTrafficAgent *)a3_component_get(w, e, A3_T_TRAFFIC_AGENT);
            if (!ag) continue;
            if (ag->kind == KIND_CAR) {
                A3CVehicle *v = (A3CVehicle *)a3_component_get(w, e, A3_T_VEHICLE);
                if (v && !v->use_input) drive_car(w, &gc->g, rng, e, ag, v, tr, obs, no, dt, time);
            } else {
                walk_ped(w, &gc->g, rng, e, ag, obs, no, dt);
            }
        }
        a3_free(obs);
    }
}

/* ======================================================================== */
/* Road queries (scripts, missions)                                         */
/* ======================================================================== */

static GraphCache *world_graph(A3World *w) {
    if (A3_T_TRAFFIC != 0xFFFFFFFFu) {
        u32 n = 0;
        const A3Entity *ents = 0;
        A3CTraffic *trs = (A3CTraffic *)a3_component_array(w, A3_T_TRAFFIC, &n, &ents);
        for (u32 i = 0; i < n; ++i) { GraphCache *gc = graph_for(w, trs[i].roads); if (gc) return gc; }
    }
    return a3_city_generated_roads(w, 0) ? graph_for(w, "generated") : 0;
}

const A3RoadGraph *a3_traffic_graph(A3World *w) { GraphCache *gc = world_graph(w); return gc ? &gc->g : 0; }

b32 a3_traffic_random_road_point(A3World *w, A3Vec3 center, f32 min_d, f32 max_d, A3Vec3 *out, A3Vec3 *dir) {
    GraphCache *gc = world_graph(w);
    if (!gc || !gc->g.node_count) return 0;
    const A3RoadGraph *g = &gc->g;
    b32 have = 0;
    f32 best_err = 1e30f;
    for (u32 attempt = 0; attempt < 400; ++attempt) {
        u32 a = a3_rng_range_u32(&gc->rng, g->node_count);
        if (gc->comp[a] != gc->main_comp || !degree(g, a)) continue;
        u32 b = g->adj[g->adj_start[a] + a3_rng_range_u32(&gc->rng, degree(g, a))];
        f32 len = edge_len(g, a, b);
        if (len < g->half_width * 2.0f + 4.0f) continue;
        f32 s = a3_rng_range_f32(&gc->rng, g->half_width + 2.0f, len - g->half_width - 2.0f);
        A3Vec2 d = edge_dir(g, a, b);
        A3Vec3 p = a3_v3(g->nodes[a].x + d.x * s, g->road_y, g->nodes[a].y + d.y * s);
        f32 dist = a3_sqrtf((p.x - center.x) * (p.x - center.x) + (p.z - center.z) * (p.z - center.z));
        f32 err = dist < min_d ? min_d - dist : dist > max_d ? dist - max_d : 0.0f;
        if (err < best_err) {
            best_err = err;
            *out = p;
            if (dir) *dir = a3_v3(d.x, 0, d.y);
            have = 1;
            if (err == 0) break;
        }
    }
    return have;
}

b32 a3_traffic_nearest_road_point(A3World *w, A3Vec3 p, A3Vec3 *out, A3Vec3 *dir) {
    GraphCache *gc = world_graph(w);
    if (!gc) return 0;
    const A3RoadGraph *g = &gc->g;
    f32 best = 1e30f;
    b32 have = 0;
    for (u32 a = 0; a < g->node_count; ++a) {
        for (u32 k = g->adj_start[a]; k < g->adj_start[a + 1]; ++k) {
            u32 b = g->adj[k];
            if (b < a) continue;
            A3Vec2 d = edge_dir(g, a, b);
            f32 len = edge_len(g, a, b);
            f32 s = a3_clampf((p.x - g->nodes[a].x) * d.x + (p.z - g->nodes[a].y) * d.y, 0, len);
            A3Vec3 q = a3_v3(g->nodes[a].x + d.x * s, g->road_y, g->nodes[a].y + d.y * s);
            f32 dd = (q.x - p.x) * (q.x - p.x) + (q.z - p.z) * (q.z - p.z);
            if (dd < best) { best = dd; *out = q; if (dir) *dir = a3_v3(d.x, 0, d.y); have = 1; }
        }
    }
    return have;
}
