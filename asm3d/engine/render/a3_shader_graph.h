/*
 * ASM3D - a3_shader_graph.h
 * Shader Maker graph: nodes connected by links, compiled to a GLSL surface
 * function `void a3_surface(inout A3Surface s)` for the PBR renderer.
 *
 * The graph is plain data (fixed-size arrays, no pointers) so the editor can
 * copy it for undo and it serializes to readable JSON (.a3shader). Code
 * generation records which output line belongs to which node, so a GLSL
 * compile error is reported on the node that caused it.
 */
#ifndef A3_SHADER_GRAPH_H
#define A3_SHADER_GRAPH_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"
#include "../core/a3_strbuf.h"

A3_EXTERN_C_BEGIN

#define A3_SG_MAX_NODES 96
#define A3_SG_MAX_PINS 6
#define A3_SG_MAX_PARAMS 4
#define A3_SG_TEXT 128

typedef enum A3SgType { A3_SG_FLOAT = 0, A3_SG_VEC2, A3_SG_VEC3, A3_SG_VEC4, A3_SG_GENERIC } A3SgType;

typedef enum A3SgValueKind {
    A3_SG_VALUE_NONE = 0,
    A3_SG_VALUE_FLOAT,       /* value[0] */
    A3_SG_VALUE_COLOR,       /* value[0..2] rgb (sRGB, converted to linear in code) */
    A3_SG_VALUE_VEC3,
    A3_SG_VALUE_TEXTURE,     /* text = texture path, param = slot */
    A3_SG_VALUE_CODE,        /* text = GLSL expression */
} A3SgValueKind;

typedef struct A3SgNodeDef {
    const char *name;
    const char *category;
    const char *help;
    u8 in_count, out_count;
    const char *in_names[A3_SG_MAX_PINS];
    u8 in_types[A3_SG_MAX_PINS];
    const char *in_default_expr[A3_SG_MAX_PINS];   /* used when unconnected (else a constant) */
    f32 in_defaults[A3_SG_MAX_PINS][4];
    const char *out_names[A3_SG_MAX_PINS];
    u8 out_types[A3_SG_MAX_PINS];
    const char *out_access[A3_SG_MAX_PINS];        /* suffix on the node variable, e.g. ".rgb" */
    u8 var_type;                                    /* type of the node variable */
    const char *expr;                               /* $0..$3 inputs, $V value, $P param slot, $T texture sampler */
    u8 value_kind;
    b32 is_param;                                   /* exposed material parameter */
} A3SgNodeDef;

typedef enum A3SgNodeType {
    A3_SGN_OUTPUT = 0,
    /* inputs */
    A3_SGN_UV, A3_SGN_WORLD_POS, A3_SGN_LOCAL_POS, A3_SGN_NORMAL, A3_SGN_VIEW_DIR, A3_SGN_TIME, A3_SGN_OBJECT_COLOR,
    /* constants & parameters */
    A3_SGN_NUMBER, A3_SGN_COLOR, A3_SGN_VECTOR, A3_SGN_PARAM_NUMBER, A3_SGN_PARAM_COLOR, A3_SGN_TEXTURE,
    /* math */
    A3_SGN_ADD, A3_SGN_SUB, A3_SGN_MUL, A3_SGN_DIV, A3_SGN_MIX, A3_SGN_POW, A3_SGN_MIN, A3_SGN_MAX, A3_SGN_ABS,
    A3_SGN_FRACT, A3_SGN_FLOOR, A3_SGN_SIN, A3_SGN_COS, A3_SGN_ONE_MINUS, A3_SGN_SATURATE, A3_SGN_SMOOTHSTEP, A3_SGN_STEP,
    /* vectors */
    A3_SGN_DOT, A3_SGN_NORMALIZE, A3_SGN_LENGTH, A3_SGN_SPLIT, A3_SGN_COMBINE,
    /* patterns */
    A3_SGN_NOISE, A3_SGN_FBM, A3_SGN_CHECKER, A3_SGN_FRESNEL, A3_SGN_PANNER, A3_SGN_STRIPES,
    /* advanced */
    A3_SGN_CODE,
    A3_SGN_COUNT
} A3SgNodeType;

