/*
 * ASM3D - a3_citygen.c
 * Procedural coastal city (see a3_citygen.h).
 */
#include "a3_citygen.h"
#include "a3_procmeshes.h"
#include "../scene/a3_components.h"
#include "../physics/a3_physics.h"
#include "../core/a3_hash.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_json.h"
#include "../core/a3_log.h"
#include "../platform/a3_platform.h"

#define LAND_TOP 0.4f
#define ROAD_TOP 0.5f
#define WALK_TOP 0.65f

typedef enum District { D_DOWNTOWN = 0, D_MIDRISE, D_LOWRISE, D_WAREHOUSE, D_HOTEL, D_CONDO, D_PARK, D_BAYSIDE } District;

typedef struct Gen {
    A3World *w;
    A3Rng rng;
    const A3CityDesc *d;
    A3CityStats *st;
    A3Entity root, g_land, g_roads, g_blocks, g_build, g_lights, g_palms, g_props;
    A3_ARRAY_TYPE(A3Vec2) nodes;
    A3_ARRAY_TYPE(u32) edges;     /* a, b, lanes */
    A3HashMap node_map;
    u32 light_count;
} Gen;

void a3_city_desc_default(A3CityDesc *d) {
    a3_zero_struct(d);
    d->seed = 1981;
    d->density = 1.0f;
    d->time = A3_CITY_NIGHT;
    d->street_lights = 1;
    d->neon = 1;
}

static f32 rf(Gen *g, f32 a, f32 b) { return a3_rng_range_f32(&g->rng, a, b); }
static i32 ri(Gen *g, i32 a, i32 b) { return a3_rng_range_i32(&g->rng, a, b); }
static b32 chance(Gen *g, f32 p) { return a3_rng_f32(&g->rng) < p; }

static A3Entity ent(Gen *g, const char *name, A3Entity parent, A3Vec3 pos, A3Vec3 scale, f32 yaw) {
    A3Entity e = a3_entity_create(g->w, name);
    if (!a3_entity_is_null(parent)) a3_entity_set_parent(g->w, e, parent);
    A3CTransform *t = (A3CTransform *)a3_component_add(g->w, e, A3_T_TRANSFORM);
    t->position = pos;
    t->scale = scale;
    t->rotation = a3_quat_euler(0, yaw * A3_DEG2RAD, 0);
    g->st->entities++;
    return e;
}

static A3CMeshRenderer *mesh(Gen *g, A3Entity e, i32 prim, const char *material, A3Vec4 color) {
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(g->w, e, A3_T_MESH_RENDERER);
    mr->primitive = prim;
    mr->base_color = color;
    mr->roughness = 0.7f;
    mr->metallic = 0;
    if (material) a3_strcpy(mr->material.path, sizeof(mr->material.path), material);
    return mr;
}

static void collide(Gen *g, A3Entity e, i32 shape) {
    A3CCollider *c = (A3CCollider *)a3_component_add(g->w, e, A3_T_COLLIDER);
    c->shape = shape;
    c->size = a3_v3_one();
    c->radius = 0.5f;
    c->height = 1.0f;
}

/* box from its min corner at y = base and size */
static A3Entity box(Gen *g, A3Entity parent, const char *name, A3Vec3 center, A3Vec3 size, f32 yaw, const char *material, A3Vec4 color, b32 solid) {
    A3Entity e = ent(g, name, parent, center, size, yaw);
    mesh(g, e, A3_PRIM_CUBE, material, color);
    if (solid) collide(g, e, A3_SHAPE_BOX);
    return e;
}

static A3Vec4 rgb(f32 r, f32 gg, f32 b) { return a3_v4(r, gg, b, 1); }

/* ======================================================================== */
/* Road graph                                                               */
/* ======================================================================== */

static u32 node(Gen *g, f32 x, f32 z) {
    u64 key = a3_hash_combine((u64)(i64)a3_roundf(x * 2), (u64)(i64)a3_roundf(z * 2)) | 1, v;
    if (a3_hashmap_get(&g->node_map, key, &v)) return (u32)v - 1;
    a3_array_push(g->nodes, a3_v2(x, z), A3_MEM_WORLD);
    a3_hashmap_put(&g->node_map, key, g->nodes.count);
    return g->nodes.count - 1;
}

static void edge(Gen *g, u32 a, u32 b, u32 lanes) {
    if (a == b) return;
    a3_array_push(g->edges, a, A3_MEM_WORLD);
    a3_array_push(g->edges, b, A3_MEM_WORLD);
    a3_array_push(g->edges, lanes, A3_MEM_WORLD);
}

/* ======================================================================== */
/* Props                                                                    */
/* ======================================================================== */

static void street_light(Gen *g, A3Vec3 base, f32 yaw) {
    if (!g->d->street_lights) return;
    A3Entity pole = box(g, g->g_lights, "Lamp Post", a3_v3(base.x, base.y + 3.6f, base.z), a3_v3(0.16f, 7.2f, 0.16f), yaw, "builtin:metal", rgb(0.18f, 0.19f, 0.2f), 0);
    A3_UNUSED(pole);
    A3Quat q = a3_quat_euler(0, yaw * A3_DEG2RAD, 0);
    A3Vec3 arm = a3_quat_rotate(q, a3_v3(0, 0, -1.3f));
    A3Entity lamp = box(g, g->g_lights, "Lamp", a3_v3(base.x + arm.x, base.y + 7.1f, base.z + arm.z), a3_v3(0.5f, 0.12f, 0.9f), yaw, "builtin:neon", rgb(1.0f, 0.78f, 0.5f), 0);
    A3CLight *l = (A3CLight *)a3_component_add(g->w, lamp, A3_T_LIGHT);
    l->type = A3_LIGHT_POINT;
    l->color = a3_v4(1.0f, 0.72f, 0.45f, 1);
    l->intensity = 14.0f;    /* radiance falls off as 1/(d^2+1) */
    l->range = 22.0f;
    g->st->lights++;
}

