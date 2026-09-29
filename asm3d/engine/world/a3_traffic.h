/*
 * ASM3D - a3_traffic.h
 * City life: AI cars that drive the road graph and pedestrians that walk the
 * sidewalks. Put a Traffic component on one object in a city scene; when the
 * game starts it loads the road graph (asm3d.roads JSON, written by
 * 'asm3d_cli world city') and spawns the cars and people.
 *
 * Cars drive on the right, pick a random way at each intersection (no
 * U-turns unless it is a dead end), slow down for corners, keep a distance
 * to anything ahead of them (other cars, the player's car, pedestrians) and
 * give up waiting after a few seconds so intersections never lock up.
 * Pedestrians walk the sidewalks and turn at corners.
 */
#ifndef A3_TRAFFIC_H
#define A3_TRAFFIC_H

#include "../ecs/a3_ecs.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

extern u32 A3_T_TRAFFIC;
extern u32 A3_T_TRAFFIC_AGENT;

typedef struct A3CTraffic {
    char roads[A3_NAME_MAX]; /* project path of the road graph JSON, or "generated" */
    i32 cars;
    i32 pedestrians;
    u32 seed;
    f32 speed_limit;        /* m/s */
    f32 spawn_radius;       /* around the primary camera; 0 = anywhere */
    b32 spawned;            /* runtime */
    i32 active_cars;        /* runtime */
    i32 active_pedestrians; /* runtime */
    f32 _pad[3];
} A3CTraffic;

typedef struct A3CTrafficAgent {
    i32 kind;               /* 0 = car, 1 = pedestrian */
    i32 from, to, next;     /* road graph nodes */
    f32 lane;               /* offset to the right of the road center (m) */
    f32 wait;               /* seconds stopped behind something */
    f32 ignore;             /* seconds to ignore obstacles (unlocks jams) */
    f32 phase;              /* pedestrian walk cycle */
    i32 stage;              /* 0 = along the edge, 1 = turning through the intersection */
    f32 speed;              /* pedestrian walking speed */
    f32 _pad[2];
} A3CTrafficAgent;

void a3_traffic_register(void);
/* Spawns (first call) and drives all traffic in the world. */
void a3_traffic_update(A3World *w, f32 dt, f64 time);   /* time: play time (signal cycle) */

/* Road graph access (loaded by the Traffic component; for tests and tools). */
typedef struct A3RoadGraph {
    A3Vec2 *nodes;
    u32 node_count;
    u32 *adj_start;         /* node_count + 1 */
    u32 *adj;               /* neighbor node indices */
    u8 *adj_lanes;
    u8 *signal;             /* per node: 1 = traffic signals */
    f32 road_y, walk_y, half_width;
} A3RoadGraph;

b32  a3_road_graph_parse(const char *json, usize len, A3RoadGraph *g);
/* The world's road network (from its Traffic component, else the generated city). */
const A3RoadGraph *a3_traffic_graph(A3World *w);
/* A random point on a road center line, min..max meters from center (dir = along the road). */
b32  a3_traffic_random_road_point(A3World *w, A3Vec3 center, f32 min_dist, f32 max_dist, A3Vec3 *out, A3Vec3 *dir);
/* The closest point on any road center line. */
b32  a3_traffic_nearest_road_point(A3World *w, A3Vec3 p, A3Vec3 *out, A3Vec3 *dir);
void a3_road_graph_free(A3RoadGraph *g);

A3_EXTERN_C_END

#endif
