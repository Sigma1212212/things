/*
 * ASM3D - test_shader_graph.c : Shader Maker graph -> GLSL surface code
 * (GPU compilation of every preset is covered by asm3d_editor --selftest)
 */
#include "a3_test.h"
#include "../engine/render/a3_shader_graph.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_strbuf.h"

static i32 line_of_text(const char *code, const char *needle) {
    const char *p = a3_strstr(code, needle);
    if (!p) return -1;
    i32 line = 1;
    for (const char *c = code; c < p; ++c) line += *c == '\n';
    return line;
}

A3_TEST(shadergraph_basic_codegen) {
    A3SgGraph g;
    A3StrBuf code;
    A3SgCompiled c;
    a3_strbuf_init(&code, A3_MEM_TEMP);
    a3_sg_init(&g);
    A3_CHECK(a3_sg_output(&g) == 0);
    A3_CHECK(a3_sg_compile(&g, &code, &c));
    A3_CHECK(a3_strstr(code.data, "void a3_surface(inout A3Surface s)") != 0);
    A3_CHECK(a3_strstr(code.data, "s.albedo") == 0); /* unconnected outputs keep Mesh Renderer values */
    a3_sg_preset(&g, A3_SG_PRESET_BASIC);
    A3_CHECK(a3_sg_compile(&g, &code, &c));
    A3_CHECK_EQ_INT(c.param_count, 2);
    A3_CHECK(a3_strstr(code.data, "u_material[0].rgb") != 0);
    A3_CHECK(a3_strstr(code.data, "s.roughness = ") != 0);
    /* error line mapping: the albedo assignment belongs to the Output node */
    i32 line = line_of_text(code.data, "s.albedo = ");
    A3_CHECK(line > 0);
    A3_CHECK_EQ_INT(a3_sg_node_for_line(&c, line), a3_sg_output(&g));
    /* and the parameter line belongs to the parameter node */
    i32 pline = line_of_text(code.data, "u_material[0].rgb");
    i32 pn = a3_sg_node_for_line(&c, pline);
    A3_CHECK(pn >= 0 && g.nodes[pn].type == A3_SGN_PARAM_COLOR);
    a3_strbuf_free(&code);
}

A3_TEST(shadergraph_types_and_cycles) {
    A3SgGraph g;
    A3StrBuf code;
    A3SgCompiled c;
    a3_strbuf_init(&code, A3_MEM_TEMP);
    a3_sg_init(&g);
    i32 o = a3_sg_output(&g);
    i32 col = a3_sg_add(&g, A3_SGN_COLOR, a3_v2(0, 0));
    i32 num = a3_sg_add(&g, A3_SGN_NUMBER, a3_v2(0, 100));
    i32 mul = a3_sg_add(&g, A3_SGN_MUL, a3_v2(200, 0));
    A3_CHECK(a3_sg_connect(&g, col, 0, mul, 0));
    A3_CHECK(a3_sg_connect(&g, num, 0, mul, 1));
    A3_CHECK_EQ_INT(a3_sg_output_type(&g, mul, 0), A3_SG_VEC3);
    A3_CHECK(a3_sg_connect(&g, mul, 0, o, 0));
    A3_CHECK(a3_sg_compile(&g, &code, &c));
    A3_CHECK(a3_strstr(code.data, "vec3 n3 = (n1 * vec3(n2));") != 0);
    /* loops are refused */
    i32 add = a3_sg_add(&g, A3_SGN_ADD, a3_v2(0, 200));
    A3_CHECK(a3_sg_connect(&g, mul, 0, add, 0));
    A3_CHECK(!a3_sg_connect(&g, add, 0, mul, 0));
    A3_CHECK(!a3_sg_connect(&g, add, 0, add, 1));
    /* invalid pins */
    A3_CHECK(!a3_sg_connect(&g, col, 3, add, 0));
    A3_CHECK(!a3_sg_connect(&g, col, 0, add, 5));
    /* removing a node clears links to it; the Output node stays */
    a3_sg_remove(&g, mul);
    A3_CHECK_EQ_INT(g.nodes[o].src[0], -1);
    a3_sg_remove(&g, o);
    A3_CHECK(a3_sg_output(&g) == o);
    a3_strbuf_free(&code);
}

