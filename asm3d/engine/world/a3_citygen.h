/*
 * ASM3D - a3_citygen.h
 * Procedural coastal city: "Sol Harbor", a fictional city whose layout is
 * inspired by Miami's geography (approximate, not a map):
 *
 *   west  mainland: glass towers downtown by the bay, a colorful low-rise
 *         district further west, warehouses of an arts district to the north
 *   bay   crossed by three causeways, a chain of small residential islands
 *   east  barrier island: pastel Art Deco hotels with neon on the beachfront
 *         drive, condo towers further north, palms, a wide beach, the ocean
 *   port  island with container stacks and cranes south of downtown
 *
 * +X is east, -Z is north, 1 unit = 1 meter. The city is made of regular
 * entities (boxes, cylinders, lights) with the builtin: procedural materials,
 * so it renders and collides with no extra assets. A road graph (nodes at
 * intersections, two-way edges) is produced for traffic.
 */
#ifndef A3_CITYGEN_H
#define A3_CITYGEN_H

#include "../ecs/a3_ecs.h"
#include "../core/a3_strbuf.h"

A3_EXTERN_C_BEGIN

typedef enum A3CityTime { A3_CITY_DAY = 0, A3_CITY_SUNSET, A3_CITY_NIGHT } A3CityTime;

typedef struct A3CityDesc {
    u64 seed;
    f32 density;          /* 0.25..1: fraction of lots with buildings (1 = full city) */
    i32 time;             /* A3CityTime: sky, sun and post-processing look */
    b32 street_lights;
    b32 neon;             /* neon trim lights on the beachfront (night) */
} A3CityDesc;

typedef struct A3CityStats {
    u32 entities, buildings, towers, hotels, roads, lights, palms, props;
    u32 road_nodes, road_edges;
    f32 min_x, max_x, min_z, max_z;   /* land extent */
} A3CityStats;

void a3_city_desc_default(A3CityDesc *d);
/* Adds the city to `w`. The road graph is appended to graph_json when given:
 * {"format":"asm3d.roads","nodes":[[x,z],...],"edges":[[a,b,lanes],...]} */
b32  a3_city_generate(A3World *w, const A3CityDesc *d, A3CityStats *stats, A3StrBuf *graph_json);
/* Applies the sky / sun / fog / post look for a time of day to the world. */
void a3_city_apply_time(A3World *w, A3CityTime t);
A3CityTime a3_city_time_from_name(const char *name);   /* "day", "sunset", "night" */

/* City component: generates the city when the game starts, so a scene can
 * hold a whole city in a few lines. The generated road graph stays in memory
 * for the Traffic component (Roads = "generated"). */
extern u32 A3_T_CITY;
typedef struct A3CCity {
    u32 seed;
    f32 density;
    i32 time;               /* A3CityTime */
    b32 street_lights;
    b32 neon;
    b32 generated;          /* runtime */
    f32 _pad[2];
} A3CCity;

void a3_city_register(void);
/* Generates every City component that has not been generated yet. */
void a3_city_update(A3World *w);
/* Road graph JSON of the city generated in this world (NULL if none). */
const char *a3_city_generated_roads(A3World *w, usize *len);

A3_EXTERN_C_END

#endif
