/*
 * ASM3D Editor - Shader Maker.
 *
 * Build materials by connecting nodes. The graph compiles to a GLSL surface
 * function (engine/render/a3_shader_graph.c) that plugs into the PBR
 * renderer; the preview updates live, parameters update without
 * recompiling, and GPU compile errors are shown on the node that caused them.
 * Materials are saved as .a3shader files and assigned through the Mesh
 * Renderer's Material field, so built games render them too.
 */
#include "editor.h"
#include "../../engine/render/a3_shader_graph.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/platform/a3_platform.h"

#define NODE_W 210.0f
#define HEADER_H 26.0f
#define SIDE_W 300.0f
#define UNDO_MAX 40
#define PREVIEW_PX 256

typedef struct EdShader {
    A3SgGraph g;
    A3SgGraph before;              /* state at the start of the current edit */
    b32 edit_pending;
    A3SgGraph *undo, *redo;        /* UNDO_MAX each */
    u32 undo_count, redo_count;
    char path[ED_PATH];            /* project relative, empty until saved */
    b32 dirty;
    b32 needs_compile;
    f64 change_time;
    /* compile state */
    u32 material;
    A3StrBuf code;
    A3SgCompiled compiled;
    b32 gpu_ok;
    char graph_error[200];
    i32 node_error_line[A3_SG_MAX_NODES]; /* index into errors, -1 */
    char errors[8][220];
    i32 error_nodes[8];
    u32 error_count;
    /* canvas */
    A3Vec2 pan;
    i32 selected;
    b32 linking;
    i32 link_node, link_out;
    b32 link_pending_add;          /* link released on empty canvas: add menu connects */
    i32 pending_node, pending_out;
    A3Vec2 add_pos;
    char add_filter[64];
    b32 open_add_menu;
    A3Vec2 pin_in[A3_SG_MAX_NODES][A3_SG_MAX_PINS];
    A3Vec2 pin_out[A3_SG_MAX_NODES][A3_SG_MAX_PINS];
    u8 pin_ok[A3_SG_MAX_NODES];
    f32 node_h[A3_SG_MAX_NODES];
    /* preview */
    A3World *pw;
    A3Entity pe;
    i32 preview_shape;
    b32 preview_rotate;
    f32 preview_angle;
    A3RhiTexture ptex;
    A3RhiTarget ptarget;
    A3UiId canvas_id;
    char open_list[32][ED_PATH];
    u32 open_count;
} EdShader;

static u32 colr(A3Ui *ui, A3UiColor c) { return a3_ui_theme(ui)->colors[c]; }

/* ======================================================================== */
/* State & undo                                                             */
/* ======================================================================== */

static void graph_changed(EdShader *s, b32 recompile) {
    s->edit_pending = 1;
    s->dirty = 1;
    if (recompile) { s->needs_compile = 1; s->change_time = a3_time_seconds(); }
}

static void push_stack(A3SgGraph *stack, u32 *count, const A3SgGraph *g) {
    if (*count == UNDO_MAX) { a3_memmove(stack, stack + 1, sizeof(A3SgGraph) * (UNDO_MAX - 1)); (*count)--; }
    stack[(*count)++] = *g;
}

static void undo(EdShader *s) {
    if (!s->undo_count) return;
    push_stack(s->redo, &s->redo_count, &s->g);
    s->g = s->undo[--s->undo_count];
    s->before = s->g;
    a3_zero(s->pin_ok, sizeof(s->pin_ok));
    s->needs_compile = 1;
    s->dirty = 1;
}

static void redo(EdShader *s) {
    if (!s->redo_count) return;
    push_stack(s->undo, &s->undo_count, &s->g);
    s->g = s->redo[--s->redo_count];
    s->before = s->g;
    a3_zero(s->pin_ok, sizeof(s->pin_ok));
    s->needs_compile = 1;
    s->dirty = 1;
}

static void reset_graph(EdShader *s) {
    s->undo_count = s->redo_count = 0;
    s->before = s->g;
    s->edit_pending = 0;
    s->selected = -1;
    s->linking = 0;
    s->pan = a3_v2(20, 20);
    a3_zero(s->pin_ok, sizeof(s->pin_ok));
    for (u32 i = 0; i < A3_SG_MAX_NODES; ++i) s->node_h[i] = 120;
    s->needs_compile = 1;
    s->change_time = 0;
}

void ed_shader_init(A3Editor *ed) {
    EdShader *s = A3_NEW(EdShader, A3_MEM_EDITOR);
    if (!s) { A3_ERROR("shader", "out of memory for the Shader Maker"); return; }
    s->undo = A3_NEW_ARRAY(A3SgGraph, UNDO_MAX, A3_MEM_EDITOR);
    s->redo = A3_NEW_ARRAY(A3SgGraph, UNDO_MAX, A3_MEM_EDITOR);
    if (!s->undo || !s->redo) { a3_free(s->undo); a3_free(s->redo); a3_free(s); A3_ERROR("shader", "out of memory for the Shader Maker"); return; }
    a3_strbuf_init(&s->code, A3_MEM_EDITOR);
    a3_sg_preset(&s->g, A3_SG_PRESET_BASIC);
    reset_graph(s);
    s->preview_rotate = 1;
    ed->shader = s;
}

void ed_shader_shutdown(A3Editor *ed) {
    EdShader *s = ed->shader;
    if (!s) return;
    A3Renderer *r = a3_engine_renderer(ed->engine);
    if (s->material) a3_renderer_material_destroy(r, s->material);
    if (s->pw) a3_world_destroy(s->pw);
    if (s->ptarget.id) a3_rhi_target_destroy(s->ptarget);
    if (s->ptex.id) a3_rhi_texture_destroy(s->ptex);
    a3_strbuf_free(&s->code);
    a3_free(s->undo);
    a3_free(s->redo);
    a3_free(s);
    ed->shader = 0;
}

void ed_shader_new(A3Editor *ed) {
    EdShader *s = ed->shader;
    if (!s) return;
    a3_sg_preset(&s->g, A3_SG_PRESET_BASIC);
    a3_strcpy(s->g.name, sizeof(s->g.name), "New Shader");
    s->path[0] = 0;
    s->dirty = 1;
    reset_graph(s);
}

void ed_shader_preset(A3Editor *ed, u32 preset) {
    EdShader *s = ed->shader;
    if (!s) return;
    a3_sg_preset(&s->g, preset);
    s->path[0] = 0;
    s->dirty = 1;
    reset_graph(s);
}

