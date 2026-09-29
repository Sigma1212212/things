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

typedef struct BuiltinMaterial { const char *name; const char *doc; const char *code; } BuiltinMaterial;

static const BuiltinMaterial g_builtins[] = {
    { "building", "Office / apartment facade: floors every 3.5 m, lit windows at night, flat roof. Base Color = wall color.",
      NIGHT H2
      "void a3_surface(inout A3Surface s) {\n"
      "    vec3 n = normalize(s.normal);\n"
      "    vec3 wall = s.color.rgb * (0.9 + 0.2 * a3_noise(s.world_pos * 0.7));\n"
      "    if (abs(n.y) > 0.6) { s.albedo = vec3(0.32, 0.31, 0.30) * (0.8 + 0.4 * a3_noise(s.world_pos * 2.0)); s.roughness = 0.95; return; }\n"
      "    vec3 t = normalize(cross(vec3(0.0, 1.0, 0.0), n));\n"
      "    float u = dot(s.world_pos, t), y = s.world_pos.y;\n"
      "    vec2 cell = vec2(floor(u / 3.0), floor(y / 3.5));\n"
      "    vec2 f = vec2(fract(u / 3.0), fract(y / 3.5));\n"
      "    float win = step(0.18, f.x) * step(f.x, 0.82) * step(0.25, f.y) * step(f.y, 0.85) * step(1.0, y);\n"
      "    float night = a3_night();\n"
      "    float rnd = a3_h2(cell + floor(s.world_pos.xz / 40.0) * 7.31);\n"
      "    float lit = step(0.62, rnd) * (0.35 + 0.65 * a3_h2(cell * 3.3 + 1.7));\n"       /* ~40% lit, varied brightness */
      "    vec3 glass = vec3(0.05, 0.07, 0.09);\n"
      "    float kind = a3_h2(cell * 1.7);\n"
      "    vec3 warm = kind < 0.6 ? vec3(1.0, 0.68, 0.38) : kind < 0.85 ? vec3(0.95, 0.85, 0.7) : vec3(0.6, 0.75, 1.0);\n"
      "    warm *= 0.8 + 0.2 * step(0.5, fract(u / 1.5)) * step(0.5, f.y);\n"                 /* hint of curtains */
      "    s.albedo = mix(wall, glass, win);\n"
      "    s.roughness = mix(0.85, 0.12, win);\n"
      "    s.metallic = mix(0.0, 0.4, win);\n"
      "    s.emissive = warm * win * lit * night * 1.25;\n"
      "}\n" },
    { "artdeco", "Pastel Art Deco hotel: horizontal eyebrow ledges, porthole-style windows and neon trim that glows at night. Base Color = pastel wall.",
      NIGHT H2
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
      "    s.albedo = mix(mix(wall, wall * 1.15, ledge), vec3(0.04, 0.06, 0.08), win);\n"
      "    s.roughness = mix(0.8, 0.15, win);\n"
      "    s.emissive = vec3(1.0, 0.75, 0.5) * win * lit * night * 1.1 + neon * trim * (0.3 + 3.2 * night);\n"
      "}\n" },
    { "tower", "Glass skyscraper: reflective curtain wall with mullions, some offices lit at night, red beacon on top. Base Color = glass tint.",
      NIGHT H2
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
      "    s.emissive = office * (1.0 - mull) * lit * night * 0.6;\n"
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
      "    s.albedo = s.color.rgb * (0.85 + 0.25 * a3_noise(s.world_pos * 3.0)) * (1.0 - joint * 0.35);\n"
      "    s.roughness = 0.75;\n"
      "}\n" },
    { "sand", "Beach sand with ripples. Base Color = sand tint.",
      "void a3_surface(inout A3Surface s) {\n"
      "    float r = sin(s.world_pos.x * 2.3 + a3_noise(s.world_pos * 0.5) * 6.0) * 0.5 + 0.5;\n"
      "    s.albedo = s.color.rgb * (0.88 + 0.12 * r + 0.1 * a3_noise(s.world_pos * 9.0));\n"
      "    s.roughness = 0.95;\n"
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
};

u32 a3_builtin_material_count(void) { return A3_ARRAY_COUNT(g_builtins); }

const char *a3_builtin_material_name(u32 i) { return i < A3_ARRAY_COUNT(g_builtins) ? g_builtins[i].name : 0; }
const char *a3_builtin_material_doc(u32 i) { return i < A3_ARRAY_COUNT(g_builtins) ? g_builtins[i].doc : 0; }

const char *a3_builtin_material_code(const char *name) {
    if (a3_str_starts_with(name, "builtin:")) name += 8;
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_builtins); ++i) if (a3_streq(g_builtins[i].name, name)) return g_builtins[i].code;
    return 0;
}