typedef struct A3SgNode {
    b32 used;
    u16 type;                         /* A3SgNodeType */
    A3Vec2 pos;                       /* canvas position */
    i16 src[A3_SG_MAX_PINS];          /* source node per input (-1 = unconnected) */
    i8 src_out[A3_SG_MAX_PINS];
    f32 in_value[A3_SG_MAX_PINS][4];  /* constants for unconnected inputs */
    f32 value[4];
    char text[A3_SG_TEXT];            /* texture path / code / parameter name */
    i32 param;                        /* parameter or texture slot, assigned by the compiler */
} A3SgNode;

typedef struct A3SgGraph {
    A3SgNode nodes[A3_SG_MAX_NODES];
    char name[64];
} A3SgGraph;

typedef struct A3SgCompiled {
    b32 ok;                           /* graph was valid (GLSL may still fail to compile on the GPU) */
    char error[200];
    i32 error_node;                   /* graph-level error location, -1 if none */
    A3Vec4 params[A3_SG_MAX_PARAMS];
    char textures[2][A3_SG_TEXT];
    u32 param_count;
    i32 line_node[512];               /* 1-based line of the surface code -> node index (-1) */
    u32 line_count;
} A3SgCompiled;

const A3SgNodeDef *a3_sg_def(u32 type);
u32  a3_sg_type_from_name(const char *name);      /* A3_SGN_COUNT if unknown */

void a3_sg_init(A3SgGraph *g);                    /* just an Output node */
i32  a3_sg_add(A3SgGraph *g, u32 type, A3Vec2 pos); /* index or -1 when full */
void a3_sg_remove(A3SgGraph *g, i32 node);        /* the Output node cannot be removed */
i32  a3_sg_output(const A3SgGraph *g);
/* Connects src output -> dst input. Refuses cycles and invalid pins. */
b32  a3_sg_connect(A3SgGraph *g, i32 src, i32 src_out, i32 dst, i32 dst_in);
void a3_sg_disconnect(A3SgGraph *g, i32 dst, i32 dst_in);
b32  a3_sg_depends_on(const A3SgGraph *g, i32 node, i32 upstream);
A3SgType a3_sg_output_type(const A3SgGraph *g, i32 node, i32 out);

/* Generates the surface function. Returns false for graph errors (cycle, too
 * many parameters); `code` then contains a safe fallback surface. */
b32  a3_sg_compile(A3SgGraph *g, A3StrBuf *code, A3SgCompiled *out);
/* Node responsible for a 1-based line of the generated code (-1 if none). */
i32  a3_sg_node_for_line(const A3SgCompiled *c, i32 line);

/* JSON (.a3shader): graph + compiled surface + params, readable by the player. */
void a3_sg_save_json(const A3SgGraph *g, const char *surface_code, const A3SgCompiled *c, A3StrBuf *out);
b32  a3_sg_load_json(A3SgGraph *g, const char *text, usize len, char *error, usize error_cap);

/* Starting points offered in the editor. */
typedef enum A3SgPreset { A3_SG_PRESET_BASIC = 0, A3_SG_PRESET_TEXTURED, A3_SG_PRESET_PULSE, A3_SG_PRESET_CHECKER,
                          A3_SG_PRESET_RIM, A3_SG_PRESET_LAVA, A3_SG_PRESET_WATER, A3_SG_PRESET_COUNT } A3SgPreset;
extern const char *const a3_sg_preset_names[A3_SG_PRESET_COUNT];
void a3_sg_preset(A3SgGraph *g, u32 preset);

A3_EXTERN_C_END

#endif