b32 ed_shader_open(A3Editor *ed, const char *rel) {
    EdShader *s = ed->shader;
    if (!s) return 0;
    char abs[ED_PATH];
    ed_project_path(ed, rel, abs, sizeof(abs));
    A3FileData fd;
    if (a3_file_read_all(abs, A3_MEM_TEMP, &fd) != A3_OK) { A3_ERROR("shader", "could not read %s", rel); return 0; }
    char err[160];
    A3SgGraph *tmp = A3_NEW(A3SgGraph, A3_MEM_TEMP);
    b32 ok = tmp && a3_sg_load_json(tmp, (const char *)fd.data, fd.size, err, sizeof(err));
    a3_free(fd.data);
    if (!ok) {
        a3_log_hint(A3_LOG_ERROR, "shader", "The file may have been edited by hand. It was not changed.", "cannot open %s: %s", rel, tmp ? err : "out of memory");
        a3_free(tmp);
        return 0;
    }
    s->g = *tmp;
    a3_free(tmp);
    a3_strcpy(s->path, sizeof(s->path), rel);
    s->dirty = 0;
    reset_graph(s);
    return 1;
}

/* ======================================================================== */
/* Compile                                                                  */
/* ======================================================================== */

static void compile(A3Editor *ed, EdShader *s) {
    A3Renderer *r = a3_engine_renderer(ed->engine);
    s->needs_compile = 0;
    s->error_count = 0;
    s->graph_error[0] = 0;
    for (u32 i = 0; i < A3_SG_MAX_NODES; ++i) s->node_error_line[i] = -1;
    b32 graph_ok = a3_sg_compile(&s->g, &s->code, &s->compiled);
    if (!graph_ok) {
        a3_strcpy(s->graph_error, sizeof(s->graph_error), s->compiled.error);
        if (s->compiled.error_node >= 0) s->node_error_line[s->compiled.error_node] = 0;
        a3_snprintf(s->errors[0], sizeof(s->errors[0]), "%s", s->compiled.error);
        s->error_nodes[0] = s->compiled.error_node;
        s->error_count = 1;
    }
    A3ShaderCompileResult res;
    a3_zero_struct(&res);
    b32 ok;
    if (!s->material) { s->material = a3_renderer_material_create(r, s->code.data, "shader_maker_preview", &res); ok = s->material != 0; }
    else ok = a3_renderer_material_update(r, s->material, s->code.data, &res);
    s->gpu_ok = ok && graph_ok;
    if (!ok) {
        for (u32 i = 0; i < res.error_count && s->error_count < A3_ARRAY_COUNT(s->errors); ++i) {
            i32 node = a3_sg_node_for_line(&s->compiled, res.errors[i].line);
            const A3SgNodeDef *d = node >= 0 ? a3_sg_def(s->g.nodes[node].type) : 0;
            a3_snprintf(s->errors[s->error_count], sizeof(s->errors[0]), "%s%s%s", d ? d->name : "", d ? ": " : "", res.errors[i].message);
            s->error_nodes[s->error_count] = node;
            if (node >= 0 && s->node_error_line[node] < 0) s->node_error_line[node] = (i32)s->error_count;
            s->error_count++;
        }
        if (!res.error_count && s->error_count < A3_ARRAY_COUNT(s->errors)) {
            a3_snprintf(s->errors[s->error_count], sizeof(s->errors[0]), "GPU compile failed: %.180s", res.raw_log);
            s->error_nodes[s->error_count++] = -1;
        }
        a3_log_hint(A3_LOG_WARN, "shader", "The node outlined in red caused the problem; the last working version stays active.",
                    "shader '%s' did not compile: %s", s->g.name, s->error_count ? s->errors[0] : "unknown error");
    }
    if (s->material) {
        A3Vec4 params[4];
        for (int i = 0; i < 4; ++i) params[i] = i < (int)s->compiled.param_count ? s->compiled.params[i] : a3_v4(1, 1, 1, 1);
        a3_renderer_material_set_params(r, s->material, params);
        for (u32 t = 0; t < 2; ++t) a3_renderer_material_set_texture(r, s->material, t, s->compiled.textures[t][0] ? a3_assets_texture(s->compiled.textures[t]) : 0);
    }
}

/* parameter edits update the material immediately, without recompiling */
static void push_param(A3Editor *ed, EdShader *s, i32 n) {
    A3SgNode *nd = &s->g.nodes[n];
    if (!s->material || nd->param < 0 || nd->param >= (i32)s->compiled.param_count) { s->needs_compile = 1; return; }
    A3Vec4 v = a3_v4(nd->value[0], nd->value[0], nd->value[0], 1);
    if (nd->type == A3_SGN_PARAM_COLOR) v = a3_v4(a3_srgb_to_linear(nd->value[0]), a3_srgb_to_linear(nd->value[1]), a3_srgb_to_linear(nd->value[2]), 1);
    s->compiled.params[nd->param] = v;
    A3Vec4 params[4];
    for (int i = 0; i < 4; ++i) params[i] = i < (int)s->compiled.param_count ? s->compiled.params[i] : a3_v4(1, 1, 1, 1);
    a3_renderer_material_set_params(a3_engine_renderer(ed->engine), s->material, params);
}

/* ======================================================================== */
/* Save / apply                                                             */
/* ======================================================================== */

static b32 save(A3Editor *ed, EdShader *s) {
    if (!ed->has_project) return 0;
    if (s->needs_compile) compile(ed, s);
    if (!s->path[0]) {
        char base[64];
        a3_strcpy(base, sizeof(base), s->g.name[0] ? s->g.name : "Shader");
        for (char *p = base; *p; ++p) if (!a3_is_alnum(*p) && *p != '-' && *p != '_') *p = '_';
        for (int i = 1; i < 100; ++i) {
            char abs[ED_PATH];
            if (i == 1) a3_snprintf(s->path, sizeof(s->path), "Assets/Shaders/%s.a3shader", base);
            else a3_snprintf(s->path, sizeof(s->path), "Assets/Shaders/%s_%d.a3shader", base, i);
            ed_project_path(ed, s->path, abs, sizeof(abs));
            if (!a3_file_exists(abs)) break;
        }
    }
    A3StrBuf json;
    a3_strbuf_init(&json, A3_MEM_EDITOR);
    a3_sg_save_json(&s->g, s->code.data, &s->compiled, &json);
    char abs[ED_PATH], dir[ED_PATH];
    ed_project_path(ed, s->path, abs, sizeof(abs));
    a3_path_dirname(abs, dir, sizeof(dir));
    a3_dir_create(dir);
    b32 ok = a3_file_write_atomic(abs, json.data, json.len) == A3_OK;
    a3_strbuf_free(&json);
    if (!ok) { a3_log_hint(A3_LOG_ERROR, "shader", "Check that the project folder is writable.", "could not save %s", s->path); return 0; }
    s->dirty = 0;
    /* objects using this material reload it on the next frame */
    a3_renderer_material_invalidate(a3_engine_renderer(ed->engine), s->path);
    a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_SUCCESS), "Saved %s", a3_path_filename(s->path));
    return 1;
}