static void neon_light(Gen *g, A3Vec3 p, A3Vec4 color, f32 range) {
    A3Entity e = ent(g, "Neon Glow", g->g_lights, p, a3_v3_one(), 0);
    A3CLight *l = (A3CLight *)a3_component_add(g->w, e, A3_T_LIGHT);
    l->type = A3_LIGHT_POINT;
    l->color = color;
    l->intensity = 14.0f;
    l->range = range;
    g->st->lights++;
}

static void palm(Gen *g, A3Vec3 base, f32 height) {
    f32 lean = rf(g, -7, 7), yaw = rf(g, 0, 360);
    A3Entity trunk = ent(g, "Palm", g->g_palms, a3_v3(base.x, base.y + height * 0.5f, base.z), a3_v3(0.36f, height, 0.36f), yaw);
    A3CTransform *tt = a3_transform(g->w, trunk);
    tt->rotation = a3_quat_euler(lean * A3_DEG2RAD, yaw * A3_DEG2RAD, 0);
    mesh(g, trunk, A3_PRIM_CYLINDER, "builtin:palm_trunk", rgb(1, 1, 1));
    collide(g, trunk, A3_SHAPE_BOX);
    A3Vec3 top = a3_v3_add(base, a3_quat_rotate(tt->rotation, a3_v3(0, height, 0)));
    A3Entity crown = ent(g, "Palm Crown", g->g_palms, top, a3_v3s(height * 0.55f), yaw);
    A3CMeshRenderer *mr = mesh(g, crown, A3_PRIM_NONE, "builtin:foliage", rgb(0.2f + rf(g, 0, 0.08f), 0.42f + rf(g, 0, 0.1f), 0.14f));
    a3_strcpy(mr->mesh.path, sizeof(mr->mesh.path), "builtin:palm_crown");
    g->st->palms++;
}

static void lifeguard_tower(Gen *g, A3Vec3 p, A3Vec4 color) {
    for (int i = 0; i < 4; ++i) {
        f32 dx = (i & 1) ? 1.1f : -1.1f, dz = (i & 2) ? 1.1f : -1.1f;
        box(g, g->g_props, "Tower Leg", a3_v3(p.x + dx, p.y + 1.1f, p.z + dz), a3_v3(0.15f, 2.2f, 0.15f), 0, "builtin:metal", rgb(0.9f, 0.9f, 0.9f), 0);
    }
    box(g, g->g_props, "Lifeguard Tower", a3_v3(p.x, p.y + 3.2f, p.z), a3_v3(3.0f, 2.2f, 3.0f), 0, "builtin:building", color, 1);
    box(g, g->g_props, "Tower Roof", a3_v3(p.x, p.y + 4.45f, p.z), a3_v3(3.6f, 0.3f, 3.6f), 0, 0, rgb(0.95f, 0.95f, 0.9f), 0);
    g->st->props++;
}

/* ======================================================================== */
/* Buildings                                                                */
/* ======================================================================== */

static const A3Vec4 k_pastels[] = {
    { 0.98f, 0.70f, 0.76f, 1 }, { 0.62f, 0.90f, 0.82f, 1 }, { 0.78f, 0.72f, 0.95f, 1 }, { 0.62f, 0.82f, 0.97f, 1 },
    { 0.99f, 0.92f, 0.62f, 1 }, { 0.98f, 0.97f, 0.93f, 1 }, { 0.99f, 0.80f, 0.62f, 1 },
};
static const A3Vec4 k_walls[] = {
    { 0.82f, 0.78f, 0.70f, 1 }, { 0.90f, 0.89f, 0.86f, 1 }, { 0.72f, 0.70f, 0.68f, 1 }, { 0.86f, 0.74f, 0.66f, 1 }, { 0.62f, 0.60f, 0.58f, 1 },
};
static const A3Vec4 k_vivid[] = {
    { 0.95f, 0.35f, 0.45f, 1 }, { 0.25f, 0.65f, 0.95f, 1 }, { 0.98f, 0.78f, 0.2f, 1 }, { 0.45f, 0.85f, 0.4f, 1 }, { 0.75f, 0.4f, 0.9f, 1 }, { 0.98f, 0.55f, 0.2f, 1 },
};
static const A3Vec4 k_neon[] = { { 1.0f, 0.15f, 0.6f, 1 }, { 0.1f, 0.9f, 1.0f, 1 }, { 0.65f, 0.25f, 1.0f, 1 }, { 1.0f, 0.55f, 0.1f, 1 } };

static void building(Gen *g, f32 cx, f32 cz, f32 w, f32 dpt, f32 h, const char *mat, A3Vec4 color, const char *name) {
    box(g, g->g_build, name, a3_v3(cx, WALK_TOP + h * 0.5f, cz), a3_v3(w, h, dpt), 0, mat, color, 1);
    g->st->buildings++;
}

static void tower(Gen *g, f32 cx, f32 cz, f32 w, f32 dpt, f32 h) {
    static const A3Vec4 tints[] = { { 0.35f, 0.55f, 0.75f, 1 }, { 0.3f, 0.6f, 0.6f, 1 }, { 0.55f, 0.6f, 0.68f, 1 }, { 0.25f, 0.4f, 0.6f, 1 } };
    A3Vec4 tint = tints[ri(g, 0, 3)];
    f32 podium = chance(g, 0.5f) ? rf(g, 8, 16) : 0;
    if (podium > 0) building(g, cx, cz, w * 1.25f, dpt * 1.25f, podium, "builtin:building", k_walls[ri(g, 0, 4)], "Podium");
    building(g, cx, cz, w, dpt, h, "builtin:tower", tint, "Tower");
    /* setback crown on some towers */
    if (chance(g, 0.45f)) box(g, g->g_build, "Tower Crown", a3_v3(cx, WALK_TOP + h + 6, cz), a3_v3(w * 0.6f, 12, dpt * 0.6f), 0, "builtin:tower", tint, 1);
    g->st->towers++;
}

