/*
 * ASM3D - a3_shader_graph.c
 */
#include "a3_shader_graph.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_json.h"
#include "../core/a3_memory.h"
#include "../core/a3_log.h"

#define F A3_SG_FLOAT
#define V2 A3_SG_VEC2
#define V3 A3_SG_VEC3
#define V4 A3_SG_VEC4
#define G A3_SG_GENERIC

static const A3SgNodeDef g_defs[A3_SGN_COUNT] = {
    [A3_SGN_OUTPUT] = { "Output", "Output", "The final material. Anything left unconnected keeps the object's Mesh Renderer settings.",
        6, 0, { "Base Color", "Metallic", "Roughness", "Emissive", "Alpha", "Normal" }, { V3, F, F, V3, F, V3 } },
    /* ---- inputs ---- */
    [A3_SGN_UV] = { "UV", "Input", "Texture coordinates of the surface (0..1).", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "UV" }, { V2 }, { "" }, V2, "s.uv" },
    [A3_SGN_WORLD_POS] = { "World Position", "Input", "Position of the pixel in the world, in meters.", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Position" }, { V3 }, { "" }, V3, "s.world_pos" },
    [A3_SGN_LOCAL_POS] = { "Object Position", "Input", "Position of the pixel relative to the object (moves with it).", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Position" }, { V3 }, { "" }, V3, "s.local_pos" },
    [A3_SGN_NORMAL] = { "Normal", "Input", "Direction the surface faces.", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Normal" }, { V3 }, { "" }, V3, "s.normal" },
    [A3_SGN_VIEW_DIR] = { "View Direction", "Input", "Direction from the surface towards the camera.", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Direction" }, { V3 }, { "" }, V3, "s.view_dir" },
    [A3_SGN_TIME] = { "Time", "Input", "Seconds since the game started. Use it to animate.", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Seconds" }, { F }, { "" }, F, "s.time" },
    [A3_SGN_OBJECT_COLOR] = { "Object Color", "Input", "The Color set on the object's Mesh Renderer.", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Color" }, { V3 }, { "" }, V3, "s.color.rgb" },
    /* ---- constants ---- */
    [A3_SGN_NUMBER] = { "Number", "Constant", "A fixed number.", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Value" }, { F }, { "" }, F, "$V", A3_SG_VALUE_FLOAT },
    [A3_SGN_COLOR] = { "Color", "Constant", "A fixed color.", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Color" }, { V3 }, { "" }, V3, "$V", A3_SG_VALUE_COLOR },
    [A3_SGN_VECTOR] = { "Vector", "Constant", "Three fixed numbers (x, y, z).", 0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Vector" }, { V3 }, { "" }, V3, "$V", A3_SG_VALUE_VEC3 },
    [A3_SGN_PARAM_NUMBER] = { "Number Parameter", "Parameter", "A number you can tweak later without recompiling (up to 4 parameters).",
        0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Value" }, { F }, { "" }, F, "u_material[$P].x", A3_SG_VALUE_FLOAT, 1 },
    [A3_SGN_PARAM_COLOR] = { "Color Parameter", "Parameter", "A color you can tweak later without recompiling (up to 4 parameters).",
        0, 1, { 0 }, { 0 }, { 0 }, { { 0 } }, { "Color" }, { V3 }, { "" }, V3, "u_material[$P].rgb", A3_SG_VALUE_COLOR, 1 },
    [A3_SGN_TEXTURE] = { "Texture", "Input", "Reads an image. Drag a texture from the Assets panel onto the node (2 textures per material).",
        1, 2, { "UV" }, { V2 }, { "s.uv" }, { { 0 } }, { "Color", "Alpha" }, { V3, F }, { ".rgb", ".a" }, V4, "texture($T, $0)", A3_SG_VALUE_TEXTURE },
    /* ---- math ---- */
    [A3_SGN_ADD] = { "Add", "Math", "A + B", 2, 1, { "A", "B" }, { G, G }, { 0 }, { { 0 }, { 0 } }, { "Result" }, { G }, { "" }, G, "($0 + $1)" },
    [A3_SGN_SUB] = { "Subtract", "Math", "A - B", 2, 1, { "A", "B" }, { G, G }, { 0 }, { { 0 }, { 0 } }, { "Result" }, { G }, { "" }, G, "($0 - $1)" },
    [A3_SGN_MUL] = { "Multiply", "Math", "A x B. Multiplying colors tints them.", 2, 1, { "A", "B" }, { G, G }, { 0 }, { { 1 }, { 1 } }, { "Result" }, { G }, { "" }, G, "($0 * $1)" },
    [A3_SGN_DIV] = { "Divide", "Math", "A / B", 2, 1, { "A", "B" }, { G, G }, { 0 }, { { 1 }, { 1 } }, { "Result" }, { G }, { "" }, G, "($0 / $1)" },
    [A3_SGN_MIX] = { "Blend", "Math", "Blends from A to B by Amount (0 = A, 1 = B).", 3, 1, { "A", "B", "Amount" }, { G, G, F }, { 0 }, { { 0 }, { 1 }, { 0.5f } }, { "Result" }, { G }, { "" }, G, "mix($0, $1, $2)" },
    [A3_SGN_POW] = { "Power", "Math", "A raised to the power B.", 2, 1, { "A", "B" }, { G, G }, { 0 }, { { 1 }, { 2 } }, { "Result" }, { G }, { "" }, G, "pow(max($0, 0.0), $1)" },
    [A3_SGN_MIN] = { "Minimum", "Math", "The smaller of A and B.", 2, 1, { "A", "B" }, { G, G }, { 0 }, { { 0 }, { 1 } }, { "Result" }, { G }, { "" }, G, "min($0, $1)" },
    [A3_SGN_MAX] = { "Maximum", "Math", "The larger of A and B.", 2, 1, { "A", "B" }, { G, G }, { 0 }, { { 0 }, { 1 } }, { "Result" }, { G }, { "" }, G, "max($0, $1)" },
    [A3_SGN_ABS] = { "Absolute", "Math", "Removes the minus sign.", 1, 1, { "In" }, { G }, { 0 }, { { 0 } }, { "Result" }, { G }, { "" }, G, "abs($0)" },
    [A3_SGN_FRACT] = { "Fraction", "Math", "Keeps only the part after the decimal point (repeats 0..1).", 1, 1, { "In" }, { G }, { 0 }, { { 0 } }, { "Result" }, { G }, { "" }, G, "fract($0)" },
    [A3_SGN_FLOOR] = { "Floor", "Math", "Rounds down.", 1, 1, { "In" }, { G }, { 0 }, { { 0 } }, { "Result" }, { G }, { "" }, G, "floor($0)" },
    [A3_SGN_SIN] = { "Sine", "Math", "Smooth wave between -1 and 1.", 1, 1, { "In" }, { G }, { 0 }, { { 0 } }, { "Result" }, { G }, { "" }, G, "sin($0)" },
    [A3_SGN_COS] = { "Cosine", "Math", "Smooth wave between -1 and 1 (shifted sine).", 1, 1, { "In" }, { G }, { 0 }, { { 0 } }, { "Result" }, { G }, { "" }, G, "cos($0)" },
    [A3_SGN_ONE_MINUS] = { "One Minus", "Math", "1 - In. Inverts masks and colors.", 1, 1, { "In" }, { G }, { 0 }, { { 0 } }, { "Result" }, { G }, { "" }, G, "(1.0 - $0)" },
    [A3_SGN_SATURATE] = { "Clamp 0-1", "Math", "Keeps the value between 0 and 1.", 1, 1, { "In" }, { G }, { 0 }, { { 0 } }, { "Result" }, { G }, { "" }, G, "clamp($0, 0.0, 1.0)" },
    [A3_SGN_SMOOTHSTEP] = { "Smooth Step", "Math", "0 below Edge 1, 1 above Edge 2, smooth in between.", 3, 1, { "In", "Edge 1", "Edge 2" }, { G, F, F }, { 0 }, { { 0 }, { 0 }, { 1 } }, { "Result" }, { G }, { "" }, G, "smoothstep($1, $2, $0)" },
    [A3_SGN_STEP] = { "Step", "Math", "0 below Edge, 1 at or above it.", 2, 1, { "In", "Edge" }, { G, F }, { 0 }, { { 0 }, { 0.5f } }, { "Result" }, { G }, { "" }, G, "step($1, $0)" },
    /* ---- vectors ---- */
    [A3_SGN_DOT] = { "Dot Product", "Vector", "How much two directions point the same way (-1..1).", 2, 1, { "A", "B" }, { V3, V3 }, { 0 }, { { 0, 1, 0 }, { 0, 1, 0 } }, { "Result" }, { F }, { "" }, F, "dot($0, $1)" },
    [A3_SGN_NORMALIZE] = { "Normalize", "Vector", "Makes a direction 1 unit long.", 1, 1, { "In" }, { V3 }, { 0 }, { { 0, 1, 0 } }, { "Result" }, { V3 }, { "" }, V3, "normalize($0)" },
    [A3_SGN_LENGTH] = { "Length", "Vector", "Length of a vector (distance).", 1, 1, { "In" }, { G }, { 0 }, { { 0 } }, { "Length" }, { F }, { "" }, F, "length($0)" },
    [A3_SGN_SPLIT] = { "Split", "Vector", "Separates X, Y and Z (or R, G and B).", 1, 3, { "In" }, { V3 }, { 0 }, { { 0 } }, { "X", "Y", "Z" }, { F, F, F }, { ".x", ".y", ".z" }, V3, "$0" },
    [A3_SGN_COMBINE] = { "Combine", "Vector", "Builds a vector (or color) from three numbers.", 3, 1, { "X", "Y", "Z" }, { F, F, F }, { 0 }, { { 0 }, { 0 }, { 0 } }, { "Vector" }, { V3 }, { "" }, V3, "vec3($0, $1, $2)" },
    /* ---- patterns ---- */
    [A3_SGN_NOISE] = { "Noise", "Pattern", "Smooth random clouds (0..1).", 2, 1, { "Position", "Scale" }, { V3, F }, { "s.world_pos", 0 }, { { 0 }, { 4 } }, { "Value" }, { F }, { "" }, F, "a3_noise($0 * $1)" },
    [A3_SGN_FBM] = { "Detailed Noise", "Pattern", "Layered noise with fine detail (rock, clouds, lava).", 2, 1, { "Position", "Scale" }, { V3, F }, { "s.world_pos", 0 }, { { 0 }, { 2 } }, { "Value" }, { F }, { "" }, F, "a3_fbm($0 * $1)" },
    [A3_SGN_CHECKER] = { "Checker", "Pattern", "Checkerboard of 0 and 1.", 2, 1, { "UV", "Scale" }, { V2, F }, { "s.uv", 0 }, { { 0 }, { 8 } }, { "Value" }, { F }, { "" }, F, "mod(floor($0.x * $1) + floor($0.y * $1), 2.0)" },
    [A3_SGN_FRESNEL] = { "Rim Light", "Pattern", "Bright at the edges of the object, dark facing the camera (Fresnel).", 1, 1, { "Power" }, { F }, { 0 }, { { 3 } }, { "Value" }, { F }, { "" }, F, "pow(1.0 - clamp(dot(s.normal, s.view_dir), 0.0, 1.0), $0)" },
    [A3_SGN_PANNER] = { "Scroll UV", "Pattern", "Moves texture coordinates over time (flowing water, conveyor belts).", 2, 1, { "UV", "Speed" }, { V2, V2 }, { "s.uv", 0 }, { { 0 }, { 0.1f, 0 } }, { "UV" }, { V2 }, { "" }, V2, "($0 + $1 * s.time)" },
    [A3_SGN_STRIPES] = { "Stripes", "Pattern", "Soft repeating stripes along In.", 2, 1, { "In", "Count" }, { F, F }, { 0 }, { { 0 }, { 10 } }, { "Value" }, { F }, { "" }, F, "(0.5 + 0.5 * sin($0 * $1 * 6.2831853))" },
    /* ---- advanced ---- */
    [A3_SGN_CODE] = { "Custom Code", "Advanced", "Your own GLSL expression. Inputs are a (vec3), b (vec3) and t (float); the result is a vec3.",
        3, 1, { "a", "b", "t" }, { V3, V3, F }, { 0 }, { { 0 }, { 0 }, { 0 } }, { "Result" }, { V3 }, { "" }, V3, "$V", A3_SG_VALUE_CODE },
};

static const char *const g_type_names[] = { "float", "vec2", "vec3", "vec4", "float" };
static const u32 g_type_width[] = { 1, 2, 3, 4, 1 };

const A3SgNodeDef *a3_sg_def(u32 type) { return type < A3_SGN_COUNT ? &g_defs[type] : 0; }

u32 a3_sg_type_from_name(const char *name) {
    for (u32 i = 0; i < A3_SGN_COUNT; ++i) if (g_defs[i].name && a3_streq(g_defs[i].name, name)) return i;
    return A3_SGN_COUNT;
}

/* ======================================================================== */
/* Editing                                                                  */
/* ======================================================================== */

static void node_defaults(A3SgNode *n, u32 type, A3Vec2 pos) {
    a3_zero_struct(n);
    const A3SgNodeDef *d = &g_defs[type];
    n->used = 1;
    n->type = (u16)type;
    n->pos = pos;
    for (u32 i = 0; i < A3_SG_MAX_PINS; ++i) {
        n->src[i] = -1;
        for (u32 k = 0; k < 4; ++k) n->in_value[i][k] = d->in_defaults[i][k];
    }
    n->param = -1;
    switch (d->value_kind) {
    case A3_SG_VALUE_FLOAT: n->value[0] = type == A3_SGN_PARAM_NUMBER ? 0.5f : 1.0f; break;
    case A3_SG_VALUE_COLOR: n->value[0] = 0.9f; n->value[1] = 0.45f; n->value[2] = 0.2f; n->value[3] = 1; break;
    case A3_SG_VALUE_VEC3: n->value[1] = 1; break;
    case A3_SG_VALUE_CODE: a3_strcpy(n->text, sizeof(n->text), "a * (0.5 + 0.5 * sin(t))"); break;
    default: break;
    }
    if (d->is_param) a3_strcpy(n->text, sizeof(n->text), type == A3_SGN_PARAM_COLOR ? "Tint" : "Strength");
}

void a3_sg_init(A3SgGraph *g) {
    a3_zero_struct(g);
    a3_strcpy(g->name, sizeof(g->name), "New Shader");
    node_defaults(&g->nodes[0], A3_SGN_OUTPUT, a3_v2(420, 60));
}

i32 a3_sg_output(const A3SgGraph *g) {
    for (i32 i = 0; i < A3_SG_MAX_NODES; ++i) if (g->nodes[i].used && g->nodes[i].type == A3_SGN_OUTPUT) return i;
    return -1;
}

i32 a3_sg_add(A3SgGraph *g, u32 type, A3Vec2 pos) {
    if (type >= A3_SGN_COUNT || type == A3_SGN_OUTPUT) return -1;
    for (i32 i = 0; i < A3_SG_MAX_NODES; ++i)
        if (!g->nodes[i].used) { node_defaults(&g->nodes[i], type, pos); return i; }
    return -1;
}

void a3_sg_remove(A3SgGraph *g, i32 node) {
    if (node < 0 || node >= A3_SG_MAX_NODES || !g->nodes[node].used || g->nodes[node].type == A3_SGN_OUTPUT) return;
    g->nodes[node].used = 0;
    for (i32 i = 0; i < A3_SG_MAX_NODES; ++i)
        for (u32 k = 0; k < A3_SG_MAX_PINS; ++k) if (g->nodes[i].src[k] == node) g->nodes[i].src[k] = -1;
}

b32 a3_sg_depends_on(const A3SgGraph *g, i32 node, i32 upstream) {
    /* iterative DFS over inputs; bounded by the node count */
    i32 stack[A3_SG_MAX_NODES * A3_SG_MAX_PINS];
    u8 seen[A3_SG_MAX_NODES] = { 0 };
    i32 sp = 0;
    stack[sp++] = node;
    while (sp) {
        i32 n = stack[--sp];
        if (n == upstream) return 1;
        if (n < 0 || n >= A3_SG_MAX_NODES || seen[n] || !g->nodes[n].used) continue;
        seen[n] = 1;
        for (u32 k = 0; k < A3_SG_MAX_PINS; ++k) if (g->nodes[n].src[k] >= 0 && sp < (i32)A3_ARRAY_COUNT(stack)) stack[sp++] = g->nodes[n].src[k];
    }
    return 0;
}

b32 a3_sg_connect(A3SgGraph *g, i32 src, i32 src_out, i32 dst, i32 dst_in) {
    if (src < 0 || dst < 0 || src >= A3_SG_MAX_NODES || dst >= A3_SG_MAX_NODES || src == dst) return 0;
    if (!g->nodes[src].used || !g->nodes[dst].used) return 0;
    if (src_out < 0 || src_out >= g_defs[g->nodes[src].type].out_count) return 0;
    if (dst_in < 0 || dst_in >= g_defs[g->nodes[dst].type].in_count) return 0;
    if (a3_sg_depends_on(g, src, dst)) return 0; /* would create a loop */
    g->nodes[dst].src[dst_in] = (i16)src;
    g->nodes[dst].src_out[dst_in] = (i8)src_out;
    return 1;
}

void a3_sg_disconnect(A3SgGraph *g, i32 dst, i32 dst_in) {
    if (dst < 0 || dst >= A3_SG_MAX_NODES || dst_in < 0 || dst_in >= A3_SG_MAX_PINS) return;
    g->nodes[dst].src[dst_in] = -1;
}

/* Resolved type of a node variable (generic nodes take the widest input). */
static A3SgType node_type(const A3SgGraph *g, i32 n, u32 depth) {
    const A3SgNodeDef *d = &g_defs[g->nodes[n].type];
    if (d->var_type != G) return (A3SgType)d->var_type;
    A3SgType best = A3_SG_FLOAT;
    if (depth > A3_SG_MAX_NODES) return best;
    for (u32 i = 0; i < d->in_count; ++i) {
        if (d->in_types[i] != G || g->nodes[n].src[i] < 0) continue;
        i32 s = g->nodes[n].src[i];
        const A3SgNodeDef *sd = &g_defs[g->nodes[s].type];
        A3SgType t = sd->out_types[g->nodes[n].src_out[i]] == G ? node_type(g, s, depth + 1) : (A3SgType)sd->out_types[g->nodes[n].src_out[i]];
        if (g_type_width[t] > g_type_width[best]) best = t;
    }
    return best;
}

A3SgType a3_sg_output_type(const A3SgGraph *g, i32 node, i32 out) {
    const A3SgNodeDef *d = &g_defs[g->nodes[node].type];
    return d->out_types[out] == G ? node_type(g, node, 0) : (A3SgType)d->out_types[out];
}

/* ======================================================================== */
/* Code generation                                                          */
/* ======================================================================== */

static void fmt_float(char *out, usize cap, f32 v) {
    if (!(v == v)) v = 0; /* NaN guard */
    a3_snprintf(out, cap, "%.5f", (f64)v);
    /* trim trailing zeros but keep one digit after the point */
    usize n = a3_strlen(out);
    while (n > 2 && out[n - 1] == '0' && out[n - 2] != '.') out[--n] = 0;
}

static void literal(char *out, usize cap, A3SgType t, const f32 *v) {
    char a[32], b[32], c[32], d[32];
    fmt_float(a, sizeof(a), v[0]); fmt_float(b, sizeof(b), v[1]); fmt_float(c, sizeof(c), v[2]); fmt_float(d, sizeof(d), v[3]);
    switch (t) {
    case A3_SG_VEC2: a3_snprintf(out, cap, "vec2(%s, %s)", a, b); break;
    case A3_SG_VEC3: a3_snprintf(out, cap, "vec3(%s, %s, %s)", a, b, c); break;
    case A3_SG_VEC4: a3_snprintf(out, cap, "vec4(%s, %s, %s, %s)", a, b, c, d); break;
    default: a3_snprintf(out, cap, "%s", a); break;
    }
}

static void convert(char *out, usize cap, const char *e, A3SgType from, A3SgType to) {
    if (from == G) from = A3_SG_FLOAT;
    if (to == G) to = A3_SG_FLOAT;
    if (from == to) { a3_snprintf(out, cap, "%s", e); return; }
    static const char *const swz[] = { ".x", ".xy", ".xyz", "" };
    if (from == A3_SG_FLOAT) { a3_snprintf(out, cap, "%s(%s)", g_type_names[to], e); return; }
    if (g_type_width[to] < g_type_width[from]) { a3_snprintf(out, cap, "(%s)%s", e, swz[g_type_width[to] - 1]); return; }
    /* widen: vec2 -> vec3/vec4, vec3 -> vec4 */
    if (from == A3_SG_VEC2 && to == A3_SG_VEC3) a3_snprintf(out, cap, "vec3(%s, 0.0)", e);
    else if (from == A3_SG_VEC2) a3_snprintf(out, cap, "vec4(%s, 0.0, 1.0)", e);
    else a3_snprintf(out, cap, "vec4(%s, 1.0)", e);
}

typedef struct Gen {
    A3SgGraph *g;
    A3StrBuf *sb;
    A3SgCompiled *c;
    u8 state[A3_SG_MAX_NODES];   /* 0 new, 1 visiting, 2 done */
    u32 line;
    i32 tex_slot_of[A3_SG_MAX_NODES];
    u32 tex_count;
    b32 failed;
} Gen;

static void emit_line(Gen *gen, i32 node, const char *text) {
    a3_strbuf_append(gen->sb, "    ");
    a3_strbuf_append(gen->sb, text);
    a3_strbuf_append_char(gen->sb, '\n');
    if (gen->line < A3_ARRAY_COUNT(gen->c->line_node)) gen->c->line_node[gen->line] = node;
    gen->line++;
}

static b32 is_color_input(const char *name) { return name && (a3_strstr(name, "Color") != 0); }

/* Expression for input i of node n, converted to `want`. */
static void input_expr(Gen *gen, i32 n, u32 i, A3SgType want, char *out, usize cap) {
    A3SgNode *nd = &gen->g->nodes[n];
    const A3SgNodeDef *d = &g_defs[nd->type];
    char tmp[256];
    if (nd->src[i] >= 0) {
        i32 s = nd->src[i];
        const A3SgNodeDef *sd = &g_defs[gen->g->nodes[s].type];
        u32 o = (u32)nd->src_out[i];
        a3_snprintf(tmp, sizeof(tmp), "n%d%s", s, sd->out_access[o] ? sd->out_access[o] : "");
        convert(out, cap, tmp, a3_sg_output_type(gen->g, s, (i32)o), want);
        return;
    }
    if (d->in_default_expr[i]) { convert(out, cap, d->in_default_expr[i], (A3SgType)d->in_types[i], want); return; }
    if (d->in_types[i] == G) {
        literal(tmp, sizeof(tmp), A3_SG_FLOAT, nd->in_value[i]);
        convert(out, cap, tmp, A3_SG_FLOAT, want);
        return;
    }
    f32 v[4] = { nd->in_value[i][0], nd->in_value[i][1], nd->in_value[i][2], nd->in_value[i][3] };
    if (d->in_types[i] == A3_SG_VEC3 && is_color_input(d->in_names[i])) for (int k = 0; k < 3; ++k) v[k] = a3_srgb_to_linear(v[k]);
    literal(tmp, sizeof(tmp), (A3SgType)d->in_types[i], v);
    convert(out, cap, tmp, (A3SgType)d->in_types[i], want);
}

static void gen_node(Gen *gen, i32 n) {
    if (gen->failed) return;
    if (gen->state[n] == 2) return;
    if (gen->state[n] == 1) {
        gen->failed = 1;
        gen->c->error_node = n;
        a3_snprintf(gen->c->error, sizeof(gen->c->error), "The graph has a loop: a node feeds into itself.");
        return;
    }
    gen->state[n] = 1;
    A3SgNode *nd = &gen->g->nodes[n];
    const A3SgNodeDef *d = &g_defs[nd->type];
    for (u32 i = 0; i < d->in_count; ++i) {
        i32 s = nd->src[i];
        if (s < 0) continue;
        if (s >= A3_SG_MAX_NODES || !gen->g->nodes[s].used) { nd->src[i] = -1; continue; }
        gen_node(gen, s);
    }
    if (gen->failed) return;
    gen->state[n] = 2;
    if (nd->type == A3_SGN_OUTPUT) return;
    A3SgType vt = node_type(gen->g, n, 0);
    /* parameters and textures get slots in order of first use */
    if (d->is_param) {
        if (gen->c->param_count >= A3_SG_MAX_PARAMS) {
            gen->failed = 1;
            gen->c->error_node = n;
            a3_snprintf(gen->c->error, sizeof(gen->c->error), "Too many parameters: a material can have at most %d.", A3_SG_MAX_PARAMS);
            return;
        }
        nd->param = (i32)gen->c->param_count;
        A3Vec4 v = a3_v4(nd->value[0], nd->value[1], nd->value[2], 1);
        if (d->value_kind == A3_SG_VALUE_COLOR) { v.x = a3_srgb_to_linear(v.x); v.y = a3_srgb_to_linear(v.y); v.z = a3_srgb_to_linear(v.z); }
        else v = a3_v4(nd->value[0], nd->value[0], nd->value[0], 1);
        gen->c->params[gen->c->param_count++] = v;
    }
    if (d->value_kind == A3_SG_VALUE_TEXTURE) {
        i32 slot = -1;
        for (u32 k = 0; k < gen->tex_count; ++k) if (a3_streq(gen->c->textures[k], nd->text)) slot = (i32)k;
        if (slot < 0) {
            if (gen->tex_count >= 2) {
                gen->failed = 1;
                gen->c->error_node = n;
                a3_snprintf(gen->c->error, sizeof(gen->c->error), "Too many textures: a material can use at most 2 different images.");
                return;
            }
            slot = (i32)gen->tex_count++;
            a3_strcpy(gen->c->textures[slot], sizeof(gen->c->textures[slot]), nd->text);
        }
        nd->param = slot;
    }
    /* expand the expression template */
    char expr[1024];
    usize w = 0;
    expr[0] = 0;
    if (d->value_kind == A3_SG_VALUE_CODE) {
        char a[256], b[256], t[256];
        input_expr(gen, n, 0, A3_SG_VEC3, a, sizeof(a));
        input_expr(gen, n, 1, A3_SG_VEC3, b, sizeof(b));
        input_expr(gen, n, 2, A3_SG_FLOAT, t, sizeof(t));
        /* one line, so compile errors map to this node */
        char line[1400];
        a3_snprintf(line, sizeof(line), "vec3 n%d; { vec3 a = %s; vec3 b = %s; float t = %s; n%d = vec3(%s); }", n, a, b, t, n, nd->text[0] ? nd->text : "0.0");
        emit_line(gen, n, line);
        return;
    }
    for (const char *p = d->expr; *p && w + 300 < sizeof(expr); ++p) {
        char piece[512];
        piece[0] = 0;
        if (p[0] == '$' && p[1] >= '0' && p[1] <= '5') {
            u32 i = (u32)(p[1] - '0');
            A3SgType want = d->in_types[i] == G ? vt : (A3SgType)d->in_types[i];
            input_expr(gen, n, i, want, piece, sizeof(piece));
            ++p;
        } else if (p[0] == '$' && p[1] == 'V') {
            f32 v[4] = { nd->value[0], nd->value[1], nd->value[2], nd->value[3] };
            if (d->value_kind == A3_SG_VALUE_COLOR) { for (int k = 0; k < 3; ++k) v[k] = a3_srgb_to_linear(v[k]); literal(piece, sizeof(piece), A3_SG_VEC3, v); }
            else if (d->value_kind == A3_SG_VALUE_VEC3) literal(piece, sizeof(piece), A3_SG_VEC3, v);
            else literal(piece, sizeof(piece), A3_SG_FLOAT, v);
            ++p;
        } else if (p[0] == '$' && p[1] == 'P') {
            a3_snprintf(piece, sizeof(piece), "%d", nd->param);
            ++p;
        } else if (p[0] == '$' && p[1] == 'T') {
            a3_snprintf(piece, sizeof(piece), "%s", nd->param == 1 ? "u_tex2" : "u_tex1");
            ++p;
        } else {
            piece[0] = *p;
            piece[1] = 0;
        }
        usize pl = a3_strlen(piece);
        if (w + pl + 1 >= sizeof(expr)) break;
        a3_memcpy(expr + w, piece, pl);
        w += pl;
        expr[w] = 0;
    }
    char line[1200];
    a3_snprintf(line, sizeof(line), "%s n%d = %s;", g_type_names[vt], n, expr);
    emit_line(gen, n, line);
}

b32 a3_sg_compile(A3SgGraph *g, A3StrBuf *code, A3SgCompiled *out) {
    a3_zero_struct(out);
    out->error_node = -1;
    for (u32 i = 0; i < A3_ARRAY_COUNT(out->line_node); ++i) out->line_node[i] = -1;
    Gen gen;
    a3_zero_struct(&gen);
    gen.g = g;
    gen.sb = code;
    gen.c = out;
    gen.line = 1;
    i32 o = a3_sg_output(g);
    a3_strbuf_clear(code);
    a3_strbuf_appendf(code, "void a3_surface(inout A3Surface s) {\n");
    gen.line = 2;
    if (o < 0) {
        a3_snprintf(out->error, sizeof(out->error), "The graph has no Output node.");
    } else {
        gen_node(&gen, o);
        if (!gen.failed) {
            static const char *const targets[] = { "s.albedo", "s.metallic", "s.roughness", "s.emissive", "s.alpha", "s.normal" };
            const A3SgNodeDef *od = &g_defs[A3_SGN_OUTPUT];
            for (u32 i = 0; i < od->in_count; ++i) {
                if (g->nodes[o].src[i] < 0) continue; /* unconnected: keep the Mesh Renderer value */
                char e[512], line[640];
                input_expr(&gen, o, i, (A3SgType)od->in_types[i], e, sizeof(e));
                if (i == 5) a3_snprintf(line, sizeof(line), "%s = normalize(%s);", targets[i], e);
                else a3_snprintf(line, sizeof(line), "%s = %s;", targets[i], e);
                emit_line(&gen, o, line);
            }
        }
    }
    if (gen.failed || o < 0) {
        /* safe fallback so the object still renders (magenta = broken material) */
        a3_strbuf_clear(code);
        a3_strbuf_append(code, "void a3_surface(inout A3Surface s) {\n    s.albedo = vec3(1.0, 0.0, 1.0);\n}\n");
        out->line_count = 3;
        out->ok = 0;
        return 0;
    }
    a3_strbuf_append(code, "}\n");
    out->line_count = gen.line;
    out->ok = 1;
    return 1;
}

i32 a3_sg_node_for_line(const A3SgCompiled *c, i32 line) {
    if (!c || line <= 0 || line >= (i32)A3_ARRAY_COUNT(c->line_node)) return -1;
    return c->line_node[line];
}

/* ======================================================================== */
/* JSON                                                                     */
/* ======================================================================== */

void a3_sg_save_json(const A3SgGraph *g, const char *surface, const A3SgCompiled *c, A3StrBuf *out) {
    A3JsonWriter jw;
    a3_jw_init(&jw, out, 0);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", "asm3d.shader");
    a3_jw_kv_int(&jw, "version", 1);
    a3_jw_kv_string(&jw, "name", g->name);
    a3_jw_kv_string(&jw, "surface", surface ? surface : "");
    a3_jw_key(&jw, "params");
    a3_jw_begin_array(&jw);
    for (u32 i = 0; c && i < c->param_count; ++i) a3_jw_floats(&jw, &c->params[i].x, 4);
    a3_jw_end_array(&jw);
    a3_jw_key(&jw, "textures");
    a3_jw_begin_array(&jw);
    for (u32 i = 0; i < 2; ++i) a3_jw_string(&jw, c ? c->textures[i] : "");
    a3_jw_end_array(&jw);
    a3_jw_key(&jw, "nodes");
    a3_jw_begin_array(&jw);
    for (i32 i = 0; i < A3_SG_MAX_NODES; ++i) {
        const A3SgNode *n = &g->nodes[i];
        if (!n->used) continue;
        const A3SgNodeDef *d = &g_defs[n->type];
        a3_jw_begin_object(&jw);
        a3_jw_kv_int(&jw, "id", i);
        a3_jw_kv_string(&jw, "type", d->name);
        a3_jw_kv_floats(&jw, "pos", &n->pos.x, 2);
        a3_jw_key(&jw, "inputs");
        a3_jw_begin_array(&jw);
        for (u32 k = 0; k < d->in_count; ++k) {
            a3_jw_begin_object(&jw);
            if (n->src[k] >= 0) { a3_jw_kv_int(&jw, "node", n->src[k]); a3_jw_kv_int(&jw, "out", n->src_out[k]); }
            a3_jw_kv_floats(&jw, "value", n->in_value[k], 4);
            a3_jw_end_object(&jw);
        }
        a3_jw_end_array(&jw);
        if (d->value_kind != A3_SG_VALUE_NONE) a3_jw_kv_floats(&jw, "value", n->value, 4);
        if (n->text[0]) a3_jw_kv_string(&jw, "text", n->text);
        a3_jw_end_object(&jw);
    }
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    a3_strbuf_append_char(out, '\n');
}

b32 a3_sg_load_json(A3SgGraph *g, const char *text, usize len, char *error, usize error_cap) {
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(64));
    A3JsonError err;
    A3Json *root = a3_json_parse(text, len, &ar, &err);
    b32 ok = 0;
    if (!root) { a3_snprintf(error, error_cap, "line %d: %s", err.line, err.message); goto done; }
    if (!a3_streq(a3_json_get_string(root, "format", ""), "asm3d.shader")) { a3_snprintf(error, error_cap, "not an ASM3D shader file"); goto done; }
    if (a3_json_get_number(root, "version", 1) > 1) { a3_snprintf(error, error_cap, "made with a newer ASM3D (version %d)", (int)a3_json_get_number(root, "version", 1)); goto done; }
    a3_zero_struct(g);
    a3_strcpy(g->name, sizeof(g->name), a3_json_get_string(root, "name", "Shader"));
    A3_JSON_FOREACH(jn, a3_json_get(root, "nodes")) {
        i32 id = (i32)a3_json_get_number(jn, "id", -1);
        u32 type = a3_sg_type_from_name(a3_json_get_string(jn, "type", ""));
        if (id < 0 || id >= A3_SG_MAX_NODES || type >= A3_SGN_COUNT) { A3_WARN("shader", "skipping unknown node '%s'", a3_json_get_string(jn, "type", "?")); continue; }
        A3SgNode *n = &g->nodes[id];
        f32 pos[2] = { 0, 0 };
        a3_json_get_floats(a3_json_get(jn, "pos"), pos, 2);
        node_defaults(n, type, a3_v2(pos[0], pos[1]));
        u32 k = 0;
        A3_JSON_FOREACH(ji, a3_json_get(jn, "inputs")) {
            if (k >= A3_SG_MAX_PINS) break;
            n->src[k] = (i16)a3_json_get_number(ji, "node", -1);
            n->src_out[k] = (i8)a3_json_get_number(ji, "out", 0);
            a3_json_get_floats(a3_json_get(ji, "value"), n->in_value[k], 4);
            ++k;
        }
        a3_json_get_floats(a3_json_get(jn, "value"), n->value, 4);
        a3_strcpy(n->text, sizeof(n->text), a3_json_get_string(jn, "text", n->text));
    }
    /* validate links */
    for (i32 i = 0; i < A3_SG_MAX_NODES; ++i) {
        A3SgNode *n = &g->nodes[i];
        if (!n->used) continue;
        for (u32 k = 0; k < A3_SG_MAX_PINS; ++k) {
            i32 s = n->src[k];
            if (s < 0) continue;
            if (s >= A3_SG_MAX_NODES || !g->nodes[s].used || n->src_out[k] >= g_defs[g->nodes[s].type].out_count) n->src[k] = -1;
        }
    }
    if (a3_sg_output(g) < 0) node_defaults(&g->nodes[A3_SG_MAX_NODES - 1], A3_SGN_OUTPUT, a3_v2(420, 60));
    ok = 1;
done:
    a3_arena_release(&ar);
    return ok;
}

/* ======================================================================== */
/* Presets                                                                  */
/* ======================================================================== */

const char *const a3_sg_preset_names[A3_SG_PRESET_COUNT] = { "Basic Color", "Textured", "Glowing Pulse", "Checkerboard", "Rim Light", "Lava", "Water" };

void a3_sg_preset(A3SgGraph *g, u32 preset) {
    a3_sg_init(g);
    a3_strcpy(g->name, sizeof(g->name), preset < A3_SG_PRESET_COUNT ? a3_sg_preset_names[preset] : "Shader");
    i32 o = a3_sg_output(g);
    g->nodes[o].pos = a3_v2(560, 80);
    switch (preset) {
    default:
    case A3_SG_PRESET_BASIC: {
        i32 c = a3_sg_add(g, A3_SGN_PARAM_COLOR, a3_v2(200, 60));
        i32 r = a3_sg_add(g, A3_SGN_PARAM_NUMBER, a3_v2(200, 200));
        a3_strcpy(g->nodes[r].text, sizeof(g->nodes[r].text), "Roughness");
        a3_sg_connect(g, c, 0, o, 0);
        a3_sg_connect(g, r, 0, o, 2);
    } break;
    case A3_SG_PRESET_TEXTURED: {
        i32 t = a3_sg_add(g, A3_SGN_TEXTURE, a3_v2(60, 60));
        i32 c = a3_sg_add(g, A3_SGN_PARAM_COLOR, a3_v2(60, 240));
        g->nodes[c].value[0] = g->nodes[c].value[1] = g->nodes[c].value[2] = 1;
        i32 m = a3_sg_add(g, A3_SGN_MUL, a3_v2(320, 100));
        a3_sg_connect(g, t, 0, m, 0);
        a3_sg_connect(g, c, 0, m, 1);
        a3_sg_connect(g, m, 0, o, 0);
    } break;
    case A3_SG_PRESET_PULSE: {
        i32 col = a3_sg_add(g, A3_SGN_PARAM_COLOR, a3_v2(40, 40));
        g->nodes[col].value[0] = 0.2f; g->nodes[col].value[1] = 0.8f; g->nodes[col].value[2] = 1.0f;
        i32 t = a3_sg_add(g, A3_SGN_TIME, a3_v2(40, 220));
        i32 mul = a3_sg_add(g, A3_SGN_MUL, a3_v2(200, 220));
        g->nodes[mul].in_value[1][0] = 3.0f;
        i32 s = a3_sg_add(g, A3_SGN_SIN, a3_v2(360, 220));
        i32 b = a3_sg_add(g, A3_SGN_MIX, a3_v2(360, 360));
        g->nodes[b].in_value[0][0] = 0.3f; g->nodes[b].in_value[1][0] = 2.5f;
        i32 e = a3_sg_add(g, A3_SGN_MUL, a3_v2(400, 40));
        a3_sg_connect(g, t, 0, mul, 0);
        a3_sg_connect(g, mul, 0, s, 0);
        i32 half = a3_sg_add(g, A3_SGN_MUL, a3_v2(200, 360));
        g->nodes[half].in_value[1][0] = 0.5f;
        i32 add = a3_sg_add(g, A3_SGN_ADD, a3_v2(260, 460));
        g->nodes[add].in_value[1][0] = 0.5f;
        a3_sg_connect(g, s, 0, half, 0);
        a3_sg_connect(g, half, 0, add, 0);
        a3_sg_connect(g, add, 0, b, 2);
        a3_sg_connect(g, col, 0, e, 0);
        a3_sg_connect(g, b, 0, e, 1);
        a3_sg_connect(g, col, 0, o, 0);
        a3_sg_connect(g, e, 0, o, 3);
    } break;
    case A3_SG_PRESET_CHECKER: {
        i32 ch = a3_sg_add(g, A3_SGN_CHECKER, a3_v2(60, 120));
        i32 a = a3_sg_add(g, A3_SGN_COLOR, a3_v2(60, 280));
        g->nodes[a].value[0] = g->nodes[a].value[1] = g->nodes[a].value[2] = 0.95f;
        i32 b = a3_sg_add(g, A3_SGN_COLOR, a3_v2(60, 400));
        g->nodes[b].value[0] = g->nodes[b].value[1] = g->nodes[b].value[2] = 0.1f;
        i32 m = a3_sg_add(g, A3_SGN_MIX, a3_v2(320, 200));
        a3_sg_connect(g, a, 0, m, 0);
        a3_sg_connect(g, b, 0, m, 1);
        a3_sg_connect(g, ch, 0, m, 2);
        a3_sg_connect(g, m, 0, o, 0);
    } break;
    case A3_SG_PRESET_RIM: {
        i32 base = a3_sg_add(g, A3_SGN_OBJECT_COLOR, a3_v2(60, 40));
        i32 fr = a3_sg_add(g, A3_SGN_FRESNEL, a3_v2(60, 160));
        i32 rc = a3_sg_add(g, A3_SGN_PARAM_COLOR, a3_v2(60, 300));
        g->nodes[rc].text[0] = 0;
        a3_strcpy(g->nodes[rc].text, sizeof(g->nodes[rc].text), "Rim Color");
        g->nodes[rc].value[0] = 0.3f; g->nodes[rc].value[1] = 0.7f; g->nodes[rc].value[2] = 1.0f;
        i32 m = a3_sg_add(g, A3_SGN_MUL, a3_v2(320, 220));
        a3_sg_connect(g, fr, 0, m, 0);
        a3_sg_connect(g, rc, 0, m, 1);
        a3_sg_connect(g, base, 0, o, 0);
        a3_sg_connect(g, m, 0, o, 3);
    } break;
    case A3_SG_PRESET_LAVA: {
        i32 p = a3_sg_add(g, A3_SGN_LOCAL_POS, a3_v2(20, 60));
        i32 t = a3_sg_add(g, A3_SGN_TIME, a3_v2(20, 180));
        i32 tv = a3_sg_add(g, A3_SGN_COMBINE, a3_v2(160, 180));
        i32 tm = a3_sg_add(g, A3_SGN_MUL, a3_v2(160, 60));
        g->nodes[tm].in_value[1][0] = 0.2f;
        a3_sg_connect(g, t, 0, tm, 0);
        a3_sg_connect(g, tm, 0, tv, 1);
        i32 pp = a3_sg_add(g, A3_SGN_ADD, a3_v2(300, 60));
        a3_sg_connect(g, p, 0, pp, 0);
        a3_sg_connect(g, tv, 0, pp, 1);
        i32 n = a3_sg_add(g, A3_SGN_FBM, a3_v2(300, 200));
        a3_sg_connect(g, pp, 0, n, 0);
        i32 hot = a3_sg_add(g, A3_SGN_COLOR, a3_v2(20, 320));
        g->nodes[hot].value[0] = 1.0f; g->nodes[hot].value[1] = 0.45f; g->nodes[hot].value[2] = 0.05f;
        i32 cold = a3_sg_add(g, A3_SGN_COLOR, a3_v2(20, 440));
        g->nodes[cold].value[0] = 0.08f; g->nodes[cold].value[1] = 0.04f; g->nodes[cold].value[2] = 0.03f;
        i32 ss = a3_sg_add(g, A3_SGN_SMOOTHSTEP, a3_v2(300, 340));
        g->nodes[ss].in_value[1][0] = 0.45f; g->nodes[ss].in_value[2][0] = 0.7f;
        a3_sg_connect(g, n, 0, ss, 0);
        i32 m = a3_sg_add(g, A3_SGN_MIX, a3_v2(440, 300));
        a3_sg_connect(g, cold, 0, m, 0);
        a3_sg_connect(g, hot, 0, m, 1);
        a3_sg_connect(g, ss, 0, m, 2);
        i32 glow = a3_sg_add(g, A3_SGN_MUL, a3_v2(440, 440));
        g->nodes[glow].in_value[1][0] = 3.0f;
        a3_sg_connect(g, m, 0, glow, 0);
        a3_sg_connect(g, m, 0, o, 0);
        a3_sg_connect(g, glow, 0, o, 3);
        g->nodes[o].pos = a3_v2(640, 120);
    } break;
    case A3_SG_PRESET_WATER: {
        i32 uv = a3_sg_add(g, A3_SGN_WORLD_POS, a3_v2(20, 60));
        i32 n = a3_sg_add(g, A3_SGN_NOISE, a3_v2(200, 60));
        g->nodes[n].in_value[1][0] = 1.5f;
        a3_sg_connect(g, uv, 0, n, 0);
        i32 deep = a3_sg_add(g, A3_SGN_PARAM_COLOR, a3_v2(20, 220));
        a3_strcpy(g->nodes[deep].text, sizeof(g->nodes[deep].text), "Deep");
        g->nodes[deep].value[0] = 0.02f; g->nodes[deep].value[1] = 0.18f; g->nodes[deep].value[2] = 0.35f;
        i32 shallow = a3_sg_add(g, A3_SGN_COLOR, a3_v2(20, 360));
        g->nodes[shallow].value[0] = 0.1f; g->nodes[shallow].value[1] = 0.6f; g->nodes[shallow].value[2] = 0.7f;
        i32 m = a3_sg_add(g, A3_SGN_MIX, a3_v2(360, 200));
        a3_sg_connect(g, deep, 0, m, 0);
        a3_sg_connect(g, shallow, 0, m, 1);
        a3_sg_connect(g, n, 0, m, 2);
        i32 r = a3_sg_add(g, A3_SGN_NUMBER, a3_v2(360, 380));
        g->nodes[r].value[0] = 0.08f;
        a3_sg_connect(g, m, 0, o, 0);
        a3_sg_connect(g, r, 0, o, 2);
    } break;
    }
    /* preset layouts are sketched on a tight grid; spread them to fit node sizes */
    for (i32 i = 0; i < A3_SG_MAX_NODES; ++i)
        if (g->nodes[i].used) g->nodes[i].pos = a3_v2(g->nodes[i].pos.x * 1.45f, g->nodes[i].pos.y * 1.3f);
}