static void apply_to_selection(A3Editor *ed, EdShader *s) {
    if (ed->mode != ED_EDIT) { a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_WARNING), "Stop the game first"); return; }
    A3Entity e = ed_selected(ed);
    A3CMeshRenderer *mr = a3_entity_valid(ed->world, e) ? (A3CMeshRenderer *)a3_component_get(ed->world, e, A3_T_MESH_RENDERER) : 0;
    if (!mr) { a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_WARNING), "Select an object with a Mesh Renderer first"); return; }
    if (!save(ed, s)) return;
    ed_undo_begin_frame(ed);
    mr = (A3CMeshRenderer *)a3_component_get(ed->world, e, A3_T_MESH_RENDERER);
    a3_strcpy(mr->material.path, sizeof(mr->material.path), s->path);
    mr->material.handle = 0;
    ed_undo_mark_changed(ed, "Apply Material");
    a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_SUCCESS), "Applied %s to %s", s->g.name, a3_entity_name(ed->world, e));
}

/* ======================================================================== */
/* Preview                                                                  */
/* ======================================================================== */

void ed_shader_render_preview(A3Editor *ed) {
    EdShader *s = ed->shader;
    if (!s || !ed->has_project || !a3_dock_is_visible(&ed->dock, "Shader Maker")) return;
    if (s->needs_compile && a3_time_seconds() - s->change_time > 0.15) compile(ed, s);
    if (!s->material) return;
    A3Renderer *r = a3_engine_renderer(ed->engine);
    if (!s->ptarget.id) {
        A3TextureDesc d;
        a3_zero_struct(&d);
        d.width = d.height = PREVIEW_PX;
        d.format = A3_TEX_RGBA8;
        d.wrap = A3_WRAP_CLAMP;
        d.debug_name = "shader_preview";
        s->ptex = a3_rhi_texture_create(&d);
        s->ptarget = a3_rhi_target_create(s->ptex, (A3RhiTexture){ 0 });
        if (!s->ptarget.id) return;
    }
    if (!s->pw) {
        s->pw = a3_world_create("ShaderPreview");
        A3CWorldSettings *ws = a3_world_settings(s->pw);
        ws->fog_density = 0;
        A3Entity sun = a3_entity_create(s->pw, "Sun");
        A3CTransform *st = (A3CTransform *)a3_component_add(s->pw, sun, A3_T_TRANSFORM);
        st->rotation = a3_quat_euler(-40 * A3_DEG2RAD, 40 * A3_DEG2RAD, 0);
        A3CLight *l = (A3CLight *)a3_component_add(s->pw, sun, A3_T_LIGHT);
        l->type = A3_LIGHT_DIRECTIONAL;
        l->intensity = 1.1f;
        l->cast_shadows = 0;
        s->pe = a3_entity_create(s->pw, "Preview");
        a3_component_add(s->pw, s->pe, A3_T_TRANSFORM);
        A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(s->pw, s->pe, A3_T_MESH_RENDERER);
        mr->base_color = a3_v4(0.8f, 0.8f, 0.8f, 1);
    }
    static const A3Primitive shapes[] = { A3_PRIM_SPHERE, A3_PRIM_CUBE, A3_PRIM_CYLINDER, A3_PRIM_PLANE };
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(s->pw, s->pe, A3_T_MESH_RENDERER);
    A3CTransform *t = a3_transform(s->pw, s->pe);
    if (!mr || !t) return;
    mr->primitive = shapes[a3_clampi(s->preview_shape, 0, 3)];
    if (s->preview_rotate) s->preview_angle += 0.01f;
    t->rotation = s->preview_shape == 3 ? a3_quat_euler(0.6f, s->preview_angle, 0) : a3_quat_euler(0.25f, s->preview_angle, 0);
    t->scale = s->preview_shape == 3 ? a3_v3(1.6f, 1, 1.6f) : a3_v3s(1.4f);
    a3_transform_system_update(s->pw);
    a3_renderer_set_entity_material(r, s->pw, s->pe, s->material);
    A3RenderView v = a3_render_view_look_at(a3_v3(0, 0.6f, 2.6f), a3_v3(0, 0, 0), 40, PREVIEW_PX, PREVIEW_PX);
    v.target = s->ptarget;
    v.draw_sky = 1;
    v.time = (f32)a3_time_seconds();
    v.exposure = 1;
    b32 wire = a3_renderer_settings(r)->wireframe;
    a3_renderer_settings(r)->wireframe = 0;
    a3_renderer_draw_world(r, s->pw, &v);
    a3_renderer_settings(r)->wireframe = wire;
}

/* ======================================================================== */
/* Canvas                                                                   */
/* ======================================================================== */

static u32 category_color(const char *cat) {
    if (a3_streq(cat, "Output")) return a3_rgb(64, 120, 220);
    if (a3_streq(cat, "Input")) return a3_rgb(58, 140, 92);
    if (a3_streq(cat, "Constant")) return a3_rgb(92, 104, 128);
    if (a3_streq(cat, "Parameter")) return a3_rgb(196, 120, 40);
    if (a3_streq(cat, "Math")) return a3_rgb(70, 96, 170);
    if (a3_streq(cat, "Vector")) return a3_rgb(126, 84, 170);
    if (a3_streq(cat, "Pattern")) return a3_rgb(40, 140, 150);
    return a3_rgb(170, 70, 70);
}

static u32 type_color(A3SgType t) {
    switch (t) {
    case A3_SG_FLOAT: return a3_rgb(170, 170, 180);
    case A3_SG_VEC2: return a3_rgb(120, 200, 120);
    case A3_SG_VEC3: return a3_rgb(230, 200, 90);
    case A3_SG_VEC4: return a3_rgb(230, 120, 200);
    default: return a3_rgb(150, 190, 240);
    }
}

static b32 edit_vec4(A3Ui *ui, const char *id, f32 *v, b32 color) {
    A3Vec4 c = a3_v4(v[0], v[1], v[2], 1);
    b32 ch = color ? a3_ui_color_edit(ui, id, &c, 0) : a3_ui_drag_float_n(ui, id, &c.x, 3, 0.01f, 0, 0, "%.2f");
    if (ch) { v[0] = c.x; v[1] = c.y; v[2] = c.z; }
    return ch;
}