static void hotel(Gen *g, f32 cx, f32 cz, f32 w, f32 dpt, f32 h, f32 facade_x) {
    A3Vec4 c = k_pastels[ri(g, 0, (i32)A3_ARRAY_COUNT(k_pastels) - 1)];
    building(g, cx, cz, w, dpt, h, "builtin:artdeco", c, "Hotel");
    /* vertical neon sign on the facade + colored glow */
    if (g->d->neon) {
        A3Vec4 n = k_neon[ri(g, 0, 3)];
        f32 sh = h * rf(g, 0.55f, 0.8f);
        box(g, g->g_build, "Neon Sign", a3_v3(facade_x + 0.35f, WALK_TOP + h * 0.5f + 1.0f, cz + rf(g, -w * 0.3f, w * 0.3f)), a3_v3(0.3f, sh, 1.2f), 0, "builtin:neon", n, 0);
        box(g, g->g_build, "Neon Band", a3_v3(facade_x + 0.25f, WALK_TOP + 3.6f, cz), a3_v3(0.2f, 0.35f, w * 0.9f), 0, "builtin:neon", k_neon[ri(g, 0, 3)], 0);
        neon_light(g, a3_v3(facade_x + 3.0f, WALK_TOP + 4.5f, cz), n, 14.0f);
    }
    g->st->hotels++;
}

static void fill_block(Gen *g, f32 x0, f32 z0, f32 x1, f32 z1, District d) {
    f32 w = x1 - x0, dp = z1 - z0;
    if (w < 6 || dp < 6) return;
    f32 cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
    b32 park = d == D_PARK;
    box(g, g->g_blocks, park ? "Park" : "Block", a3_v3(cx, (LAND_TOP + WALK_TOP) * 0.5f, cz), a3_v3(w, WALK_TOP - LAND_TOP, dp), 0,
        park ? "builtin:foliage" : "builtin:sidewalk", park ? rgb(0.22f, 0.4f, 0.18f) : rgb(0.66f, 0.65f, 0.62f), 1);
    f32 m = 4.0f;           /* setback from the curb */
    x0 += m; x1 -= m; z0 += m; z1 -= m;
    w = x1 - x0; dp = z1 - z0;
    f32 density = a3_clampf(g->d->density, 0.05f, 1.0f);
    switch (d) {
    case D_PARK:
        for (int i = 0; i < 6; ++i) palm(g, a3_v3(rf(g, x0, x1), WALK_TOP, rf(g, z0, z1)), rf(g, 7, 11));
        break;
    case D_DOWNTOWN: {
        u32 n = (u32)ri(g, 1, 3);
        for (u32 i = 0; i < n; ++i) {
            if (!chance(g, density)) continue;
            f32 tw = rf(g, 22, a3_minf(w / (n > 1 ? 1.7f : 1.1f), 40)), td = rf(g, 22, a3_minf(dp, 40));
            f32 px = n == 1 ? cx : x0 + tw * 0.5f + (w - tw) * (f32)i / (f32)(n - 1 ? n - 1 : 1);
            f32 pz = rf(g, z0 + td * 0.5f, a3_maxf(z0 + td * 0.5f, z1 - td * 0.5f));
            f32 bay = a3_clampf((cx + 400.0f) / 400.0f, 0, 1);   /* taller toward the bay */
            tower(g, px, pz, tw, td, rf(g, 50, 110) + bay * rf(g, 40, 130));
        }
    } break;
    case D_CONDO:
        if (chance(g, density)) building(g, cx, cz, a3_minf(w, 36), a3_minf(dp, 26), rf(g, 45, 125), "builtin:building", rgb(0.93f, 0.93f, 0.9f), "Condo");
        break;
    case D_MIDRISE: case D_BAYSIDE: {
        for (int ix = 0; ix < 2; ++ix) for (int iz = 0; iz < 2; ++iz) {
            if (!chance(g, density * 0.9f)) continue;
            f32 lw = w * 0.5f, ld = dp * 0.5f;
            f32 bw = rf(g, lw * 0.6f, lw * 0.92f), bd = rf(g, ld * 0.6f, ld * 0.92f);
            building(g, x0 + lw * (ix + 0.5f), z0 + ld * (iz + 0.5f), bw, bd, d == D_BAYSIDE ? rf(g, 8, 20) : rf(g, 12, 46), "builtin:building", k_walls[ri(g, 0, 4)], "Building");
        }
    } break;
    case D_LOWRISE: {
        for (int ix = 0; ix < 3; ++ix) for (int iz = 0; iz < 3; ++iz) {
            if ((ix == 1 && iz == 1) || !chance(g, density)) continue;   /* courtyard */
            f32 lw = w / 3.0f, ld = dp / 3.0f;
            f32 bx = x0 + lw * (ix + 0.5f), bz = z0 + ld * (iz + 0.5f), bh = rf(g, 4.5f, 10);
            building(g, bx, bz, lw * 0.92f, ld * 0.92f, bh, "builtin:building", chance(g, 0.6f) ? k_vivid[ri(g, 0, 5)] : k_pastels[ri(g, 0, 6)], "Shop");
            if (g->d->neon && chance(g, 0.2f)) {
                A3Vec4 n = k_neon[ri(g, 0, 3)];
                box(g, g->g_build, "Shop Sign", a3_v3(bx, WALK_TOP + 3.2f, bz - ld * 0.46f - 0.2f), a3_v3(lw * 0.5f, 0.5f, 0.15f), 0, "builtin:neon", n, 0);
            }
        }
    } break;
    case D_WAREHOUSE: {
        for (int ix = 0; ix < 2; ++ix) {
            if (!chance(g, density)) continue;
            building(g, x0 + w * (ix * 0.5f + 0.25f), cz, w * 0.46f, dp * 0.85f, rf(g, 7, 13), "builtin:building", k_vivid[ri(g, 0, 5)], "Warehouse");
        }
    } break;
    case D_HOTEL: {
        /* a row of hotels facing east (onto the beachfront drive at x1 + m) */
        f32 zc = z0;
        while (zc < z1 - 12) {
            f32 hw = rf(g, 16, 30);
            if (zc + hw > z1) hw = z1 - zc;
            if (chance(g, density)) hotel(g, x1 - 12, zc + hw * 0.5f, 22, hw - 1.5f, rf(g, 10, 24), x1 - 1.0f);
            zc += hw;
        }
        if (w > 30) building(g, x0 + (w - 26) * 0.5f, cz, w - 30, dp * 0.9f, rf(g, 12, 28), "builtin:building", k_walls[ri(g, 0, 4)], "Building");
    } break;
    }
}

