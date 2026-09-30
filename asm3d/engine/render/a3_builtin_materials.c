/*
 * ASM3D - a3_builtin_materials.c
 * Procedural surface materials usable by name ("builtin:building" in the
 * Material field of a Mesh Renderer). Everything is computed in the shader
 * from world position, so large scenes (a whole city) need no textures:
 * windows follow floors and bays in meters on any scaled box, roads get lane
 * markings, water moves. The Mesh Renderer's Base Color tints each material.
 *
 * Night detection: the sun's brightness (u_sun_color) dims during the night,
 * so windows and neon light up automatically when the sun goes down.
 */
#include "a3_renderer.h"
#include "../core/a3_string.h"

#define NIGHT "float a3_night() { return 1.0 - smoothstep(0.08, 0.9, dot(u_sun_color, vec3(0.3333))); }\n"
#define H2 "float a3_h2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"

/* Interior mapping: a ray from the camera through the window is intersected
 * with a virtual room box behind the facade (back wall, side walls, floor,
 * ceiling), so windows show rooms with depth and parallax instead of a flat
 * texture. f = position in the facade cell (0..1), t = facade tangent,
 * n = outward normal, size = cell width / height (m), depth = room depth (m). */
#define ROOM \
"vec3 a3_room(vec2 f, vec3 t, vec3 n, vec3 v, vec2 size, float depth, float seed, float light, float day) {\n" \
"    vec3 rd = -v;\n" \
"    vec3 d = vec3(dot(rd, t) / size.x, rd.y / size.y, max(dot(rd, -n), 0.05) / depth);\n" \
"    vec3 p = vec3(f, 0.0);\n" \
"    vec3 st = step(0.0, d);\n" \
"    vec3 tt = (st - p) / (mix(vec3(-1.0), vec3(1.0), st) * max(abs(d), vec3(1e-5)));\n" \
"    float tm = min(tt.x, min(tt.y, tt.z));\n" \
"    vec3 h = p + d * tm;\n" \
"    float hs = fract(sin(seed * 91.7) * 4375.5);\n" \
"    vec3 wallc = hs < 0.3 ? vec3(0.82, 0.78, 0.7) : hs < 0.55 ? vec3(0.7, 0.74, 0.78) : hs < 0.8 ? vec3(0.86, 0.84, 0.8) : vec3(0.75, 0.6, 0.5);\n" \
"    vec3 c;\n" \
"    if (tm == tt.z) {\n" \
"        c = wallc;\n" \
"        float pic = step(abs(h.x - 0.3 - hs * 0.4), 0.12) * step(abs(h.y - 0.6), 0.1);\n" \
"        c = mix(c, vec3(0.3 + hs * 0.5, 0.35, 0.5 - hs * 0.3), pic * step(0.4, hs));\n" \
"        float desk = step(h.y, 0.26) * step(abs(h.x - 0.5 + (hs - 0.5) * 0.4), 0.28);\n" \
"        c = mix(c, vec3(0.25, 0.18, 0.12), desk);\n" \
"    } else if (tm == tt.y) {\n" \
"        if (d.y < 0.0) c = (hs < 0.5 ? vec3(0.42, 0.3, 0.2) : vec3(0.35, 0.36, 0.38)) * (0.85 + 0.15 * step(0.5, fract(h.x * 6.0)));\n" \
"        else c = vec3(0.9) + light * 2.5 * step(abs(h.x - 0.5), 0.18) * step(abs(h.z - 0.5), 0.12);\n" \
"    } else {\n" \
"        c = wallc * 0.8;\n" \
"        float shelf = step(0.6, fract(seed * 13.1)) * step(h.z, 0.5) * step(h.y, 0.7);\n" \
"        c = mix(c, vec3(0.3, 0.22, 0.15), shelf);\n" \
"    }\n" \
"    float lamp = light * (0.55 + 0.45 * h.y) * (1.0 - 0.35 * h.z);\n" \
"    float amb = day * (1.0 - 0.6 * h.z);\n" \
"    return c * (lamp + amb);\n" \
"}\n"

typedef struct BuiltinMaterial { const char *name; const char *doc; const char *code; } BuiltinMaterial;