/* Draws one node; returns true when its values changed. */
static void draw_node(A3Editor *ed, EdShader *s, A3Ui *ui, i32 n, A3Rect canvas) {
    A3SgNode *nd = &s->g.nodes[n];
    const A3SgNodeDef *d = a3_sg_def(nd->type);
    A3UiTheme *th = a3_ui_theme(ui);
    const A3InputState *in = a3_ui_input(ui);
    A3Rect nr = a3_rect(canvas.x + s->pan.x + nd->pos.x, canvas.y + s->pan.y + nd->pos.y, NODE_W, s->node_h[n]);
    if (nr.x > canvas.x + canvas.w || nr.y > canvas.y + canvas.h || nr.x + nr.w < canvas.x || nr.y + nr.h < canvas.y) { s->pin_ok[n] = 0; return; }
    a3_ui_push_id_int(ui, n);
    b32 sel = s->selected == n;
    b32 err = s->node_error_line[n] >= 0;
    a3_ui_shadow(ui, nr, 10, 8, a3_color_alpha(th->colors[A3_UIC_SHADOW], 0.8f));
    a3_ui_rect(ui, nr, th->colors[A3_UIC_PANEL], 8);
    A3Rect hr = a3_rect(nr.x, nr.y, nr.w, HEADER_H);
    a3_ui_rect_corners(ui, hr, category_color(d->category), 8, 1 | 2);
    char title[160];
    if (d->is_param && nd->text[0]) a3_snprintf(title, sizeof(title), "%s  (%s)", nd->text, d->name);
    else a3_snprintf(title, sizeof(title), "%s", d->name);
    a3_ui_text_in_rect(ui, A3_FONT_UI_BOLD, a3_rect(hr.x + 10, hr.y, hr.w - 16, hr.h), A3_ALIGN_LEFT, a3_rgb(245, 245, 250), title);
    if (sel || err) a3_ui_rect_outline(ui, nr, err ? th->colors[A3_UIC_ERROR] : th->colors[A3_UIC_ACCENT], 8, 2);
    /* header: select + drag */
    a3_ui_invisible_button(ui, "hdr", hr);
    if (a3_ui_item_hovered(ui) && in->mouse_pressed[A3_MOUSE_LEFT]) s->selected = n;
    if (a3_ui_item_active(ui) && (in->mouse_delta.x != 0 || in->mouse_delta.y != 0)) {
        nd->pos = a3_v2_add(nd->pos, in->mouse_delta);
        graph_changed(s, 0);
    }
    a3_ui_tooltip(ui, d->help);
    if (a3_ui_begin_context_menu(ui, "node_ctx")) {
        if (a3_ui_menu_item(ui, "Duplicate", "Ctrl+D", nd->type != A3_SGN_OUTPUT)) {
            i32 c = a3_sg_add(&s->g, nd->type, a3_v2_add(nd->pos, a3_v2(24, 24)));
            if (c >= 0) {
                A3SgNode keep = s->g.nodes[c];
                s->g.nodes[c] = *nd;
                s->g.nodes[c].pos = keep.pos;
                for (u32 k = 0; k < A3_SG_MAX_PINS; ++k) s->g.nodes[c].src[k] = -1;
                s->selected = c;
                graph_changed(s, 1);
            }
        }
        if (a3_ui_menu_item(ui, "Disconnect Inputs", 0, 1)) { for (u32 k = 0; k < A3_SG_MAX_PINS; ++k) nd->src[k] = -1; graph_changed(s, 1); }
        if (a3_ui_menu_item(ui, "Delete", "Del", nd->type != A3_SGN_OUTPUT)) { a3_sg_remove(&s->g, n); s->pin_ok[n] = 0; graph_changed(s, 1); }
        a3_ui_end_popup(ui);
    }
    if (!nd->used) { a3_ui_pop_id(ui); return; }
    /* body */
    A3Rect body = a3_rect(nr.x, nr.y + HEADER_H, nr.w, 2000);
    f32 used_h = 40;
    f32 saved_label = th->label_width;
    th->label_width = 0.42f;
    if (a3_ui_begin_panel(ui, "body", body, A3_PANEL_NO_SCROLL | A3_PANEL_NO_BACKGROUND)) {
        f32 top = a3_ui_cursor_pos(ui).y;
        if (err) {
            const char *msg = s->errors[s->node_error_line[n]];
            f32 wdt = a3_ui_content_width(ui);
            A3Rect er = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt, msg));
            a3_ui_text_wrapped(ui, A3_FONT_UI, er, th->colors[A3_UIC_ERROR], msg);
        }
        /* value row */
        switch (d->value_kind) {
        case A3_SG_VALUE_FLOAT:
            if (d->is_param) { a3_ui_property(ui, "Name", "Shown when tweaking the material"); if (a3_ui_input_text(ui, "pname", nd->text, sizeof(nd->text), 0, "Name")) graph_changed(s, 0); }
            a3_ui_property(ui, "Value", 0);
            if (a3_ui_drag_float(ui, "val", &nd->value[0], 0.01f, 0, 0, "%.3f")) { if (d->is_param) { push_param(ed, s, n); graph_changed(s, 0); } else graph_changed(s, 1); }
            break;
        case A3_SG_VALUE_COLOR:
            if (d->is_param) { a3_ui_property(ui, "Name", "Shown when tweaking the material"); if (a3_ui_input_text(ui, "pname", nd->text, sizeof(nd->text), 0, "Name")) graph_changed(s, 0); }
            a3_ui_property(ui, "Color", 0);
            if (edit_vec4(ui, "val", nd->value, 1)) { if (d->is_param) { push_param(ed, s, n); graph_changed(s, 0); } else graph_changed(s, 1); }
            break;
        case A3_SG_VALUE_VEC3:
            if (edit_vec4(ui, "val", nd->value, 0)) graph_changed(s, 1);
            break;
        case A3_SG_VALUE_TEXTURE: {
            A3Rect tr = a3_ui_next_rect(ui, 0, 64);
            u32 tex = nd->text[0] ? a3_assets_texture(nd->text) : 0;
            a3_ui_rect(ui, tr, th->colors[A3_UIC_BG], 4);
            if (tex) a3_ui_image(ui, a3_assets_texture_rhi(tex), a3_rect(tr.x + 2, tr.y + 2, 60, 60), a3_v2(0, 0), a3_v2(1, 1), 0xFFFFFFFFu);
            a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect(tr.x + (tex ? 68 : 6), tr.y + 4, tr.w - (tex ? 72 : 12), 56), th->colors[A3_UIC_TEXT_DIM],
                               nd->text[0] ? a3_path_filename(nd->text) : "Drag an image here from the Assets panel");
            u32 sz;
            const char *drop = (const char *)a3_ui_drop_target_rect(ui, tr, "asset", &sz);
            if (drop) {
                const char *ext = a3_path_extension(drop);
                if (a3_streq(ext, ".png") || a3_streq(ext, ".tga") || a3_streq(ext, ".bmp") || a3_streq(ext, ".ppm")) { a3_strcpy(nd->text, sizeof(nd->text), drop); graph_changed(s, 1); }
                else a3_ui_notify(ui, th->colors[A3_UIC_WARNING], "Only images can be used in a Texture node");
            }
        } break;
        case A3_SG_VALUE_CODE:
            if (a3_ui_input_text(ui, "code", nd->text, sizeof(nd->text), A3_INPUT_ENTER_RETURNS, "a * t")) graph_changed(s, 1);
            a3_ui_tooltip(ui, "GLSL expression producing a vec3. Inputs: a, b (vec3), t (float). Press Enter to apply.");
            break;
        default: break;
        }
        /* inputs */
        for (u32 k = 0; k < d->in_count; ++k) {
            a3_ui_push_id_int(ui, 100 + (i64)k);
            b32 linked = nd->src[k] >= 0;
            if (linked || d->in_default_expr[k]) {
                A3Rect rr = a3_ui_next_rect(ui, 0, th->row_height);
                char lbl[64];
                a3_snprintf(lbl, sizeof(lbl), "%s%s", d->in_names[k], !linked && d->in_default_expr[k] ? "  (auto)" : "");
                a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(rr.x + 4, rr.y, rr.w, rr.h), A3_ALIGN_LEFT, linked ? th->colors[A3_UIC_TEXT] : th->colors[A3_UIC_TEXT_DIM], lbl);
            } else if (nd->type == A3_SGN_OUTPUT) {
                A3Rect rr = a3_ui_next_rect(ui, 0, th->row_height);
                a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(rr.x + 4, rr.y, rr.w, rr.h), A3_ALIGN_LEFT, th->colors[A3_UIC_TEXT_DIM], d->in_names[k]);
            } else {
                a3_ui_property(ui, d->in_names[k], 0);
                u8 t = d->in_types[k];
                b32 ch = 0;
                if (t == A3_SG_VEC3) ch = edit_vec4(ui, "v", nd->in_value[k], a3_strstr(d->in_names[k], "Color") != 0);
                else if (t == A3_SG_VEC2) ch = a3_ui_drag_float_n(ui, "v", nd->in_value[k], 2, 0.01f, 0, 0, "%.2f");
                else ch = a3_ui_drag_float(ui, "v", &nd->in_value[k][0], 0.01f, 0, 0, "%.2f");
                if (ch) graph_changed(s, 1);
            }
            A3Rect lr = a3_ui_last_rect(ui);
            s->pin_in[n][k] = a3_v2(nr.x, lr.y + lr.h * 0.5f);
            a3_ui_pop_id(ui);
        }
        /* outputs */
        for (u32 k = 0; k < d->out_count; ++k) {
            A3Rect rr = a3_ui_next_rect(ui, 0, th->row_height);
            a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(rr.x, rr.y, rr.w - 4, rr.h), A3_ALIGN_RIGHT, th->colors[A3_UIC_TEXT], d->out_names[k]);
            s->pin_out[n][k] = a3_v2(nr.x + nr.w, rr.y + rr.h * 0.5f);
        }
        used_h = a3_ui_cursor_pos(ui).y - top;
    }
    a3_ui_end_panel(ui);
    th->label_width = saved_label;
    s->node_h[n] = HEADER_H + used_h + 14;
    s->pin_ok[n] = 1;
    /* pins */
    for (u32 k = 0; k < d->in_count; ++k) {
        A3Vec2 p = s->pin_in[n][k];
        A3SgType t = d->in_types[k] == A3_SG_GENERIC ? a3_sg_output_type(&s->g, n, 0) : (A3SgType)d->in_types[k];
        if (nd->type == A3_SGN_OUTPUT) t = (A3SgType)d->in_types[k];
        a3_ui_circle(ui, p, 6, th->colors[A3_UIC_PANEL]);
        if (nd->src[k] >= 0) a3_ui_circle(ui, p, 4.5f, type_color(t));
        else a3_ui_circle_outline(ui, p, 4.5f, type_color(t), 1.5f);
        a3_ui_push_id_int(ui, 200 + (i64)k);
        a3_ui_invisible_button(ui, "pin_in", a3_rect(p.x - 8, p.y - 8, 16, 16));
        if (a3_ui_item_hovered(ui) && in->mouse_pressed[A3_MOUSE_LEFT] && nd->src[k] >= 0) {
            /* pick up an existing link to move it */
            s->linking = 1;
            s->link_node = nd->src[k];
            s->link_out = nd->src_out[k];
            nd->src[k] = -1;
            graph_changed(s, 1);
        }
        a3_ui_pop_id(ui);
    }
    for (u32 k = 0; k < d->out_count; ++k) {
        A3Vec2 p = s->pin_out[n][k];
        a3_ui_circle(ui, p, 6, th->colors[A3_UIC_PANEL]);
        a3_ui_circle(ui, p, 4.5f, type_color(a3_sg_output_type(&s->g, n, (i32)k)));
        a3_ui_push_id_int(ui, 300 + (i64)k);
        a3_ui_invisible_button(ui, "pin_out", a3_rect(p.x - 8, p.y - 8, 16, 16));
        if (a3_ui_item_hovered(ui)) a3_ui_tooltip(ui, "Drag to an input to connect");
        if (a3_ui_item_hovered(ui) && in->mouse_pressed[A3_MOUSE_LEFT]) { s->linking = 1; s->link_node = n; s->link_out = (i32)k; }
        a3_ui_pop_id(ui);
    }
    a3_ui_pop_id(ui);
}