/* ======================================================================== */
/* Roads                                                                    */
/* ======================================================================== */

/* straight road between two points (axis aligned), with sidewalks and lights */
static void road(Gen *g, A3Vec2 a, A3Vec2 b, f32 width, b32 walks, b32 lights, u32 lanes) {
    b32 along_x = a3_absf(b.x - a.x) > a3_absf(b.y - a.y);
    f32 len = along_x ? a3_absf(b.x - a.x) : a3_absf(b.y - a.y);
    if (len < 0.5f) return;
    A3Vec3 c = a3_v3((a.x + b.x) * 0.5f, ROAD_TOP - 0.05f, (a.y + b.y) * 0.5f);
    f32 yaw = along_x ? 90.0f : 0.0f;
    box(g, g->g_roads, "Road", c, a3_v3(width, 0.1f, len), yaw, "builtin:road", rgb(1, 1, 1), 1);
    g->st->roads++;
    if (walks) {
        for (int side = -1; side <= 1; side += 2) {
            f32 off = side * (width * 0.5f + 2.0f);
            A3Vec3 wc = along_x ? a3_v3(c.x, (LAND_TOP + WALK_TOP) * 0.5f, c.z + off) : a3_v3(c.x + off, (LAND_TOP + WALK_TOP) * 0.5f, c.z);
            box(g, g->g_roads, "Sidewalk", wc, a3_v3(4.0f, WALK_TOP - LAND_TOP, len), yaw, "builtin:sidewalk", rgb(0.7f, 0.69f, 0.66f), 1);
        }
    }
    if (lights) {
        int n = (int)(len / 38.0f);
        for (int i = 0; i <= n; ++i) {
            f32 t = n ? (f32)i / (f32)n : 0.5f;
            int side = (i & 1) ? 1 : -1;
            f32 off = side * (width * 0.5f + 0.8f);
            A3Vec3 p = along_x ? a3_v3(a3_lerpf(a.x, b.x, t), WALK_TOP, a.y + off) : a3_v3(a.x + off, WALK_TOP, a3_lerpf(a.y, b.y, t));
            f32 face = along_x ? (side > 0 ? 0.0f : 180.0f) : (side > 0 ? 90.0f : -90.0f);
            street_light(g, p, face);
        }
    }
    A3_UNUSED(lanes);   /* graph edges are added by the callers between intersection nodes */
}

static void intersection(Gen *g, f32 x, f32 z, f32 size) {
    A3Entity e = box(g, g->g_roads, "Intersection", a3_v3(x, ROAD_TOP - 0.05f, z), a3_v3(size, 0.1f, size), 0, "builtin:road", rgb(1, 1, 1), 1);
    a3_component_get(g->w, e, A3_T_MESH_RENDERER) ? (void)(((A3CMeshRenderer *)a3_component_get(g->w, e, A3_T_MESH_RENDERER))->metallic = 1.0f) : (void)0;
}

typedef struct Grid { f32 x0, x1, z0, z1, sx, sz, road; } Grid;

/* roads on grid lines, blocks in between; district chosen per block */
static void grid(Gen *g, const Grid *gr, District (*pick)(Gen *g, f32 cx, f32 cz)) {
    i32 nx = (i32)((gr->x1 - gr->x0) / gr->sx + 0.5f), nz = (i32)((gr->z1 - gr->z0) / gr->sz + 0.5f);
    f32 hw = gr->road * 0.5f;
    for (i32 i = 0; i <= nx; ++i) for (i32 j = 0; j <= nz; ++j) {
        f32 x = gr->x0 + i * gr->sx, z = gr->z0 + j * gr->sz;
        intersection(g, x, z, gr->road);
        b32 major_x = (j % 3) == 0, major_z = (i % 3) == 0;
        if (i < nx) road(g, a3_v2(x + hw, z), a3_v2(x + gr->sx - hw, z), gr->road, 1, major_x || (i % 2 == 0), major_x ? 2 : 1);
        if (j < nz) road(g, a3_v2(x, z + hw), a3_v2(x, z + gr->sz - hw), gr->road, 1, major_z || (j % 2 == 0), major_z ? 2 : 1);
        if (i < nx && j < nz) {
            f32 bx0 = x + hw + 4, bx1 = x + gr->sx - hw - 4, bz0 = z + hw + 4, bz1 = z + gr->sz - hw - 4;
            fill_block(g, bx0, bz0, bx1, bz1, pick(g, (bx0 + bx1) * 0.5f, (bz0 + bz1) * 0.5f));
        }
    }
    /* graph edges between intersections (the road pieces link node centers) */
    for (i32 i = 0; i <= nx; ++i) for (i32 j = 0; j <= nz; ++j) {
        f32 x = gr->x0 + i * gr->sx, z = gr->z0 + j * gr->sz;
        if (i < nx) edge(g, node(g, x, z), node(g, x + gr->sx, z), (j % 3) == 0 ? 2 : 1);
        if (j < nz) edge(g, node(g, x, z), node(g, x, z + gr->sz), (i % 3) == 0 ? 2 : 1);
    }
}

static District pick_mainland(Gen *g, f32 cx, f32 cz) {
    if (chance(g, 0.05f)) return D_PARK;
    if (cx > -360 && cz > -320 && cz < 460) return D_DOWNTOWN;
    if (cz < -500) return D_WAREHOUSE;
    if (cx < -620 && cz > -120) return D_LOWRISE;
    return D_MIDRISE;
}

static District pick_beach(Gen *g, f32 cx, f32 cz) {
    A3_UNUSED(cx);
    if (cz > -330 && cz < 330) return D_HOTEL;
    if (chance(g, 0.08f)) return D_PARK;
    return cz < -330 ? D_CONDO : D_MIDRISE;
}