A3_TEST(shadergraph_limits_fallback) {
    A3SgGraph g;
    A3StrBuf code;
    A3SgCompiled c;
    a3_strbuf_init(&code, A3_MEM_TEMP);
    a3_sg_init(&g);
    i32 o = a3_sg_output(&g);
    /* chain 5 parameters through adds: one too many */
    i32 prev = a3_sg_add(&g, A3_SGN_PARAM_NUMBER, a3_v2(0, 0));
    for (int i = 0; i < 4; ++i) {
        i32 p = a3_sg_add(&g, A3_SGN_PARAM_NUMBER, a3_v2(0, 0));
        i32 a = a3_sg_add(&g, A3_SGN_ADD, a3_v2(0, 0));
        a3_sg_connect(&g, prev, 0, a, 0);
        a3_sg_connect(&g, p, 0, a, 1);
        prev = a;
    }
    a3_sg_connect(&g, prev, 0, o, 1);
    A3_CHECK(!a3_sg_compile(&g, &code, &c));
    A3_CHECK(c.error_node >= 0 && g.nodes[c.error_node].type == A3_SGN_PARAM_NUMBER);
    A3_CHECK(a3_strstr(code.data, "vec3(1.0, 0.0, 1.0)") != 0); /* visible fallback */
    a3_strbuf_free(&code);
}

A3_TEST(shadergraph_every_node_and_preset) {
    A3SgGraph g;
    A3StrBuf code;
    A3SgCompiled c;
    a3_strbuf_init(&code, A3_MEM_TEMP);
    for (u32 t = 1; t < A3_SGN_COUNT; ++t) {
        a3_sg_init(&g);
        i32 o = a3_sg_output(&g);
        i32 n = a3_sg_add(&g, t, a3_v2(0, 0));
        A3_CHECK_MSG(n >= 0, "add node %u", t);
        for (u32 out = 0; out < a3_sg_def(t)->out_count; ++out) {
            A3_CHECK(a3_sg_connect(&g, n, (i32)out, o, (i32)(out % 2 ? 1 : 0)));
            b32 ok = a3_sg_compile(&g, &code, &c);
            A3_CHECK_MSG(ok, "node '%s' output %u failed: %s", a3_sg_def(t)->name, out, c.error);
            A3_CHECK(a3_strstr(code.data, "$") == 0); /* every template placeholder expanded */
        }
    }
    for (u32 p = 0; p < A3_SG_PRESET_COUNT; ++p) {
        a3_sg_preset(&g, p);
        A3_CHECK_MSG(a3_sg_compile(&g, &code, &c), "preset %s: %s", a3_sg_preset_names[p], c.error);
    }
    a3_strbuf_free(&code);
}

A3_TEST(shadergraph_json_roundtrip) {
    A3SgGraph g, h;
    A3StrBuf code, code2, json;
    A3SgCompiled c, c2;
    a3_strbuf_init(&code, A3_MEM_TEMP);
    a3_strbuf_init(&code2, A3_MEM_TEMP);
    a3_strbuf_init(&json, A3_MEM_TEMP);
    a3_sg_preset(&g, A3_SG_PRESET_LAVA);
    A3_CHECK(a3_sg_compile(&g, &code, &c));
    a3_sg_save_json(&g, code.data, &c, &json);
    char err[128] = { 0 };
    A3_CHECK_MSG(a3_sg_load_json(&h, json.data, json.len, err, sizeof(err)), "load: %s", err);
    A3_CHECK(a3_sg_compile(&h, &code2, &c2));
    A3_CHECK_STR(code.data, code2.data);
    A3_CHECK_STR(g.name, h.name);
    /* damaged / foreign files are rejected with a message */
    A3_CHECK(!a3_sg_load_json(&h, "{\"format\":\"other\"}", 18, err, sizeof(err)));
    A3_CHECK(!a3_sg_load_json(&h, "{ nope", 6, err, sizeof(err)));
    A3_CHECK(err[0] != 0);
    a3_strbuf_free(&code);
    a3_strbuf_free(&code2);
    a3_strbuf_free(&json);
}