static void add_menu(A3Editor *ed, EdShader *s, A3Ui *ui) {
    A3_UNUSED(ed);
    if (s->open_add_menu) { s->add_filter[0] = 0; a3_ui_open_popup(ui, "sg_add"); s->open_add_menu = 0; }
    if (!a3_ui_begin_popup(ui, "sg_add", 240)) { s->link_pending_add = 0; return; }
    a3_ui_input_text(ui, "sg_add_q", s->add_filter, sizeof(s->add_filter), A3_INPUT_SEARCH | A3_INPUT_FOCUS, "Search nodes...");
    const char *cats[] = { "Input", "Constant", "Parameter", "Math", "Vector", "Pattern", "Advanced" };
    i32 added = -1;
    for (u32 c = 0; c < A3_ARRAY_COUNT(cats) && added < 0; ++c) {
        b32 header = 0;
        for (u32 t = 1; t < A3_SGN_COUNT; ++t) {
            const A3SgNodeDef *d = a3_sg_def(t);
            if (!a3_streq(d->category, cats[c])) continue;
            if (s->add_filter[0] && !a3_stristr(d->name, s->add_filter) && !a3_stristr(d->help, s->add_filter)) continue;
            if (!header) { a3_ui_label_colored(ui, category_color(cats[c]), "%s", cats[c]); header = 1; }
            a3_ui_push_id_int(ui, (i64)t);
            if (a3_ui_menu_item(ui, d->name, 0, 1)) added = a3_sg_add(&s->g, t, s->add_pos);
            a3_ui_tooltip(ui, d->help);
            a3_ui_pop_id(ui);
            if (added >= 0) break;
        }
    }
    if (added >= 0) {
        if (s->link_pending_add && a3_sg_def(s->g.nodes[added].type)->in_count > 0) a3_sg_connect(&s->g, s->pending_node, s->pending_out, added, 0);
        s->link_pending_add = 0;
        s->selected = added;
        s->node_h[added] = 120;
        s->pin_ok[added] = 0;
        graph_changed(s, 1);
        a3_ui_close_popup(ui);
    } else if (added < 0 && a3_ui_input(ui)->keys_pressed[A3_KEY_ESCAPE]) {
        a3_ui_close_popup(ui);
    }
    a3_ui_end_popup(ui);
}