/* elevated-looking causeway across the bay: deck, rails, piers, lights */
static void causeway(Gen *g, f32 z, f32 x0, f32 x1) {
    f32 len = x1 - x0, cx = (x0 + x1) * 0.5f;
    box(g, g->g_roads, "Causeway", a3_v3(cx, ROAD_TOP - 0.05f, z), a3_v3(16, 0.1f, len), 90, "builtin:road", rgb(1, 1, 1), 1);
    box(g, g->g_roads, "Causeway Deck", a3_v3(cx, ROAD_TOP - 0.9f, z), a3_v3(len, 1.6f, 19), 0, 0, rgb(0.62f, 0.62f, 0.6f), 1);
    for (int s = -1; s <= 1; s += 2) box(g, g->g_roads, "Rail", a3_v3(cx, ROAD_TOP + 0.5f, z + s * 9.2f), a3_v3(len, 0.9f, 0.3f), 0, "builtin:metal", rgb(0.8f, 0.8f, 0.78f), 1);
    for (f32 x = x0 + 15; x < x1; x += 30) box(g, g->g_roads, "Pier", a3_v3(x, -2.0f, z), a3_v3(2.2f, 4.0f, 14), 0, 0, rgb(0.5f, 0.5f, 0.48f), 0);
    for (f32 x = x0 + 20; x < x1; x += 40) street_light(g, a3_v3(x, ROAD_TOP, z + ((int)(x / 40) & 1 ? 8.8f : -8.8f)), ((int)(x / 40) & 1) ? 0 : 180);
    edge(g, node(g, x0, z), node(g, x1, z), 2);
    g->st->roads++;
}

/* ======================================================================== */
/* Entry                                                                    */
/* ======================================================================== */

void a3_city_apply_time(A3World *w, A3CityTime t) {
    A3CWorldSettings *ws = a3_world_settings(w);
    A3Entity sun = a3_entity_find_by_name(w, "Sun");
    if (a3_entity_is_null(sun)) {
        sun = a3_entity_create(w, "Sun");
        a3_component_add(w, sun, A3_T_TRANSFORM);
        a3_component_add(w, sun, A3_T_LIGHT);
    }
    A3CLight *l = (A3CLight *)a3_component_get(w, sun, A3_T_LIGHT);
    if (!l) l = (A3CLight *)a3_component_add(w, sun, A3_T_LIGHT);
    A3CTransform *st = a3_transform(w, sun);
    l->type = A3_LIGHT_DIRECTIONAL;
    l->cast_shadows = 1;
    if (t == A3_CITY_NIGHT) {
        /* moon, violet haze, neon */
        ws->sky_top = a3_v4(0.02f, 0.025f, 0.07f, 1);
        ws->sky_horizon = a3_v4(0.28f, 0.1f, 0.3f, 1);
        ws->ground_color = a3_v4(0.05f, 0.04f, 0.07f, 1);
        ws->fog_color = a3_v4(0.2f, 0.08f, 0.24f, 1);
        ws->fog_density = 0.0018f;
        ws->fog_height_falloff = 0.02f;
        ws->ambient_intensity = 1.6f;
        ws->bloom_intensity = 0.55f;
        ws->bloom_threshold = 1.0f;
        ws->exposure = 1.35f;
        ws->saturation = 1.15f;
        ws->contrast = 1.08f;
        ws->tint = a3_v4(0.96f, 0.96f, 1.0f, 1);
        ws->time_of_day = 23.0f;
        l->color = a3_v4(0.55f, 0.62f, 0.9f, 1);
        l->intensity = 0.07f;
        st->rotation = a3_quat_euler(-38 * A3_DEG2RAD, 120 * A3_DEG2RAD, 0);
    } else if (t == A3_CITY_SUNSET) {
        ws->sky_top = a3_v4(0.18f, 0.22f, 0.5f, 1);
        ws->sky_horizon = a3_v4(1.0f, 0.5f, 0.32f, 1);
        ws->ground_color = a3_v4(0.3f, 0.2f, 0.2f, 1);
        ws->fog_color = a3_v4(0.95f, 0.55f, 0.45f, 1);
        ws->fog_density = 0.001f;
        ws->fog_height_falloff = 0.015f;
        ws->ambient_intensity = 0.5f;
        ws->bloom_intensity = 0.6f;
        ws->bloom_threshold = 1.3f;
        ws->exposure = 1.1f;
        ws->saturation = 1.2f;
        ws->contrast = 1.06f;
        ws->tint = a3_v4(1.0f, 0.95f, 0.92f, 1);
        ws->time_of_day = 19.2f;
        l->color = a3_v4(1.0f, 0.62f, 0.38f, 1);
        l->intensity = 1.0f;
        st->rotation = a3_quat_euler(-9 * A3_DEG2RAD, -95 * A3_DEG2RAD, 0);   /* low in the west, over the mainland */
    } else {
        ws->sky_top = a3_v4(0.22f, 0.5f, 0.9f, 1);
        ws->sky_horizon = a3_v4(0.72f, 0.86f, 0.97f, 1);
        ws->ground_color = a3_v4(0.4f, 0.36f, 0.3f, 1);
        ws->fog_color = a3_v4(0.7f, 0.82f, 0.95f, 1);
        ws->fog_density = 0.0012f;
        ws->fog_height_falloff = 0.01f;
        ws->ambient_intensity = 0.5f;
        ws->bloom_intensity = 0.25f;
        ws->bloom_threshold = 1.5f;
        ws->exposure = 1.0f;
        ws->saturation = 1.12f;
        ws->contrast = 1.04f;
        ws->tint = a3_v4(1.0f, 0.99f, 0.96f, 1);
        ws->time_of_day = 12.5f;
        l->color = a3_v4(1.0f, 0.95f, 0.86f, 1);
        l->intensity = 1.05f;
        st->rotation = a3_quat_euler(-58 * A3_DEG2RAD, 30 * A3_DEG2RAD, 0);
    }
}