static const BuiltinMaterial g_builtins[] = {
    { "building", "Office / apartment facade: floors every 3.5 m, recessed windows with rooms behind them (interior mapping), blinds, lit at night, flat roof. Base Color = wall color.",
      NIGHT H2 ROOM
      "void a3_surface(inout A3Surface s) {\n"
      "    vec3 n = normalize(s.normal);\n"
      "    vec3 wall = s.color.rgb * (0.88 + 0.2 * a3_noise(s.world_pos * 0.7) + 0.06 * a3_noise(s.world_pos * 6.0));\n"
      "    if (abs(n.y) > 0.6) { s.albedo = vec3(0.32, 0.31, 0.30) * (0.8 + 0.4 * a3_noise(s.world_pos * 2.0)); s.roughness = 0.95; return; }\n"
      "    vec3 t = normalize(cross(vec3(0.0, 1.0, 0.0), n));\n"
      "    float u = dot(s.world_pos, t), y = s.world_pos.y;\n"
      "    s.normal = a3_bump(n, s.world_pos, a3_noise(s.world_pos * 5.0) * 0.004 + a3_noise(s.world_pos * 31.0) * 0.0012);\n"   /* stucco */
      "    vec2 cell = vec2(floor(u / 3.0), floor(y / 3.5));\n"
      "    vec2 f = vec2(fract(u / 3.0), fract(y / 3.5));\n"
      "    float win = step(0.18, f.x) * step(f.x, 0.82) * step(0.25, f.y) * step(f.y, 0.85) * step(1.0, y);\n"
      "    float frame = win * (1.0 - step(0.2, f.x) * step(f.x, 0.8) * step(0.27, f.y) * step(f.y, 0.83));\n"
      "    float mid = win * step(abs(f.x - 0.5), 0.008);\n"
      "    float night = a3_night();\n"
      "    float seed = a3_h2(cell + floor(s.world_pos.xz / 40.0) * 7.31);\n"
      "    float lit = step(0.62, seed) * (0.35 + 0.65 * a3_h2(cell * 3.3 + 1.7));\n"
      "    float kind = a3_h2(cell * 1.7);\n"
      "    vec3 warm = kind < 0.6 ? vec3(1.0, 0.68, 0.38) : kind < 0.85 ? vec3(0.95, 0.85, 0.7) : vec3(0.6, 0.75, 1.0);\n"
      "    float day = clamp(dot(u_sun_color, vec3(0.3333)) * 0.05, 0.0, 0.2) + 0.012;\n"
      "    vec3 room = a3_room(f, t, n, s.view_dir, vec2(3.0, 3.5), 5.0, seed * 17.0 + kind, lit * night * 1.4, day) * mix(vec3(1.0), warm, lit * night);\n"
      "    float blind = step(1.0 - a3_h2(cell * 5.1) * 0.7, 1.0 - (f.y - 0.27) / 0.56) * step(0.45, a3_h2(cell * 2.3));\n"
      "    vec3 blinds = mix(vec3(0.75, 0.72, 0.66), vec3(0.55, 0.52, 0.48), step(0.5, fract(y * 12.0))) * (day * 4.0 + lit * night * 0.8 * warm);\n"
      "    room = mix(room, blinds, blind);\n"
      /* window reveal: the wall around the opening is lit from above, the sill catches light */
      "    float sill = step(abs(f.y - 0.235), 0.015) * step(0.16, f.x) * step(f.x, 0.84);\n"
      "    float glass = win * (1.0 - frame) * (1.0 - mid);\n"
      "    s.albedo = mix(mix(wall, wall * 1.2, sill), vec3(0.02, 0.025, 0.03), glass);\n"
      "    s.albedo = mix(s.albedo, vec3(0.18, 0.18, 0.2), max(frame, mid));\n"
      "    s.roughness = mix(0.85, 0.05, glass);\n"
      "    s.metallic = mix(0.0, 0.25, glass);\n"
      "    if (sill > 0.5) s.normal = normalize(n + vec3(0.0, 0.8, 0.0));\n"
      "    s.emissive = room * glass;\n"
      "}\n" },
    { "artdeco", "Pastel Art Deco hotel: horizontal eyebrow ledges, windows with rooms behind them (interior mapping) and neon trim that glows at night. Base Color = pastel wall.",
      NIGHT H2 ROOM
      "void a3_surface(inout A3Surface s) {\n"
      "    vec3 n = normalize(s.normal);\n"
      "    float night = a3_night();\n"
      "    vec3 wall = s.color.rgb * (0.95 + 0.1 * a3_noise(s.world_pos * 1.3));\n"
      "    if (abs(n.y) > 0.6) { s.albedo = wall * 0.8; s.roughness = 0.9; return; }\n"
      "    vec3 t = normalize(cross(vec3(0.0, 1.0, 0.0), n));\n"
      "    float u = dot(s.world_pos, t), y = s.world_pos.y;\n"
      "    float fy = fract(y / 3.2);\n"
      "    float ledge = smoothstep(0.02, 0.0, abs(fy - 0.08)) ;\n"
      "    vec2 cell = vec2(floor(u / 2.4), floor(y / 3.2));\n"
      "    vec2 f = vec2(fract(u / 2.4), fy);\n"
      "    float win = step(0.25, f.x) * step(f.x, 0.75) * step(0.3, f.y) * step(f.y, 0.82) * step(3.0, y);\n"
      "    float lit = step(0.55, a3_h2(cell + floor(s.world_pos.xz / 30.0))) * (0.4 + 0.6 * a3_h2(cell * 2.9));\n"
      "    float h = s.local_pos.y + 0.5;\n"                                           /* 0 bottom .. 1 top of the box */
      "    float trim = smoothstep(0.012, 0.0, abs(h - 0.94)) + smoothstep(0.01, 0.0, abs(h - 0.25)) * 0.8;\n"
      "    float hue = a3_h2(floor(s.world_pos.xz / 25.0));\n"
      "    vec3 neon = hue < 0.33 ? vec3(1.0, 0.15, 0.6) : hue < 0.66 ? vec3(0.1, 0.9, 1.0) : vec3(0.6, 0.2, 1.0);\n"
      "    float day = clamp(dot(u_sun_color, vec3(0.3333)) * 0.05, 0.0, 0.2) + 0.012;\n"
      "    vec3 room = a3_room(f, t, n, s.view_dir, vec2(2.4, 3.2), 4.5, a3_h2(cell * 4.1) * 13.0, lit * night * 1.3, day) * mix(vec3(1.0), vec3(1.0, 0.75, 0.5), lit * night);\n"
      "    if (ledge > 0.5) s.normal = normalize(n + vec3(0.0, 0.9, 0.0));\n"
      "    s.albedo = mix(mix(wall, wall * 1.15, ledge), vec3(0.03, 0.04, 0.05), win);\n"
      "    s.roughness = mix(0.8, 0.06, win);\n"
      "    s.metallic = mix(0.0, 0.2, win);\n"
      "    s.emissive = room * win + neon * trim * (0.3 + 3.2 * night);\n"
      "}\n" },
    { "tower", "Glass skyscraper: reflective curtain wall with mullions, offices behind the glass (interior mapping, lit at night), red beacon on top. Base Color = glass tint.",
      NIGHT H2 ROOM
      "void a3_surface(inout A3Surface s) {\n"
      "    vec3 n = normalize(s.normal);\n"
      "    float night = a3_night();\n"
      "    if (abs(n.y) > 0.6) { s.albedo = vec3(0.2); s.roughness = 0.9; vec2 bc = fract(s.world_pos.xz * 0.25) - 0.5; s.emissive = vec3(1.0, 0.05, 0.02) * step(0.993, a3_h2(floor(s.world_pos.xz * 0.25))) * step(length(bc), 0.18) * (1.0 + 8.0 * night); return; }\n"
      "    vec3 t = normalize(cross(vec3(0.0, 1.0, 0.0), n));\n"
      "    float u = dot(s.world_pos, t), y = s.world_pos.y;\n"
      "    vec2 cell = vec2(floor(u / 1.6), floor(y / 3.8));\n"
      "    vec2 f = vec2(fract(u / 1.6), fract(y / 3.8));\n"
      "    float mull = 1.0 - step(0.06, f.x) * step(f.y, 0.94);\n"
      "    float room = a3_h2(floor(vec2(u / 3.2, y / 3.8)) + floor(s.world_pos.xz / 50.0) * 3.1);\n"
      "    float floor_on = step(0.35, a3_h2(vec2(floor(y / 3.8), floor(s.world_pos.x / 50.0) + floor(s.world_pos.z / 50.0) * 5.0)));\n"
      "    float lit = step(0.66, room) * floor_on * (0.5 + 0.5 * a3_h2(cell));\n"
      "    s.albedo = mix(s.color.rgb * 0.35, vec3(0.55, 0.57, 0.6), mull);\n"
      "    s.metallic = mix(0.85, 0.6, mull);\n"
      "    s.roughness = mix(0.06, 0.35, mull);\n"
      "    vec3 office = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.8, 0.55), step(0.7, a3_h2(cell * 0.37)));\n"
      "    float day = clamp(dot(u_sun_color, vec3(0.3333)) * 0.008, 0.0, 0.03) + 0.004;\n"
      "    vec2 rf = vec2(fract(u / 3.2), f.y);\n"
      "    vec3 inside = a3_room(rf, t, n, s.view_dir, vec2(3.2, 3.8), 8.0, room * 29.0, lit * night * 0.4, day) * mix(vec3(1.0), office, lit * night);\n"
      "    s.emissive = inside * (1.0 - mull);\n"
      "}\n" },
    { "road", "Asphalt with lane markings, crosswalks at the ends and wet patches that reflect lights. Roads run along their local Z axis (scale X = width); Metallic 1 = plain asphalt (intersections).",
      NIGHT H2
      "void a3_surface(inout A3Surface s) {\n"
      "    vec3 n = normalize(s.normal);\n"
      "    float g = a3_fbm(s.world_pos * 1.7);\n"
      "    vec3 asphalt = vec3(0.075, 0.075, 0.08) * (0.75 + 0.5 * g);\n"
      "    float plain = step(0.5, s.metallic);\n"                                 /* Metallic 1 = intersection: no markings */
      "    s.albedo = asphalt; s.metallic = 0.0;\n"
      "    float puddle = smoothstep(0.55, 0.62, a3_fbm(s.world_pos * 0.18 + 3.1));\n"
      "    s.roughness = mix(0.62, 0.06, puddle);\n"
      "    s.albedo *= mix(1.0, 0.55, puddle);\n"
      "    s.normal = a3_bump(n, s.world_pos, (a3_noise(s.world_pos * 22.0) * 0.003 + g * 0.004) * (1.0 - puddle));\n"   /* asphalt grain; puddles are flat */
      "    if (n.y < 0.6 || plain > 0.5) return;\n"
      "    float x = s.local_pos.x, z = s.local_pos.z;\n"                         /* -0.5..0.5 across / along the road */
      "    float along = s.world_pos.x + s.world_pos.z;\n"                        /* roads run along world X or Z */
      "    float center = step(abs(abs(x) - 0.012), 0.006);\n"
      "    float edge = step(abs(abs(x) - 0.44), 0.006);\n"
      "    float dash = step(abs(abs(x) - 0.22), 0.005) * step(0.5, fract(along / 6.0));\n"
      "    float cross_walk = step(0.44, abs(z)) * step(abs(x), 0.42) * step(0.5, fract(x * 22.0));\n"
      "    vec3 paint = center > 0.5 ? vec3(0.85, 0.65, 0.12) : vec3(0.85);\n"
      "    float m = clamp(center + edge + dash + cross_walk, 0.0, 1.0) * (0.75 + 0.25 * g);\n"
      "    s.albedo = mix(s.albedo, paint, m);\n"
      "    s.roughness = mix(s.roughness, 0.5, m);\n"
      "}\n" },
    { "sidewalk", "Concrete sidewalk with 1.5 m joints and a curb edge. Base Color = concrete tint.",
      H2
      "void a3_surface(inout A3Surface s) {\n"
      "    vec2 f = fract(s.world_pos.xz / 1.5);\n"
      "    float joint = 1.0 - step(0.03, f.x) * step(0.03, f.y);\n"
      "    float stain = smoothstep(0.45, 0.75, a3_fbm(s.world_pos * 0.35)) * 0.25 + smoothstep(0.6, 0.8, a3_noise(s.world_pos * 1.3)) * 0.12;\n"
      "    float slab = 0.94 + 0.12 * a3_h2(floor(s.world_pos.xz / 1.5));\n"
      "    s.albedo = s.color.rgb * slab * (0.85 + 0.2 * a3_noise(s.world_pos * 3.0) + 0.08 * a3_noise(s.world_pos * 17.0)) * (1.0 - joint * 0.4) * (1.0 - stain);\n"
      "    s.roughness = 0.8;\n"
      "    vec2 jd = min(f, 1.0 - f) * 1.5;\n"                                   /* meters to the nearest joint */
      "    float groove = 1.0 - smoothstep(0.0, 0.03, min(jd.x, jd.y));\n"
      "    if (s.normal.y > 0.6) s.normal = a3_bump(normalize(s.normal), s.world_pos, -groove * 0.006 + a3_noise(s.world_pos * 25.0) * 0.001);\n"
      "}\n" },
    { "sand", "Beach sand with ripples. Base Color = sand tint.",
      "void a3_surface(inout A3Surface s) {\n"
      "    float r = sin(s.world_pos.x * 2.3 + a3_noise(s.world_pos * 0.5) * 6.0) * 0.5 + 0.5;\n"
      "    s.albedo = s.color.rgb * (0.88 + 0.12 * r + 0.1 * a3_noise(s.world_pos * 9.0));\n"
      "    s.roughness = 0.95;\n"
      "    s.normal = a3_bump(normalize(s.normal), s.world_pos, r * 0.012 + a3_noise(s.world_pos * 9.0) * 0.004);\n"
      "}\n" },
    { "water", "Ocean / bay water: moving waves, very smooth, reflects the sky and the city (screen-space reflections). Base Color = deep water color.",
      "float a3_wave(vec2 p, float t) {\n"
      "    return a3_fbm(vec3(p * 0.15 + vec2(t * 0.05, t * 0.03), t * 0.08)) + 0.5 * a3_noise(vec3(p * 0.6 - vec2(t * 0.2, 0.0), t * 0.3));\n"
      "}\n"
      "void a3_surface(inout A3Surface s) {\n"
      "    vec2 p = s.world_pos.xz;\n"
      "    float e = 0.35, t = s.time;\n"
      "    float h = a3_wave(p, t);\n"
      "    float hx = a3_wave(p + vec2(e, 0.0), t) - h, hz = a3_wave(p + vec2(0.0, e), t) - h;\n"
      "    float fade = 1.0 / (1.0 + length(s.world_pos - u_camera_pos) * 0.012);\n"        /* calmer far away: no sparkle aliasing */
      "    s.normal = normalize(vec3(-hx * 0.6 * fade, e, -hz * 0.6 * fade));\n"
      "    s.albedo = s.color.rgb * (0.7 + 0.5 * h);\n"
      "    s.roughness = 0.03;\n"
      "    s.metallic = 0.0;\n"
      "}\n" },
    { "glass", "Clear reflective glass (use a Base Color alpha below 1 for see-through).",
      "void a3_surface(inout A3Surface s) { s.albedo = s.color.rgb * 0.3; s.roughness = 0.04; s.metallic = 0.5; }\n" },
    { "neon", "Glowing neon tube or sign: Base Color is the light color; flickers slightly and is much brighter at night.",
      NIGHT
      "void a3_surface(inout A3Surface s) {\n"
      "    float fl = 0.92 + 0.08 * sin(s.time * 37.0 + s.world_pos.x * 3.0) * step(0.97, fract(s.time * 0.37 + s.world_pos.z));\n"
      "    s.albedo = s.color.rgb * 0.2;\n"
      "    s.emissive = s.color.rgb * (1.2 + 4.0 * a3_night()) * fl;\n"
      "    s.roughness = 0.4;\n"
      "}\n" },
    { "carpaint", "Glossy metallic car paint with a clear coat look. Base Color = paint color.",
      "void a3_surface(inout A3Surface s) { s.albedo = s.color.rgb; s.metallic = 0.55; s.roughness = 0.18 + 0.05 * a3_noise(s.world_pos * 40.0); }\n" },
    { "palm_trunk", "Palm trunk with rings.",
      "void a3_surface(inout A3Surface s) {\n"
      "    float ring = smoothstep(0.35, 0.5, abs(fract(s.world_pos.y * 2.2) - 0.5));\n"
      "    s.albedo = mix(vec3(0.36, 0.27, 0.18), vec3(0.24, 0.18, 0.12), ring) * (0.85 + 0.3 * a3_noise(s.world_pos * 6.0));\n"
      "    s.roughness = 0.9;\n"
      "}\n" },
    { "foliage", "Leaves and grass with color variation. Base Color = leaf color.",
      "void a3_surface(inout A3Surface s) { s.albedo = s.color.rgb * (0.7 + 0.5 * a3_noise(s.world_pos * 2.5)); s.roughness = 0.8; }\n" },
    { "metal", "Painted or bare metal (lamp posts, rails, cranes). Base Color = color.",
      "void a3_surface(inout A3Surface s) { s.albedo = s.color.rgb; s.metallic = 0.8; s.roughness = 0.35 + 0.2 * a3_noise(s.world_pos * 5.0); }\n" },
    { "carbody", "Car body for the builtin:car_* models: clear-coat paint (Base Color), tinted glass, chrome trim, head and tail lights (lit at night), grille, plates, door seams and handles, black sills. Regions come from the model's feature coordinates.",
      NIGHT H2
      "void a3_surface(inout A3Surface s) {\n"
      "    float u = s.uv.x, v = s.uv.y; vec3 lp = s.local_pos; float ax = abs(lp.x);\n"
      "    vec3 fn = normalize(cross(dFdx(lp), dFdy(lp)));\n"                                  /* object-space facet normal (sign free) */
      "    float side = abs(fn.x), night = a3_night();\n"
      "    float flake = a3_noise(lp * 180.0);\n"
      "    s.albedo = s.color.rgb * (0.96 + 0.08 * flake); s.metallic = 0.18; s.roughness = 0.07 + 0.04 * flake;\n"
      "    if (u > 10.5) {\n"                                                                   /* add-ons */
      "        if (u < 12.0) { if (fn.z * sign(lp.z + 10.0) > 0.7 && ax > 0.9) { s.albedo = vec3(0.6); s.metallic = 1.0; s.roughness = 0.02; } return; }\n"
      "        if (u < 13.0) { s.albedo = vec3(1.0, 0.85, 0.35); s.metallic = 0.0; s.roughness = 0.5; s.emissive = vec3(1.0, 0.8, 0.3) * (0.2 + 1.6 * night); return; }\n"
      "        float on = step(0.5, fract(s.time * 2.3 + (u < 14.0 ? 0.0 : 0.5)));\n"
      "        vec3 lc = u < 14.0 ? vec3(1.0, 0.05, 0.03) : vec3(0.05, 0.25, 1.0);\n"
      "        s.albedo = lc * 0.3; s.metallic = 0.0; s.roughness = 0.2; s.emissive = lc * on * (3.0 + 6.0 * night); return;\n"
      "    }\n"
      "    if (v < 0.05) { s.albedo = vec3(0.025); s.metallic = 0.0; s.roughness = 0.9; return; }\n"            /* underside, wheel wells */
      "    if (v < 0.17) { s.albedo = vec3(0.035); s.metallic = 0.0; s.roughness = 0.55; }\n"                   /* black sill / lower cladding */
      "    float front = (1.0 - step(0.12, u)) * step(0.3, abs(fn.z)), rear = step(4.9, u) * step(0.3, abs(fn.z));\n"
      /* glass: windshield and rear window on the top of the ring, side windows below the roof edge */
      "    float wind = step(1.045, u) * step(u, 1.955) * step(1.958, v);\n"
      "    float rwin = step(3.06, u) * step(u, 3.94) * step(1.958, v);\n"
      "    float swin = step(1.13, u) * step(u, 3.87) * step(1.115, v) * step(v, 1.905) * step(0.03, abs(u - 2.47));\n"
      "    float glass = max(max(wind, rwin), swin);\n"
      "    if (glass > 0.5) {\n"
      "        float edge = min(min(u - 1.045, 3.94 - u), 0.05) * 20.0;\n"
      "        s.albedo = vec3(0.015, 0.02, 0.025) * (1.0 - 0.6 * (1.0 - edge)); s.metallic = 0.0; s.roughness = 0.02;\n"
      "        return;\n"
      "    }\n"
      "    if (v > 1.08 && v < 1.115 && u > 1.13 && u < 3.87) { s.albedo = vec3(0.75); s.metallic = 1.0; s.roughness = 0.08; return; }\n"  /* chrome window trim */
      "    if (front > 0.5 && v > 0.62 && v < 0.95 && ax > 0.40 && ax < 0.82) {\n"               /* headlights */
      "        float ring = step(0.7, fract(length(vec2(ax - 0.61, (v - 0.78) * 0.5)) * 18.0));\n"
      "        s.albedo = vec3(0.85, 0.87, 0.9) * (0.7 + 0.3 * ring); s.metallic = 0.9; s.roughness = 0.04;\n"
      "        s.emissive = vec3(1.0, 0.96, 0.88) * (0.25 + 4.5 * night) * (0.6 + 0.4 * ring);\n"
      "        return;\n"
      "    }\n"
      "    if (front > 0.5 && v > 0.22 && v < 0.6 && ax < 0.58 && u < 0.04) {\n"                  /* grille */
      "        float slat = step(0.45, fract(lp.y * 28.0));\n"
      "        s.albedo = vec3(0.02 + 0.05 * slat); s.metallic = 0.6; s.roughness = 0.35; return;\n"
      "    }\n"
      "    if (u < 0.03 && v > 0.1 && v < 0.22 && ax < 0.26) { s.albedo = vec3(0.9, 0.9, 0.85) * (0.6 + 0.4 * step(0.25, fract(lp.x * 11.0))); s.metallic = 0.0; s.roughness = 0.4; return; }\n"
      "    if (rear > 0.5 && v > 0.56 && v < 0.94 && ax > 0.26 && ax < 0.92) {\n"                 /* tail lights */
      "        float cells = step(0.15, fract(ax * 14.0));\n"
      "        float bar = step(0.3, fract(v * 9.0));\n"
      "        float reverse = step(ax, 0.42) * step(v, 0.7);\n"                                  /* white reversing lamp inboard */
      "        s.albedo = mix(vec3(0.28, 0.015, 0.015) * (0.7 + 0.3 * bar), vec3(0.6), reverse); s.metallic = 0.3; s.roughness = 0.06;\n"
      "        s.emissive = vec3(1.0, 0.03, 0.02) * (0.04 + 2.0 * night) * (0.5 + 0.5 * cells * bar) * (1.0 - reverse);\n"
      "        return;\n"
      "    }\n"
      "    if (u > 4.97 && v > 0.36 && v < 0.5 && ax < 0.26) { s.albedo = vec3(0.9, 0.9, 0.85) * (0.6 + 0.4 * step(0.25, fract(lp.x * 11.0))); s.metallic = 0.0; s.roughness = 0.4; return; }\n"
      "    if (side > 0.6 && v > 0.17 && v < 1.0) {\n"                                         /* door seams and handles */
      "        float seam = step(abs(u - 1.12), 0.004) + step(abs(u - 2.43), 0.004) + step(abs(u - 3.32), 0.004);\n"
      "        s.albedo *= 1.0 - 0.85 * clamp(seam, 0.0, 1.0);\n"
      "        float handle = step(abs(v - 0.87), 0.025) * (step(abs(u - 2.25), 0.05) + step(abs(u - 3.18), 0.05));\n"
      "        if (handle > 0.5) { s.albedo = vec3(0.7); s.metallic = 1.0; s.roughness = 0.1; }\n"
      "    }\n"
      "    s.albedo *= 1.0 - 0.18 * (1.0 - smoothstep(0.1, 0.45, v));\n"                        /* road grime low on the body */
      "}\n" },
    { "wheel", "Tire and alloy rim for builtin:wheel_detailed: tread, sidewall, five-spoke rim, brake disc, lug nuts.",
      "void a3_surface(inout A3Surface s) {\n"
      "    vec3 lp = s.local_pos; float r = length(lp.yz); float ang = atan(lp.y, lp.z);\n"
      "    vec3 fn = normalize(cross(dFdx(lp), dFdy(lp)));\n"
      "    if (r > 0.615) {\n"
      "        s.albedo = vec3(0.035); s.metallic = 0.0; s.roughness = 0.85;\n"
      "        if (abs(fn.x) < 0.5) { float tread = step(0.55, fract(ang * 9.549 + abs(lp.x) * 2.5)) * step(0.04, abs(lp.x)); s.albedo *= 0.6 + 0.6 * tread; s.roughness = 0.9; }\n"
      "        else { s.albedo *= 1.0 + 0.4 * step(abs(r - 0.8), 0.01); }\n"
      "        return;\n"
      "    }\n"
      "    if (lp.x < 0.0) { s.albedo = vec3(0.05); s.metallic = 0.5; s.roughness = 0.6; return; }\n"
      "    float spoke = smoothstep(0.35, 0.55, cos(ang * 5.0) * 0.5 + 0.5);\n"
      "    vec3 alloy = vec3(0.72, 0.73, 0.75);\n"
      "    if (r > 0.2 && r < 0.54 && spoke < 0.5) { s.albedo = vec3(0.12) * (0.8 + 0.4 * a3_noise(lp * 60.0)); s.metallic = 0.8; s.roughness = 0.5; return; }\n"  /* brake disc */
      "    s.albedo = alloy; s.metallic = 1.0; s.roughness = 0.22;\n"
      "    if (r < 0.1) { s.albedo = vec3(0.1); s.roughness = 0.3; }\n"
      "    float nut = step(length(vec2(r - 0.16, (fract(ang * 0.7958 + 0.1) - 0.5) * 0.4)), 0.022);\n"
      "    s.albedo = mix(s.albedo, vec3(0.9), nut);\n"
      "}\n" },
    { "palm", "Palm tree for builtin:palm_a/b/c: ringed bark, glossy fronds that glow when backlit by the sun, dry fronds, coconuts. Base Color tints the leaves.",
      H2
      "void a3_surface(inout A3Surface s) {\n"
      "    float region = floor(s.uv.x), f = fract(s.uv.x);\n"
      "    if (region < 0.5) {\n"
      "        float y = s.uv.y; float ring = smoothstep(0.35, 0.5, abs(fract(y / 0.22) - 0.5));\n"
      "        float fiber = a3_noise(vec3(f * 40.0, y * 3.0, 0.0));\n"
      "        s.albedo = mix(vec3(0.46, 0.4, 0.33), vec3(0.26, 0.22, 0.18), ring) * (0.8 + 0.35 * fiber);\n"
      "        s.albedo = mix(s.albedo, vec3(0.3, 0.33, 0.2), smoothstep(8.0, 9.5, y));\n"          /* green crown shaft */
      "        s.roughness = 0.92; s.metallic = 0.0; return;\n"
      "    }\n"
      "    if (region < 2.5) {\n"
      "        float t = s.uv.y; float jit = a3_h2(vec2(floor(f * 20.0), region));\n"
      "        vec3 green = s.color.rgb * (0.75 + 0.35 * jit) * mix(vec3(0.85, 0.95, 0.7), vec3(1.05, 1.0, 0.75), t);\n"
      "        vec3 dry = vec3(0.55, 0.43, 0.26) * (0.8 + 0.3 * jit);\n"
      "        s.albedo = region < 1.5 ? green : dry;\n"
      "        s.roughness = region < 1.5 ? 0.45 : 0.85; s.metallic = 0.0;\n"
      "        float back = pow(max(dot(s.view_dir, u_sun_dir), 0.0), 3.0);\n"                    /* light shining through the leaf */
      "        s.emissive = s.albedo * u_sun_color * back * (region < 1.5 ? 0.35 : 0.15);\n"
      "        return;\n"
      "    }\n"
      "    s.albedo = vec3(0.35, 0.3, 0.12) * (0.8 + 0.3 * a3_noise(s.world_pos * 20.0)); s.roughness = 0.6; s.metallic = 0.0;\n"
      "}\n" },
    { "human", "People (builtin:human_* parts): skin tone from Metallic (0 light .. 1 dark), hair / trousers / shoes style from Roughness, shirt color from Base Color.",
      "vec3 a3_skin(float k) { return mix(mix(vec3(0.95, 0.78, 0.66), vec3(0.76, 0.55, 0.40), smoothstep(0.0, 0.5, k)), vec3(0.36, 0.24, 0.17), smoothstep(0.5, 1.0, k)); }\n"
      "void a3_surface(inout A3Surface s) {\n"
      "    float region = floor(s.uv.x); float tone = s.metallic, style = s.roughness;\n"
      "    s.metallic = 0.0;\n"
      "    vec3 skin = a3_skin(tone);\n"
      "    float cloth = 0.9 + 0.2 * a3_noise(s.local_pos * 90.0);\n"
      "    vec3 trousers = style < 0.33 ? vec3(0.12, 0.17, 0.3) : style < 0.66 ? vec3(0.55, 0.47, 0.34) : vec3(0.06, 0.06, 0.07);\n"
      "    vec3 hair = style < 0.25 ? vec3(0.03, 0.025, 0.02) : style < 0.5 ? vec3(0.22, 0.13, 0.07) : style < 0.75 ? vec3(0.7, 0.55, 0.3) : vec3(0.5, 0.48, 0.46);\n"
      "    vec3 shoe = fract(style * 7.0) < 0.5 ? vec3(0.9) : vec3(0.05);\n"
      "    if (region < 0.5) { s.albedo = skin; s.roughness = 0.55; }\n"
      "    else if (region < 1.5) { s.albedo = s.color.rgb * cloth; s.roughness = 0.8; }\n"
      "    else if (region < 2.5) { s.albedo = trousers * cloth; s.roughness = 0.85; }\n"
      "    else if (region < 3.5) { s.albedo = shoe; s.roughness = 0.5; }\n"
      "    else { s.albedo = hair * (0.85 + 0.3 * a3_noise(s.local_pos * 120.0)); s.roughness = 0.6; }\n"
      "}\n" },
    { "signal", "Traffic signal (builtin:traffic_light): lamps follow a 30 s cycle shared with the traffic AI. Metallic = phase offset (0..1), Roughness > 0.5 = cross street.",
      NIGHT
      "void a3_surface(inout A3Surface s) {\n"
      "    float region = floor(s.uv.x); float phase = s.metallic, group = s.roughness;\n"
      "    s.albedo = vec3(0.12, 0.13, 0.12); s.metallic = 0.7; s.roughness = 0.45;\n"
      "    if (region < 20.5) return;\n"
      "    if (region < 21.5) { s.albedo = vec3(0.03); s.metallic = 0.0; s.roughness = 0.6; return; }\n"
      "    float t = mod(s.time + phase * 30.0 + (group > 0.5 ? 15.0 : 0.0), 30.0);\n"
      "    float lamp = t < 12.0 ? 24.0 : t < 15.0 ? 23.0 : 22.0;\n"                                /* green, amber, red */
      "    vec3 lc = region < 22.5 ? vec3(1.0, 0.06, 0.03) : region < 23.5 ? vec3(1.0, 0.55, 0.02) : vec3(0.1, 1.0, 0.35);\n"
      "    float on = step(abs(region - lamp), 0.1);\n"
      "    s.albedo = lc * 0.12; s.metallic = 0.0; s.roughness = 0.15;\n"
      "    s.emissive = lc * on * (2.5 + 5.0 * a3_night());\n"
      "}\n" },
    { "props", "Street furniture (builtin:hydrant, bench, trash_can, street_lamp): painted metal, wood slats, lamp glass lit at night.",
      NIGHT
      "void a3_surface(inout A3Surface s) {\n"
      "    float region = floor(s.uv.x);\n"
      "    float n = a3_noise(s.world_pos * 8.0);\n"
      "    if (region < 20.0) { s.albedo = vec3(0.14, 0.15, 0.15) * (0.9 + 0.2 * n); s.metallic = 0.75; s.roughness = 0.4; return; }\n"
      "    if (region < 26.0) { s.albedo = vec3(0.9, 0.85, 0.7); s.metallic = 0.0; s.roughness = 0.2; s.emissive = vec3(1.0, 0.75, 0.45) * (0.1 + 6.0 * a3_night()); return; }\n"
      "    if (region < 30.5) { s.albedo = vec3(0.72, 0.08, 0.05) * (0.85 + 0.3 * n); s.metallic = 0.3; s.roughness = 0.35; return; }\n"
      "    if (region < 31.5) { s.albedo = vec3(0.45, 0.3, 0.18) * (0.75 + 0.4 * a3_noise(vec3(s.local_pos.x * 2.0, s.local_pos.y * 40.0, s.local_pos.z * 40.0))); s.metallic = 0.0; s.roughness = 0.7; return; }\n"
      "    s.albedo = vec3(0.1, 0.2, 0.14) * (0.9 + 0.2 * n); s.metallic = 0.6; s.roughness = 0.45;\n"
      "}\n" },
};
u32 a3_builtin_material_count(void) { return A3_ARRAY_COUNT(g_builtins); }

const char *a3_builtin_material_name(u32 i) { return i < A3_ARRAY_COUNT(g_builtins) ? g_builtins[i].name : 0; }
const char *a3_builtin_material_doc(u32 i) { return i < A3_ARRAY_COUNT(g_builtins) ? g_builtins[i].doc : 0; }

const char *a3_builtin_material_code(const char *name) {
    if (a3_str_starts_with(name, "builtin:")) name += 8;
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_builtins); ++i) if (a3_streq(g_builtins[i].name, name)) return g_builtins[i].code;
    return 0;
}