static void canvas(A3Editor *ed, EdShader *s, A3Ui *ui, A3Rect cr) {
    A3UiTheme *th = a3_ui_theme(ui);
    const A3InputState *in = a3_ui_input(ui);
    a3_ui_push_clip(ui, cr);
    a3_ui_rect(ui, cr, a3_color_lerp(th->colors[A3_UIC_BG], a3_rgb(0, 0, 0), 0.15f), 0);
    /* grid */
    f32 step = 24;
    u32 gcol = a3_color_alpha(th->colors[A3_UIC_BORDER], 0.35f), gcol2 = a3_color_alpha(th->colors[A3_UIC_BORDER], 0.8f);
    f32 ox = a3_fmodf(s->pan.x, step), oy = a3_fmodf(s->pan.y, step);
    i32 ix = (i32)a3_floorf(-s->pan.x / step), iy = (i32)a3_floorf(-s->pan.y / step);
    for (f32 x = cr.x + ox - step; x < cr.x + cr.w; x += step, ++ix) a3_ui_rect(ui, a3_rect(x, cr.y, 1, cr.h), (ix % 5) ? gcol : gcol2, 0);
    for (f32 y = cr.y + oy - step; y < cr.y + cr.h; y += step, ++iy) a3_ui_rect(ui, a3_rect(cr.x, y, cr.w, 1), (iy % 5) ? gcol : gcol2, 0);
    /* canvas background interaction */
    a3_ui_invisible_button(ui, "sg_canvas", cr);
    A3UiId canvas_id = a3_ui_last_id(ui);
    s->canvas_id = canvas_id;
    b32 canvas_hover = a3_ui_item_hovered(ui);
    if (canvas_hover && in->mouse_pressed[A3_MOUSE_LEFT]) { s->selected = -1; a3_ui_claim_keyboard(ui, canvas_id); }
    if (a3_ui_item_active(ui) && !s->linking) s->pan = a3_v2_add(s->pan, in->mouse_delta);
    if (a3_rect_contains(cr, in->mouse_pos) && in->mouse[A3_MOUSE_MIDDLE]) s->pan = a3_v2_add(s->pan, in->mouse_delta);
    if (canvas_hover && in->mouse_pressed[A3_MOUSE_RIGHT]) {
        s->add_pos = a3_v2(in->mouse_pos.x - cr.x - s->pan.x, in->mouse_pos.y - cr.y - s->pan.y);
        s->open_add_menu = 1;
        s->link_pending_add = 0;
    }
    /* images dropped on the canvas become Texture nodes */
    u32 dsz;
    const char *drop = (const char *)a3_ui_drop_target_rect(ui, cr, "asset", &dsz);
    if (drop) {
        const char *ext = a3_path_extension(drop);
        if (a3_streq(ext, ".png") || a3_streq(ext, ".tga") || a3_streq(ext, ".bmp")) {
            i32 t = a3_sg_add(&s->g, A3_SGN_TEXTURE, a3_v2(in->mouse_pos.x - cr.x - s->pan.x, in->mouse_pos.y - cr.y - s->pan.y));
            if (t >= 0) { a3_strcpy(s->g.nodes[t].text, sizeof(s->g.nodes[t].text), drop); s->selected = t; graph_changed(s, 1); }
        }
    }
    /* links (pin positions from the previous frame) */
    for (i32 n = 0; n < A3_SG_MAX_NODES; ++n) {
        A3SgNode *nd = &s->g.nodes[n];
        if (!nd->used || !s->pin_ok[n]) continue;
        const A3SgNodeDef *d = a3_sg_def(nd->type);
        for (u32 k = 0; k < d->in_count; ++k) {
            i32 src = nd->src[k];
            if (src < 0 || !s->pin_ok[src]) continue;
            A3Vec2 a = s->pin_out[src][nd->src_out[k]], b = s->pin_in[n][k];
            f32 dx = a3_maxf(a3_absf(b.x - a.x) * 0.5f, 40);
            u32 c = type_color(a3_sg_output_type(&s->g, src, nd->src_out[k]));
            if (s->node_error_line[n] >= 0 && s->node_error_line[src] >= 0) c = th->colors[A3_UIC_ERROR];
            a3_ui_bezier(ui, a, a3_v2(a.x + dx, a.y), a3_v2(b.x - dx, b.y), b, a3_color_alpha(c, 0.9f), 2.5f);
        }
    }
    /* nodes */
    for (i32 n = 0; n < A3_SG_MAX_NODES; ++n) if (s->g.nodes[n].used) draw_node(ed, s, ui, n, cr);
    /* link being dragged */
    if (s->linking) {
        A3Vec2 a = s->pin_out[s->link_node][s->link_out], b = in->mouse_pos;
        f32 dx = a3_maxf(a3_absf(b.x - a.x) * 0.5f, 40);
        a3_ui_bezier(ui, a, a3_v2(a.x + dx, a.y), a3_v2(b.x - dx, b.y), b, th->colors[A3_UIC_ACCENT], 2.5f);
        if (!in->mouse[A3_MOUSE_LEFT]) {
            s->linking = 0;
            i32 best_n = -1, best_k = -1;
            f32 best = 14;
            for (i32 n = 0; n < A3_SG_MAX_NODES; ++n) {
                if (!s->g.nodes[n].used || !s->pin_ok[n]) continue;
                const A3SgNodeDef *d = a3_sg_def(s->g.nodes[n].type);
                for (u32 k = 0; k < d->in_count; ++k) {
                    f32 dist = a3_v2_len(a3_v2_sub(s->pin_in[n][k], b));
                    if (dist < best) { best = dist; best_n = n; best_k = (i32)k; }
                }
            }
            if (best_n >= 0) {
                if (a3_sg_connect(&s->g, s->link_node, s->link_out, best_n, best_k)) graph_changed(s, 1);
                else a3_ui_notify(ui, th->colors[A3_UIC_WARNING], "That connection would make a loop");
            } else if (a3_rect_contains(cr, b)) {
                /* released on empty canvas: pick a node to connect */
                s->add_pos = a3_v2(b.x - cr.x - s->pan.x, b.y - cr.y - s->pan.y);
                s->pending_node = s->link_node;
                s->pending_out = s->link_out;
                s->open_add_menu = 1;
                s->link_pending_add = 1;
            }
        }
    }
    add_menu(ed, s, ui);
    /* help for an empty graph */
    u32 count = 0;
    for (i32 n = 0; n < A3_SG_MAX_NODES; ++n) count += s->g.nodes[n].used;
    if (count <= 1) {
        A3Rect hr = a3_rect(cr.x + 20, cr.y + cr.h - 70, cr.w - 40, 50);
        a3_ui_text_wrapped(ui, A3_FONT_UI, hr, th->colors[A3_UIC_TEXT_DIM],
                           "Right-click to add nodes. Drag from a circle on the right side of a node to a circle on the left side of another node to connect them.");
    }
    /* keyboard while the canvas has focus */
    if (a3_ui_has_keyboard(ui, canvas_id)) {
        b32 ctrl = (in->mods & A3_MOD_CTRL) != 0;
        if ((in->keys_pressed[A3_KEY_DELETE] || in->keys_pressed[A3_KEY_BACKSPACE]) && s->selected >= 0 && s->g.nodes[s->selected].type != A3_SGN_OUTPUT) {
            a3_sg_remove(&s->g, s->selected);
            s->pin_ok[s->selected] = 0;
            s->selected = -1;
            graph_changed(s, 1);
        }
        if (ctrl && in->keys_pressed[A3_KEY_Z]) { if (in->mods & A3_MOD_SHIFT) redo(s); else undo(s); }
        if (ctrl && in->keys_pressed[A3_KEY_Y]) redo(s);
        if (ctrl && in->keys_pressed[A3_KEY_S]) save(ed, s);
        if (ctrl && in->keys_pressed[A3_KEY_D] && s->selected >= 0 && s->g.nodes[s->selected].type != A3_SGN_OUTPUT) {
            A3SgNode *src = &s->g.nodes[s->selected];
            i32 c = a3_sg_add(&s->g, src->type, a3_v2_add(src->pos, a3_v2(24, 24)));
            if (c >= 0) {
                A3Vec2 p = s->g.nodes[c].pos;
                s->g.nodes[c] = *src;
                s->g.nodes[c].pos = p;
                for (u32 k = 0; k < A3_SG_MAX_PINS; ++k) s->g.nodes[c].src[k] = -1;
                s->selected = c;
                s->pin_ok[c] = 0;
                graph_changed(s, 1);
            }
        }
        if (in->keys_pressed[A3_KEY_F]) {
            /* frame all nodes */
            f32 minx = 1e9f, miny = 1e9f;
            for (i32 n = 0; n < A3_SG_MAX_NODES; ++n) if (s->g.nodes[n].used) { minx = a3_minf(minx, s->g.nodes[n].pos.x); miny = a3_minf(miny, s->g.nodes[n].pos.y); }
            if (minx < 1e8f) s->pan = a3_v2(30 - minx, 30 - miny);
        }
    }
    a3_ui_pop_clip(ui);
}