b32 a3_city_generate(A3World *w, const A3CityDesc *desc, A3CityStats *stats, A3StrBuf *graph_json) {
    A3CityDesc dd;
    if (!desc) { a3_city_desc_default(&dd); desc = &dd; }
    A3CityStats local;
    if (!stats) stats = &local;
    a3_zero_struct(stats);
    a3_procmeshes_register();
    Gen g;
    a3_zero_struct(&g);
    g.w = w;
    g.d = desc;
    g.st = stats;
    a3_rng_seed(&g.rng, desc->seed ? desc->seed : 1, 77);
    a3_hashmap_init(&g.node_map, 1024, A3_MEM_WORLD);
    b32 prev_loading = w->loading;
    w->loading = 1;   /* no on_add side effects while building */
    g.root = ent(&g, "Sol Harbor (generated)", A3_ENTITY_NULL, a3_v3_zero(), a3_v3_one(), 0);
    g.g_land = ent(&g, "Land & Water", g.root, a3_v3_zero(), a3_v3_one(), 0);
    g.g_roads = ent(&g, "Roads", g.root, a3_v3_zero(), a3_v3_one(), 0);
    g.g_blocks = ent(&g, "Blocks", g.root, a3_v3_zero(), a3_v3_one(), 0);
    g.g_build = ent(&g, "Buildings", g.root, a3_v3_zero(), a3_v3_one(), 0);
    g.g_lights = ent(&g, "Street Lights", g.root, a3_v3_zero(), a3_v3_one(), 0);
    g.g_palms = ent(&g, "Palms", g.root, a3_v3_zero(), a3_v3_one(), 0);
    g.g_props = ent(&g, "Props", g.root, a3_v3_zero(), a3_v3_one(), 0);

    /* ---- water and land ---- */
    A3Entity water = ent(&g, "Ocean", g.g_land, a3_v3(0, 0, 0), a3_v3(5200, 1, 5200), 0);
    mesh(&g, water, A3_PRIM_PLANE, "builtin:water", rgb(0.02f, 0.13f, 0.16f));
    box(&g, g.g_land, "Sea Floor", a3_v3(0, -2.5f, 0), a3_v3(5200, 1, 5200), 0, 0, rgb(0.1f, 0.12f, 0.1f), 1);
    struct { const char *name; f32 x0, x1, z0, z1; } land[] = {
        { "Mainland", -1100, 44, -920, 640 }, { "Sol Beach Island", 296, 462, -1010, 520 }, { "Port Island", 108, 252, 360, 520 },
        { "Isle A", 82, 124, -118, -42 }, { "Isle B", 146, 188, -118, -42 }, { "Isle C", 210, 252, -118, -42 },
    };
    for (u32 i = 0; i < A3_ARRAY_COUNT(land); ++i) {
        f32 h = LAND_TOP + 3.0f;
        box(&g, g.g_land, land[i].name, a3_v3((land[i].x0 + land[i].x1) * 0.5f, LAND_TOP - h * 0.5f, (land[i].z0 + land[i].z1) * 0.5f),
            a3_v3(land[i].x1 - land[i].x0, h, land[i].z1 - land[i].z0), 0, "builtin:sidewalk", rgb(0.5f, 0.5f, 0.48f), 1);
    }
    box(&g, g.g_land, "Beach", a3_v3(505, 0.1f, -245), a3_v3(90, 0.6f, 1530), 0, "builtin:sand", rgb(0.93f, 0.85f, 0.68f), 1);
    stats->min_x = -1100; stats->max_x = 550; stats->min_z = -1010; stats->max_z = 640;

    /* ---- mainland grid + bayfront boulevard ---- */
    Grid main_grid = { -1020, 0, -860, 580, 85, 80, 12 };
    grid(&g, &main_grid, pick_mainland);
    road(&g, a3_v2(24, -860), a3_v2(24, 580), 16, 0, 1, 2);
    {
        /* boulevard nodes: every mainland cross street plus the causeway landings */
        f32 zs[32];
        u32 nz = 0;
        for (f32 z = -860; z <= 580; z += 80) { edge(&g, node(&g, 0, z), node(&g, 24, z), 1); zs[nz++] = z; }
        static const f32 landings[3] = { -568, -80, 336 };
        for (u32 i = 0; i < 3; ++i) zs[nz++] = landings[i];
        for (u32 i = 1; i < nz; ++i) for (u32 j = i; j > 0 && zs[j - 1] > zs[j]; --j) { f32 t = zs[j]; zs[j] = zs[j - 1]; zs[j - 1] = t; }
        for (u32 i = 0; i + 1 < nz; ++i) edge(&g, node(&g, 24, zs[i]), node(&g, 24, zs[i + 1]), 2);
    }
    for (f32 z = -840; z < 580; z += 22) palm(&g, a3_v3(38, LAND_TOP, z + rf(&g, -3, 3)), rf(&g, 8, 12));

    /* ---- beach island: avenue, beachfront drive, cross streets ---- */
    Grid beach_grid = { 320, 400, -960, 480, 80, 72, 12 };
    grid(&g, &beach_grid, pick_beach);
    /* park strip with palms between the drive and the sand, lifeguard towers on the beach */
    box(&g, g.g_blocks, "Beachfront Park", a3_v3(431, (LAND_TOP + WALK_TOP) * 0.5f, -240), a3_v3(52, WALK_TOP - LAND_TOP, 1440), 0, "builtin:foliage", rgb(0.28f, 0.46f, 0.2f), 1);
    for (f32 z = -950; z < 470; z += 16) palm(&g, a3_v3(rf(&g, 410, 452), WALK_TOP, z + rf(&g, -4, 4)), rf(&g, 8, 13));
    static const A3Vec4 tower_colors[] = { { 1.0f, 0.55f, 0.6f, 1 }, { 0.4f, 0.8f, 0.95f, 1 }, { 1.0f, 0.85f, 0.3f, 1 }, { 0.6f, 0.9f, 0.55f, 1 }, { 0.8f, 0.6f, 1.0f, 1 } };
    for (f32 z = -900; z < 460; z += 140) lifeguard_tower(&g, a3_v3(500, 0.4f, z), tower_colors[ri(&g, 0, 4)]);
    for (f32 z = -900; z < 460; z += 23) {
        /* umbrellas */
        A3Entity pole = box(&g, g.g_props, "Umbrella Pole", a3_v3(rf(&g, 478, 530), 1.3f, z), a3_v3(0.08f, 2.2f, 0.08f), 0, "builtin:metal", rgb(0.9f, 0.9f, 0.9f), 0);
        A3CTransform *pt = a3_transform(w, pole);
        A3Entity top = ent(&g, "Umbrella", g.g_props, a3_v3(pt->position.x, 2.5f, z), a3_v3(2.6f, 0.6f, 2.6f), 0);
        mesh(&g, top, A3_PRIM_CONE, "builtin:foliage", k_vivid[ri(&g, 0, 5)]);
        g.st->props++;
    }

    /* ---- causeways and islands ---- */
    causeway(&g, -568, 44, 296);
    causeway(&g, -80, 44, 296);
    causeway(&g, 336, 44, 296);
    edge(&g, node(&g, 24, -568), node(&g, 44, -568), 2); edge(&g, node(&g, 296, -568), node(&g, 320, -568), 2);
    edge(&g, node(&g, 24, -80), node(&g, 44, -80), 2); edge(&g, node(&g, 296, -80), node(&g, 320, -80), 2);
    edge(&g, node(&g, 24, 336), node(&g, 44, 336), 2); edge(&g, node(&g, 296, 336), node(&g, 320, 336), 2);
    {
        /* tie the causeway landings into the beach avenue (x = 320, cross streets every 72 m from z = -960) */
        static const f32 landings[3] = { -568, -80, 336 };
        for (u32 i = 0; i < 3; ++i) {
            f32 zb = -960.0f + a3_floorf((landings[i] + 960.0f) / 72.0f) * 72.0f;
            edge(&g, node(&g, 320, landings[i]), node(&g, 320, zb), 2);
            if (zb + 72.0f <= 480.0f) edge(&g, node(&g, 320, landings[i]), node(&g, 320, zb + 72.0f), 2);
        }
    }
    for (int i = 0; i < 3; ++i) {
        f32 ix = 103 + i * 64;
        for (int k = 0; k < 4; ++k) {
            f32 hz = k < 2 ? -104 : -56, hx = ix + ((k & 1) ? 10 : -10);
            building(&g, hx, hz, 12, 10, rf(&g, 5, 8), "builtin:building", rgb(0.97f, 0.96f, 0.93f), "Villa");
        }
        for (int k = 0; k < 5; ++k) palm(&g, a3_v3(ix + rf(&g, -16, 16), LAND_TOP, rf(&g, -114, -46)), rf(&g, 7, 10));
    }

    /* ---- port: containers and cranes ---- */
    static const A3Vec4 cont[] = { { 0.75f, 0.2f, 0.15f, 1 }, { 0.15f, 0.35f, 0.7f, 1 }, { 0.9f, 0.6f, 0.1f, 1 }, { 0.2f, 0.55f, 0.3f, 1 }, { 0.85f, 0.85f, 0.8f, 1 } };
    for (f32 z = 380; z < 470; z += 3.2f) for (f32 x = 126; x < 230; x += 14) {
        i32 stack = ri(&g, 0, 4);
        for (i32 k = 0; k < stack; ++k) box(&g, g.g_props, "Container", a3_v3(x, LAND_TOP + 1.3f + k * 2.6f, z), a3_v3(12, 2.5f, 2.4f), 0, "builtin:metal", cont[ri(&g, 0, 4)], k == 0);
    }
    for (int i = 0; i < 3; ++i) {
        f32 x = 140 + i * 40;
        for (int s = -1; s <= 1; s += 2) box(&g, g.g_props, "Crane Leg", a3_v3(x + s * 6, LAND_TOP + 18, 505), a3_v3(1.6f, 36, 1.6f), 0, "builtin:metal", rgb(0.85f, 0.25f, 0.15f), 1);
        box(&g, g.g_props, "Crane Boom", a3_v3(x, LAND_TOP + 36, 525), a3_v3(3, 2.4f, 70), 0, "builtin:metal", rgb(0.85f, 0.25f, 0.15f), 0);
        A3Entity beacon = box(&g, g.g_props, "Crane Light", a3_v3(x, LAND_TOP + 37.6f, 490), a3_v3(0.8f, 0.8f, 0.8f), 0, "builtin:neon", rgb(1, 0.1f, 0.05f), 0);
        A3_UNUSED(beacon);
    }
    Grid port_grid = { 116, 244, 348, 348, 128, 1, 10 };
    road(&g, a3_v2(port_grid.x0, 348), a3_v2(port_grid.x1, 348), 10, 0, 1, 1);
    edge(&g, node(&g, port_grid.x0, 348), node(&g, port_grid.x1, 348), 1);

    /* ---- boats in the bay ---- */
    for (int i = 0; i < 14; ++i) {
        f32 bx = rf(&g, 60, 280), bz = rf(&g, -900, 600);
        if (a3_absf(bz + 568) < 30 || a3_absf(bz + 80) < 60 || a3_absf(bz - 336) < 30 || (bz > 350 && bx > 100)) continue;
        f32 yaw = rf(&g, 0, 360), len = rf(&g, 8, 22);
        box(&g, g.g_props, "Boat", a3_v3(bx, 0.6f, bz), a3_v3(len * 0.32f, 1.4f, len), yaw, "builtin:carpaint", rgb(0.95f, 0.95f, 0.95f), 1);
        box(&g, g.g_props, "Boat Cabin", a3_v3(bx, 1.8f, bz), a3_v3(len * 0.22f, 1.2f, len * 0.35f), yaw, "builtin:glass", rgb(0.3f, 0.4f, 0.5f), 0);
        g.st->props++;
    }

    /* ---- road graph ---- */
    stats->road_nodes = g.nodes.count;
    stats->road_edges = g.edges.count / 3;
    if (graph_json) {
        A3JsonWriter jw;
        a3_jw_init(&jw, graph_json, 1);
        a3_jw_begin_object(&jw);
        a3_jw_kv_string(&jw, "format", "asm3d.roads");
        a3_jw_kv_int(&jw, "version", 1);
        a3_jw_kv_number(&jw, "road_y", ROAD_TOP);
        a3_jw_kv_number(&jw, "walk_y", WALK_TOP);
        a3_jw_kv_number(&jw, "road_half_width", 6.0);
        a3_jw_key(&jw, "nodes");
        a3_jw_begin_array(&jw);
        for (u32 i = 0; i < g.nodes.count; ++i) { f32 p[2] = { g.nodes.data[i].x, g.nodes.data[i].y }; a3_jw_floats(&jw, p, 2); }
        a3_jw_end_array(&jw);
        a3_jw_key(&jw, "edges");
        a3_jw_begin_array(&jw);
        for (u32 i = 0; i + 2 < g.edges.count; i += 3) {
            a3_jw_begin_array(&jw);
            a3_jw_int(&jw, g.edges.data[i]); a3_jw_int(&jw, g.edges.data[i + 1]); a3_jw_int(&jw, g.edges.data[i + 2]);
            a3_jw_end_array(&jw);
        }
        a3_jw_end_array(&jw);
        a3_jw_end_object(&jw);
    }
    a3_city_apply_time(w, (A3CityTime)desc->time);
    w->loading = prev_loading;
    a3_array_free(g.nodes);
    a3_array_free(g.edges);
    a3_hashmap_free(&g.node_map);
    return 1;
}