/* ======================================================================== */
/* Sidebar & panel                                                          */
/* ======================================================================== */

static b32 list_shader(const char *dir, const A3DirEntry *e, void *user) {
    A3_UNUSED(dir);
    EdShader *s = (EdShader *)user;
    if (!e->is_dir && a3_str_ends_with(e->name, ".a3shader") && s->open_count < A3_ARRAY_COUNT(s->open_list))
        a3_snprintf(s->open_list[s->open_count++], ED_PATH, "Assets/Shaders/%s", e->name);
    return 1;
}

static void sidebar(A3Editor *ed, EdShader *s, A3Ui *ui, A3Rect sr) {
    A3UiTheme *th = a3_ui_theme(ui);
    if (!a3_ui_begin_panel(ui, "sg_side", sr, 0)) { a3_ui_end_panel(ui); return; }
    f32 wdt = a3_ui_content_width(ui);
    A3Rect pr = a3_ui_next_rect(ui, wdt, wdt);
    a3_ui_rect(ui, pr, th->colors[A3_UIC_BG], 8);
    if (s->ptex.id) a3_ui_image(ui, s->ptex, a3_rect_shrink(pr, 2), a3_v2(0, 1), a3_v2(1, 0), 0xFFFFFFFFu);
    else a3_ui_text_in_rect(ui, A3_FONT_UI, pr, A3_ALIGN_CENTER, th->colors[A3_UIC_TEXT_DIM], "Preview");
    static const char *const shapes[] = { "Sphere", "Cube", "Cylinder", "Plane" };
    a3_ui_combo(ui, "pshape", &s->preview_shape, shapes, 4);
    a3_ui_checkbox(ui, "Rotate preview", &s->preview_rotate);
    /* status */
    if (s->error_count) a3_ui_label_colored(ui, th->colors[A3_UIC_ERROR], "%s %u problem%s", "\xE2\x9C\x95", s->error_count, s->error_count == 1 ? "" : "s");
    else if (s->needs_compile) a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "Compiling...");
    else a3_ui_label_colored(ui, th->colors[A3_UIC_SUCCESS], "%s Compiled", "\xE2\x9C\x93");
    for (u32 i = 0; i < s->error_count; ++i) {
        A3Rect er = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt - 8, s->errors[i]) + 6);
        a3_ui_push_id_int(ui, (i64)i);
        if (a3_ui_invisible_button(ui, "err", er) && s->error_nodes[i] >= 0) s->selected = s->error_nodes[i];
        a3_ui_pop_id(ui);
        a3_ui_rect(ui, er, a3_color_alpha(th->colors[A3_UIC_ERROR], 0.12f), 4);
        a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect(er.x + 4, er.y + 3, er.w - 8, er.h), th->colors[A3_UIC_ERROR], s->errors[i]);
    }
    /* parameters */
    b32 any = 0;
    for (i32 n = 0; n < A3_SG_MAX_NODES; ++n) {
        A3SgNode *nd = &s->g.nodes[n];
        const A3SgNodeDef *d = a3_sg_def(nd->type);
        if (!nd->used || !d->is_param) continue;
        if (!any) { a3_ui_spacing(ui, 6); a3_ui_heading(ui, "Parameters"); any = 1; }
        a3_ui_push_id_int(ui, n);
        a3_ui_property(ui, nd->text[0] ? nd->text : d->name, "Changes apply instantly without recompiling");
        b32 ch = d->value_kind == A3_SG_VALUE_COLOR ? edit_vec4(ui, "p", nd->value, 1) : a3_ui_drag_float(ui, "p", &nd->value[0], 0.01f, 0, 0, "%.3f");
        if (ch) { push_param(ed, s, n); graph_changed(s, 0); }
        a3_ui_pop_id(ui);
    }
    /* selected node help */
    if (s->selected >= 0 && s->g.nodes[s->selected].used) {
        const A3SgNodeDef *d = a3_sg_def(s->g.nodes[s->selected].type);
        a3_ui_spacing(ui, 6);
        a3_ui_heading(ui, d->name);
        A3Rect hr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt, d->help));
        a3_ui_text_wrapped(ui, A3_FONT_UI, hr, th->colors[A3_UIC_TEXT_DIM], d->help);
    }
    a3_ui_spacing(ui, 6);
    if (a3_ui_collapsing_header(ui, "sg_code", "Generated GLSL", A3_ICON_PENCIL, 0)) {
        const char *code = a3_strbuf_cstr(&s->code);
        A3Rect cr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_MONO, wdt - 8, code) + 8);
        a3_ui_rect(ui, cr, th->colors[A3_UIC_BG], 4);
        a3_ui_text_wrapped(ui, A3_FONT_MONO, a3_rect_shrink(cr, 4), th->colors[A3_UIC_TEXT_DIM], code);
    }
    a3_ui_end_panel(ui);
}

void ed_shader_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    EdShader *s = ed->shader;
    if (!s || !ed->has_project) return;
    A3UiTheme *th = a3_ui_theme(ui);
    const A3InputState *in = a3_ui_input(ui);
    if (!s->edit_pending) s->before = s->g;
    /* toolbar */
    a3_ui_set_next_width(ui, 180);
    if (a3_ui_input_text(ui, "sg_name", s->g.name, sizeof(s->g.name), 0, "Shader name")) graph_changed(s, 0);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "New", 60, A3_BUTTON_SMALL)) a3_ui_open_popup(ui, "sg_new");
    if (a3_ui_begin_popup(ui, "sg_new", 200)) {
        a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "Start from");
        for (u32 p = 0; p < A3_SG_PRESET_COUNT; ++p) if (a3_ui_menu_item(ui, a3_sg_preset_names[p], 0, 1)) {
            if (s->dirty && s->path[0]) save(ed, s);
            a3_sg_preset(&s->g, p);
            s->path[0] = 0;
            s->dirty = 1;
            reset_graph(s);
        }
        a3_ui_end_popup(ui);
    }
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Open", 60, A3_BUTTON_SMALL)) {
        s->open_count = 0;
        char dir[ED_PATH];
        ed_project_path(ed, "Assets/Shaders", dir, sizeof(dir));
        a3_dir_list(dir, list_shader, s);
        a3_ui_open_popup(ui, "sg_open");
    }
    if (a3_ui_begin_popup(ui, "sg_open", 260)) {
        if (!s->open_count) a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "No shaders saved in Assets/Shaders yet.");
        for (u32 i = 0; i < s->open_count; ++i) {
            a3_ui_push_id_int(ui, (i64)i);
            if (a3_ui_menu_item(ui, a3_path_filename(s->open_list[i]), 0, 1)) ed_shader_open(ed, s->open_list[i]);
            a3_ui_pop_id(ui);
        }
        a3_ui_end_popup(ui);
    }
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, s->dirty ? "Save *" : "Save", 70, A3_BUTTON_SMALL | (s->dirty ? A3_BUTTON_PRIMARY : 0))) save(ed, s);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Apply to Selection", 150, A3_BUTTON_SMALL | (ed->mode == ED_EDIT ? 0 : A3_BUTTON_DISABLED))) apply_to_selection(ed, s);
    a3_ui_tooltip(ui, "Saves the shader and sets it as the Material of the selected object");
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_UNDO, "Undo (Ctrl+Z)", s->undo_count ? A3_BUTTON_SMALL : A3_BUTTON_SMALL | A3_BUTTON_DISABLED)) undo(s);
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_REDO, "Redo (Ctrl+Y)", s->redo_count ? A3_BUTTON_SMALL : A3_BUTTON_SMALL | A3_BUTTON_DISABLED)) redo(s);
    a3_ui_same_line(ui);
    a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "%s", s->path[0] ? s->path : "(not saved yet)");
    A3Vec2 cp = a3_ui_cursor_pos(ui);
    f32 side = a3_minf(SIDE_W, r.w * 0.4f);
    A3Rect area = a3_rect(r.x, cp.y, r.w, r.y + r.h - cp.y);
    A3Rect cr = a3_rect(area.x, area.y, area.w - side - 6, area.h);
    A3Rect sr = a3_rect(area.x + area.w - side, area.y, side, area.h);
    canvas(ed, s, ui, cr);
    sidebar(ed, s, ui, sr);
    /* commit the edit as one undo step when the mouse is released */
    /* (while typing in a field the step is committed once focus moves on) */
    if (s->edit_pending && !in->mouse[A3_MOUSE_LEFT] && (!a3_ui_wants_keyboard(ui) || a3_ui_has_keyboard(ui, s->canvas_id))) {
        push_stack(s->undo, &s->undo_count, &s->before);
        s->redo_count = 0;
        s->edit_pending = 0;
    }
}

/* ======================================================================== */
/* Self test (asm3d_editor --selftest)                                      */
/* ======================================================================== */

int ed_shader_selftest(A3Editor *ed) {
    EdShader *s = ed->shader;
    int failures = 0;
    if (!s) return 1;
    for (u32 p = 0; p < A3_SG_PRESET_COUNT; ++p) {
        a3_sg_preset(&s->g, p);
        reset_graph(s);
        compile(ed, s);
        if (!s->gpu_ok) { A3_ERROR("selftest", "preset '%s' failed on the GPU: %s", a3_sg_preset_names[p], s->error_count ? s->errors[0] : "?"); failures++; }
    }
    /* a broken custom expression is reported on its node */
    a3_sg_init(&s->g);
    reset_graph(s);
    i32 code = a3_sg_add(&s->g, A3_SGN_CODE, a3_v2(0, 0));
    a3_strcpy(s->g.nodes[code].text, sizeof(s->g.nodes[code].text), "a * not_a_variable");
    a3_sg_connect(&s->g, code, 0, a3_sg_output(&s->g), 0);
    a3_log_set_console(0);
    compile(ed, s);
    a3_log_set_console(1);
    if (s->gpu_ok || s->node_error_line[code] < 0) { A3_ERROR("selftest", "broken custom code was not mapped to its node"); failures++; }
    /* save + apply to an object, then render: the material file must load */
    a3_sg_preset(&s->g, A3_SG_PRESET_LAVA);
    a3_strcpy(s->g.name, sizeof(s->g.name), "SelfTestLava");
    s->path[0] = 0;
    reset_graph(s);
    A3Entity target = a3_entity_find_by_name(ed->world, "Cube");
    if (!a3_entity_valid(ed->world, target)) target = a3_entity_find_by_name(ed->world, "Crate");
    ed->selected = a3_entity_guid(ed->world, target);
    apply_to_selection(ed, s);
    ed_undo_end_frame(ed);
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(ed->world, ed_selected(ed), A3_T_MESH_RENDERER);
    if (!mr || !a3_streq(mr->material.path, s->path)) { A3_ERROR("selftest", "Apply to Selection did not set the material"); failures++; }
    u32 errs = a3_log_error_count();
    ed->vp_w = ed->vp_h = 64;
    ed_viewport_render(ed);
    if (a3_log_error_count() != errs) { A3_ERROR("selftest", "rendering with the saved material logged errors"); failures++; }
    return failures;
}