/* ======================================================================== */
/* City component                                                           */
/* ======================================================================== */

u32 A3_T_CITY = 0xFFFFFFFFu;
static const char *const g_time_names[] = { "Day", "Sunset", "Night" };

typedef struct RoadsCache { A3World *world; A3StrBuf json; } RoadsCache;
static RoadsCache g_roads[4];

static void roads_release(A3World *w) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_roads); ++i)
        if (g_roads[i].world == w) { a3_strbuf_free(&g_roads[i].json); a3_zero_struct(&g_roads[i]); }
}

const char *a3_city_generated_roads(A3World *w, usize *len) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_roads); ++i)
        if (g_roads[i].world == w && g_roads[i].json.len) { if (len) *len = g_roads[i].json.len; return g_roads[i].json.data; }
    return 0;
}

static b32 eq_nocase(const char *a, const char *b) {
    while (*a && *b && a3_to_lower((u8)*a) == a3_to_lower((u8)*b)) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

A3CityTime a3_city_time_from_name(const char *name) {
    if (!name) return A3_CITY_NIGHT;
    if (eq_nocase(name, "day")) return A3_CITY_DAY;
    if (eq_nocase(name, "sunset")) return A3_CITY_SUNSET;
    return A3_CITY_NIGHT;
}

void a3_city_register(void) {
    a3_world_on_destroy(roads_release);
    if (A3_T_CITY != 0xFFFFFFFFu) return;
    A3CCity c;
    a3_zero_struct(&c);
    c.seed = 1; c.density = 1.0f; c.time = A3_CITY_NIGHT; c.street_lights = 1; c.neon = 1;
    u32 t = a3_component_register("City", "World", sizeof(A3CCity), 16, &c, A3_COMP_BUILTIN,
        "Generates Sol Harbor, a coastal city with towers, an Art Deco beachfront, causeways and a port, when the game starts.");
    A3_T_CITY = t;
    a3_component_type(t)->icon = "city";
    A3_REFLECT_FIELD(t, A3CCity, seed, A3_FIELD_U32, "Seed", "Same seed = same city.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CCity, density, A3_FIELD_F32, "Density", "Fraction of lots with buildings."), 0.1f, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    A3FieldDesc *f = A3_REFLECT_FIELD(t, A3CCity, time, A3_FIELD_ENUM, "Time of Day", "Sky, sun, fog and post-processing look.");
    f->enum_names = g_time_names; f->enum_count = 3;
    A3_REFLECT_FIELD(t, A3CCity, street_lights, A3_FIELD_BOOL, "Street Lights", "Lamp posts with real point lights.");
    A3_REFLECT_FIELD(t, A3CCity, neon, A3_FIELD_BOOL, "Neon", "Neon signs and trim on the beachfront.");
    A3_REFLECT_FIELD(t, A3CCity, generated, A3_FIELD_BOOL, "Generated", "Runtime.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
}

void a3_city_update(A3World *w) {
    if (A3_T_CITY == 0xFFFFFFFFu) return;
    u32 n = 0;
    const A3Entity *ents = 0;
    a3_component_array(w, A3_T_CITY, &n, &ents);
    for (u32 i = 0; i < n; ++i) {
        A3CCity *c = (A3CCity *)a3_component_get(w, ents[i], A3_T_CITY);
        if (!c || c->generated || !a3_entity_active(w, ents[i])) continue;
        c->generated = 1;
        A3CityDesc d;
        a3_city_desc_default(&d);
        d.seed = c->seed;
        d.density = c->density;
        d.time = c->time;
        d.street_lights = c->street_lights;
        d.neon = c->neon;
        RoadsCache *rc = 0;
        for (u32 k = 0; k < A3_ARRAY_COUNT(g_roads) && !rc; ++k) if (g_roads[k].world == w) rc = &g_roads[k];
        for (u32 k = 0; k < A3_ARRAY_COUNT(g_roads) && !rc; ++k) if (!g_roads[k].world) rc = &g_roads[k];
        if (!rc) { roads_release(g_roads[0].world); rc = &g_roads[0]; }
        if (rc->world != w) { a3_zero_struct(rc); rc->world = w; a3_strbuf_init(&rc->json, A3_MEM_WORLD); }
        a3_strbuf_clear(&rc->json);
        u64 t0 = a3_time_ns();
        A3CityStats st;
        a3_city_generate(w, &d, &st, &rc->json);
        A3_INFO("city", "generated Sol Harbor (seed %u): %u objects, %u buildings, %u lights in %.0f ms",
                c->seed, st.entities, st.buildings, st.lights, (f64)(a3_time_ns() - t0) * 1e-6);
    }
}
