/*
 * ASM3D Editor - Edit Mode: polygon modeling in the viewport.
 *
 * Tab on a selected object opens its mesh (an .obj model, or a primitive that
 * is converted to Assets/Models/<Name>.obj). Keys follow common modeler
 * conventions: 1/2/3 vertex/edge/face, G/R/S move/rotate/scale (X/Y/Z to
 * constrain), E extrude, I inset, Ctrl+R loop cut, Ctrl+2 subdivide, X
 * delete, F fill, M merge, Shift+D duplicate, A / Alt+A select all / none,
 * L linked, double-click edge loop, drag for box select, Alt+Z x-ray, Tab done.
 * The Modeling panel has a button for everything. Mesh math lives in
 * engine/modeling (x86-64 assembly for transforms, normals and picking).
 */
#include "editor.h"
#include "../../engine/modeling/a3_emesh.h"
#include "../../engine/modeling/a3_emesh_ops.h"
#include "../../engine/modeling/a3_modeling_kernels.h"
#include "../../engine/ui/a3_font.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/render/a3_mesh.h"
#include "../../engine/platform/a3_platform.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"

#define UNDO_MAX 64

typedef enum Tool { TOOL_NONE = 0, TOOL_GRAB, TOOL_ROTATE, TOOL_SCALE, TOOL_INSET, TOOL_LOOPCUT } Tool;

typedef struct EdModel {
    b32 active;
    u64 guid;
    char path[ED_PATH];          /* project relative .obj */
    A3EMesh mesh;
    A3EMesh snap;                /* mesh when the current tool started */
    A3EMesh *undo;               /* [UNDO_MAX] */
    u32 undo_count, undo_pos;    /* states [0..count), current = pos */
    b32 changed;
    /* tools */
    Tool tool;
    i32 axis;                    /* -1 free, 0..2 world axis, 3 custom */
    A3Vec3 custom_axis;
    A3Vec2 start_mouse;
    A3Vec3 pivot;                /* world */
    A3Vec2 pivot_screen;
    u32 cuts;
    u32 hover_edge;
    /* selection */
    b32 box_pending, box_active;
    A3Vec2 box_start;
    b32 xray;
    /* panel inputs */
    f32 extrude_dist, inset_amount, inset_depth, merge_dist, add_size;
    i32 add_prim;
    A3Vec3 move_vec;
    /* frame caches */
    A3Mat4 world, inv_world, vp;
    A3Vec2 *sp;
    u8 *vis;
    u32 sp_cap;
    u8 *front;
    u32 front_cap;
    char status[160];
} EdModel;

static EdModel *model(A3Editor *ed) {
    if (!ed->model) {
        ed->model = A3_NEW(EdModel, A3_MEM_EDITOR);
        EdModel *md = ed->model;
        a3_emesh_init(&md->mesh);
        a3_emesh_init(&md->snap);
        md->undo = A3_NEW_ARRAY(A3EMesh, UNDO_MAX, A3_MEM_EDITOR);
        md->extrude_dist = 0.5f;
        md->inset_amount = 0.2f;
        md->merge_dist = 0.001f;
        md->add_size = 1.0f;
        md->cuts = 1;
        md->hover_edge = A3_ENONE;
    }
    return ed->model;
}

b32 ed_model_active(A3Editor *ed) { return ed->model && ed->model->active; }

static A3Entity target(A3Editor *ed) {
    EdModel *md = model(ed);
    return a3_entity_find_by_guid(ed->world, md->guid);
}

/* ---- undo (whole-mesh snapshots; the mesh is small next to the scene) ---- */

static void undo_reset(EdModel *md) {
    for (u32 i = 0; i < md->undo_count; ++i) a3_emesh_free(&md->undo[i]);
    md->undo_count = md->undo_pos = 0;
}

static void undo_store(EdModel *md) {
    /* drop redo states, keep at most UNDO_MAX */
    for (u32 i = md->undo_pos + 1; i < md->undo_count; ++i) a3_emesh_free(&md->undo[i]);
    md->undo_count = md->undo_count ? md->undo_pos + 1 : 0;
    if (md->undo_count == UNDO_MAX) {
        a3_emesh_free(&md->undo[0]);
        a3_memmove(&md->undo[0], &md->undo[1], sizeof(A3EMesh) * (UNDO_MAX - 1));
        md->undo_count--;
    }
    A3EMesh *s = &md->undo[md->undo_count];
    a3_emesh_init(s);
    a3_emesh_copy(s, &md->mesh);
    md->undo_pos = md->undo_count++;
}

/* ---- render mesh + file ---- */

static void refresh(A3Editor *ed) {
    EdModel *md = model(ed);
    A3MeshData d;
    if (a3_emesh_to_mesh_data(&md->mesh, &d)) a3_assets_mesh_from_data(md->path, &d);
    A3Entity e = target(ed);
    A3CMeshRenderer *mr = a3_entity_valid(ed->world, e) ? (A3CMeshRenderer *)a3_component_get(ed->world, e, A3_T_MESH_RENDERER) : 0;
    if (mr) mr->mesh.handle = 0;
}

/* call after every finished operation */
static void committed(A3Editor *ed) {
    EdModel *md = model(ed);
    md->changed = 1;
    undo_store(md);
    refresh(ed);
}

static b32 save_file(A3Editor *ed) {
    EdModel *md = model(ed);
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_EDITOR);
    char name[64];
    a3_path_stem(md->path, name, sizeof(name));
    a3_emesh_save_obj(&md->mesh, &sb, name);
    char abs[ED_PATH], dir[ED_PATH];
    ed_project_path(ed, md->path, abs, sizeof(abs));
    a3_path_dirname(abs, dir, sizeof(dir));
    a3_dir_create(dir);
    b32 ok = a3_file_write_atomic(abs, a3_strbuf_cstr(&sb), sb.len) == A3_OK;
    a3_strbuf_free(&sb);
    if (!ok) a3_log_hint(A3_LOG_ERROR, "model", "Check that the project folder is writable.", "could not save %s", md->path);
    return ok;
}

/* ======================================================================== */
/* Enter / exit                                                             */
/* ======================================================================== */

static A3EPrimitive prim_for(i32 p) {
    switch (p) {
    case A3_PRIM_SPHERE: return A3_EPRIM_SPHERE;
    case A3_PRIM_PLANE: return A3_EPRIM_PLANE;
    case A3_PRIM_CYLINDER: case A3_PRIM_CAPSULE: return A3_EPRIM_CYLINDER;
    case A3_PRIM_CONE: return A3_EPRIM_CONE;
    default: return A3_EPRIM_CUBE;
    }
}

b32 ed_model_enter(A3Editor *ed) {
    if (ed->mode != ED_EDIT) { a3_ui_notify(ed->ui, 0, "Stop the game to edit meshes"); return 0; }
    A3Entity e = ed_selected(ed);
    if (!a3_entity_valid(ed->world, e)) { a3_ui_notify(ed->ui, a3_ui_theme(ed->ui)->colors[A3_UIC_WARNING], "Select an object to edit its mesh"); return 0; }
    EdModel *md = model(ed);
    if (md->active) return 1;
    ed_undo_begin_frame(ed);
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(ed->world, e, A3_T_MESH_RENDERER);
    if (!mr) { mr = (A3CMeshRenderer *)a3_component_add(ed->world, e, A3_T_MESH_RENDERER); mr->primitive = A3_PRIM_CUBE; }
    a3_emesh_clear(&md->mesh);
    md->mesh.select_mode = A3_ESEL_VERTEX;
    b32 from_file = mr->mesh.path[0] && a3_str_ends_with(mr->mesh.path, ".obj");
    if (from_file) {
        char abs[ED_PATH], err[160];
        ed_project_path(ed, mr->mesh.path, abs, sizeof(abs));
        A3FileData fd;
        b32 ok = a3_file_read_all(abs, A3_MEM_TEMP, &fd) == A3_OK;
        if (ok) { ok = a3_emesh_load_obj(&md->mesh, (const char *)fd.data, fd.size, err, sizeof(err)); a3_free(fd.data); }
        if (!ok) { a3_ui_notify(ed->ui, a3_ui_theme(ed->ui)->colors[A3_UIC_ERROR], "Cannot open %s for editing", mr->mesh.path); return 0; }
        a3_strcpy(md->path, sizeof(md->path), mr->mesh.path);
    } else {
        a3_emesh_make(&md->mesh, prim_for(mr->primitive), 1.0f, 24, 12);
        char base[64], abs[ED_PATH];
        a3_strcpy(base, sizeof(base), a3_entity_name(ed->world, e));
        for (char *c = base; *c; ++c) if (!a3_is_ident(*c) && *c != '-') *c = '_';
        for (int i = 0; i < 1000; ++i) {
            if (i == 0) a3_snprintf(md->path, sizeof(md->path), "Assets/Models/%s.obj", base);
            else a3_snprintf(md->path, sizeof(md->path), "Assets/Models/%s%d.obj", base, i + 1);
            ed_project_path(ed, md->path, abs, sizeof(abs));
            if (!a3_file_exists(abs)) break;
        }
        if (!save_file(ed)) return 0;
        a3_strcpy(mr->mesh.path, sizeof(mr->mesh.path), md->path);
        mr->primitive = A3_PRIM_NONE;
        ed_undo_mark_changed(ed, "Convert to Editable Mesh");
    }
    a3_emesh_select_all(&md->mesh, 1);
    md->guid = a3_entity_guid(ed->world, e);
    md->active = 1;
    md->changed = 0;
    md->tool = TOOL_NONE;
    undo_reset(md);
    undo_store(md);
    refresh(ed);
    a3_dock_show(&ed->dock, "Modeling");
    a3_dock_show(&ed->dock, "Viewport");
    A3_INFO("model", "editing %s (%u vertices, %u faces)", md->path, a3_emesh_vertex_count(&md->mesh), a3_emesh_face_count(&md->mesh));
    return 1;
}

void ed_model_exit(A3Editor *ed, b32 save) {
    if (!ed_model_active(ed)) return;
    EdModel *md = model(ed);
    if (md->tool) { a3_emesh_copy(&md->mesh, &md->snap); md->tool = TOOL_NONE; }
    if (save && md->changed) {
        if (save_file(ed)) a3_ui_notify(ed->ui, a3_ui_theme(ed->ui)->colors[A3_UIC_SUCCESS], "Saved %s", md->path);
        ed->dirty = 1;
    }
    if (!save && md->changed) {
        /* reload the file version */
        a3_assets_reload(md->path);
    }
    md->active = 0;
    undo_reset(md);
}

b32 ed_model_undo(A3Editor *ed) {
    EdModel *md = model(ed);
    if (!md->active || md->tool || md->undo_pos == 0) return 0;
    md->undo_pos--;
    a3_emesh_copy(&md->mesh, &md->undo[md->undo_pos]);
    md->changed = 1;
    refresh(ed);
    return 1;
}

b32 ed_model_redo(A3Editor *ed) {
    EdModel *md = model(ed);
    if (!md->active || md->tool || md->undo_pos + 1 >= md->undo_count) return 0;
    md->undo_pos++;
    a3_emesh_copy(&md->mesh, &md->undo[md->undo_pos]);
    md->changed = 1;
    refresh(ed);
    return 1;
}

/* Runs a named operation (same names as `asm3d_cli mesh ops`). */
b32 ed_model_run(A3Editor *ed, const char *op) {
    EdModel *md = model(ed);
    if (!md->active) return 0;
    A3EMeshOpError err;
    if (!a3_emesh_run_op(&md->mesh, op, &err)) {
        a3_emesh_copy(&md->mesh, &md->undo[md->undo_pos]);
        a3_ui_notify(ed->ui, a3_ui_theme(ed->ui)->colors[A3_UIC_WARNING], "%s", err.message);
        return 0;
    }
    committed(ed);
    return 1;
}

const A3EMesh *ed_model_mesh(A3Editor *ed) { return ed_model_active(ed) ? &model(ed)->mesh : 0; }

void ed_model_shutdown(A3Editor *ed) {
    if (!ed->model) return;
    EdModel *md = ed->model;
    undo_reset(md);
    a3_free(md->undo);
    a3_emesh_free(&md->mesh);
    a3_emesh_free(&md->snap);
    a3_free(md->sp);
    a3_free(md->vis);
    a3_free(md->front);
    a3_free(md);
    ed->model = 0;
}

/* ======================================================================== */
/* Viewport                                                                 */
/* ======================================================================== */

static A3Vec3 cam_forward(A3Editor *ed) { return a3_quat_rotate(a3_quat_euler(ed->cam_pitch, ed->cam_yaw, 0), a3_v3(0, 0, -1)); }
static A3Vec3 cam_right(A3Editor *ed) { return a3_quat_rotate(a3_quat_euler(ed->cam_pitch, ed->cam_yaw, 0), a3_v3(1, 0, 0)); }
static A3Vec3 cam_up(A3Editor *ed) { return a3_quat_rotate(a3_quat_euler(ed->cam_pitch, ed->cam_yaw, 0), a3_v3(0, 1, 0)); }

static b32 to_screen(A3Editor *ed, A3Vec3 world, A3Vec2 *out) {
    EdModel *md = model(ed);
    A3Vec4 c = a3_mat4_mul_v4(&md->vp, a3_v4_from3(world, 1));
    if (c.w <= 1e-4f) return 0;
    *out = a3_v2(ed->vp_rect.x + (c.x / c.w * 0.5f + 0.5f) * ed->vp_rect.w, ed->vp_rect.y + (0.5f - c.y / c.w * 0.5f) * ed->vp_rect.h);
    return 1;
}

static void project(A3Editor *ed) {
    EdModel *md = model(ed);
    A3EMesh *m = &md->mesh;
    u32 n = m->pos.count;
    if (md->sp_cap < n + 1) {
        md->sp_cap = n + 64;
        md->sp = (A3Vec2 *)a3_realloc(md->sp, sizeof(A3Vec2) * md->sp_cap, A3_MEM_EDITOR);
        md->vis = (u8 *)a3_realloc(md->vis, md->sp_cap, A3_MEM_EDITOR);
    }
    u32 nf = a3_emesh_face_count(m);
    if (md->front_cap < nf + 1) { md->front_cap = nf + 64; md->front = (u8 *)a3_realloc(md->front, md->front_cap, A3_MEM_EDITOR); }
    for (u32 v = 0; v < n; ++v) md->vis[v] = md->xray ? 1 : 0;
    for (u32 f = 0; f < nf; ++f) {
        A3Vec3 c = a3_mat4_mul_point(&md->world, a3_emesh_face_center(m, f));
        A3Vec3 nw = a3_mat4_mul_dir(&md->world, a3_emesh_face_normal(m, f));
        md->front[f] = a3_v3_dot(nw, a3_v3_sub(ed->cam_pos, c)) > 0;
        if (md->front[f]) {
            u32 k;
            const u32 *fv = a3_emesh_face(m, f, &k);
            for (u32 i = 0; i < k; ++i) md->vis[fv[i]] = 1;
        }
    }
    for (u32 v = 0; v < n; ++v) {
        A3Vec3 w = a3_mat4_mul_point(&md->world, a3_emesh_vertex(m, v));
        if (!to_screen(ed, w, &md->sp[v])) md->vis[v] = 0;
    }
}

static f32 seg_dist(A3Vec2 p, A3Vec2 a, A3Vec2 b) {
    A3Vec2 ab = a3_v2(b.x - a.x, b.y - a.y), ap = a3_v2(p.x - a.x, p.y - a.y);
    f32 l = ab.x * ab.x + ab.y * ab.y;
    f32 t = l > 1e-6f ? a3_clampf((ap.x * ab.x + ap.y * ab.y) / l, 0, 1) : 0;
    f32 dx = a.x + ab.x * t - p.x, dy = a.y + ab.y * t - p.y;
    return a3_sqrtf(dx * dx + dy * dy);
}

static u32 nearest_edge(A3Editor *ed, A3Vec2 p, f32 max_px) {
    EdModel *md = model(ed);
    A3EMesh *m = &md->mesh;
    a3_emesh_edges(m);
    u32 best = A3_ENONE;
    f32 bd = max_px;
    for (u32 e = 0; e < m->edges.count; ++e) {
        const A3EEdge *ed2 = &m->edges.data[e];
        if (!md->vis[ed2->a] || !md->vis[ed2->b]) continue;
        f32 d = seg_dist(p, md->sp[ed2->a], md->sp[ed2->b]);
        if (d < bd) { bd = d; best = e; }
    }
    return best;
}

static u32 nearest_vertex(A3Editor *ed, A3Vec2 p, f32 max_px) {
    EdModel *md = model(ed);
    u32 best = A3_ENONE;
    f32 bd = max_px * max_px;
    for (u32 v = 0; v < md->mesh.pos.count; ++v) {
        if (!md->vis[v]) continue;
        f32 dx = md->sp[v].x - p.x, dy = md->sp[v].y - p.y, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = v; }
    }
    return best;
}

static u32 pick_face(A3Editor *ed, A3Vec3 ray_o, A3Vec3 ray_d) {
    EdModel *md = model(ed);
    A3Vec3 o = a3_mat4_mul_point(&md->inv_world, ray_o), d = a3_mat4_mul_dir(&md->inv_world, ray_d);
    f32 t;
    return a3_emesh_raycast(&md->mesh, o, d, &t);   /* assembly ray kernel */
}

static A3Vec3 ray_dir(A3Editor *ed, A3Vec2 mp) {
    EdModel *md = model(ed);
    A3Mat4 inv;
    if (!a3_mat4_inverse(&md->vp, &inv)) return cam_forward(ed);
    f32 x = (mp.x - ed->vp_rect.x) / ed->vp_rect.w * 2 - 1, y = 1 - (mp.y - ed->vp_rect.y) / ed->vp_rect.h * 2;
    A3Vec3 n = a3_mat4_mul_point_project(&inv, a3_v3(x, y, a3_rhi_depth_zero_to_one() ? 0.0f : -1.0f));
    A3Vec3 f = a3_mat4_mul_point_project(&inv, a3_v3(x, y, 1.0f));
    return a3_v3_norm(a3_v3_sub(f, n));
}

/* ---- tools ---- */

static b32 start_tool(A3Editor *ed, Tool t, A3Vec2 mouse) {
    EdModel *md = model(ed);
    a3_emesh_sync_selection(&md->mesh);
    if (t != TOOL_LOOPCUT && !a3_emesh_selected_count(&md->mesh)) { a3_ui_notify(ed->ui, 0, "Select something first"); return 0; }
    a3_emesh_copy(&md->snap, &md->mesh);
    md->tool = t;
    md->axis = -1;
    md->start_mouse = mouse;
    md->pivot = a3_mat4_mul_point(&md->world, a3_emesh_selection_center(&md->mesh));
    if (!to_screen(ed, md->pivot, &md->pivot_screen)) md->pivot_screen = mouse;
    if (t == TOOL_INSET && a3_v2_len(a3_v2_sub(mouse, md->pivot_screen)) < 60) md->start_mouse = a3_v2_add(md->pivot_screen, a3_v2(120, 0));
    return 1;
}

static void end_tool(A3Editor *ed, b32 confirm) {
    EdModel *md = model(ed);
    if (!md->tool) return;
    if (confirm && md->tool != TOOL_LOOPCUT) committed(ed);
    else if (!confirm) { a3_emesh_copy(&md->mesh, &md->snap); refresh(ed); }
    md->tool = TOOL_NONE;
}

static A3Mat4 world_to_local(EdModel *md, const A3Mat4 *world_op) {
    A3Mat4 a = a3_mat4_mul(world_op, &md->world);
    return a3_mat4_mul(&md->inv_world, &a);
}

static A3Vec3 axis_vec(EdModel *md) {
    if (md->axis == 3) return md->custom_axis;
    return a3_v3(md->axis == 0 ? 1.0f : 0.0f, md->axis == 1 ? 1.0f : 0.0f, md->axis == 2 ? 1.0f : 0.0f);
}

static void update_tool(A3Editor *ed, A3Vec2 mouse, b32 ctrl) {
    EdModel *md = model(ed);
    a3_emesh_copy(&md->mesh, &md->snap);
    A3Vec2 d = a3_v2_sub(mouse, md->start_mouse);
    if (md->tool == TOOL_GRAB) {
        A3Vec3 delta;
        if (md->axis >= 0) {
            A3Vec3 ax = axis_vec(md);
            A3Vec2 s2;
            if (!to_screen(ed, a3_v3_add(md->pivot, ax), &s2)) return;
            s2 = a3_v2_sub(s2, md->pivot_screen);
            f32 l2 = s2.x * s2.x + s2.y * s2.y;
            f32 amount = l2 > 1e-6f ? (d.x * s2.x + d.y * s2.y) / l2 : 0;
            if (ctrl) amount = a3_roundf(amount * 10.0f) / 10.0f;
            delta = a3_v3_scale(ax, amount);
        } else {
            f32 depth = a3_maxf(a3_v3_dot(a3_v3_sub(md->pivot, ed->cam_pos), cam_forward(ed)), 0.05f);
            f32 k = 2.0f * depth * a3_tanf(30.0f * A3_DEG2RAD) / a3_maxf(ed->vp_rect.h, 1);
            delta = a3_v3_add(a3_v3_scale(cam_right(ed), d.x * k), a3_v3_scale(cam_up(ed), -d.y * k));
            if (ctrl) delta = a3_v3(a3_roundf(delta.x * 10) / 10, a3_roundf(delta.y * 10) / 10, a3_roundf(delta.z * 10) / 10);
        }
        A3Mat4 t = a3_mat4_translation(delta);
        A3Mat4 l = world_to_local(md, &t);
        a3_emesh_transform_selected(&md->mesh, &l);
        a3_snprintf(md->status, sizeof(md->status), "Move  %.3f, %.3f, %.3f", (f64)delta.x, (f64)delta.y, (f64)delta.z);
    } else if (md->tool == TOOL_ROTATE) {
        A3Vec2 a0 = a3_v2_sub(md->start_mouse, md->pivot_screen), a1 = a3_v2_sub(mouse, md->pivot_screen);
        f32 ang = a3_atan2f(-a1.y, a1.x) - a3_atan2f(-a0.y, a0.x);
        if (ctrl) ang = a3_roundf(ang / (15 * A3_DEG2RAD)) * (15 * A3_DEG2RAD);
        A3Vec3 ax = md->axis >= 0 ? axis_vec(md) : a3_v3_neg(cam_forward(ed));
        if (md->axis >= 0 && a3_v3_dot(ax, cam_forward(ed)) > 0) ang = -ang;   /* turn the way the mouse turns */
        A3Mat4 r = a3_mat4_from_quat(a3_quat_axis_angle(ax, ang));
        A3Mat4 to = a3_mat4_translation(a3_v3_neg(md->pivot)), back = a3_mat4_translation(md->pivot);
        A3Mat4 t1 = a3_mat4_mul(&r, &to), t2 = a3_mat4_mul(&back, &t1);
        A3Mat4 l = world_to_local(md, &t2);
        a3_emesh_transform_selected(&md->mesh, &l);
        a3_snprintf(md->status, sizeof(md->status), "Rotate  %.1f degrees", (f64)(ang / A3_DEG2RAD));
    } else if (md->tool == TOOL_SCALE) {
        f32 d0 = a3_maxf(a3_v2_len(a3_v2_sub(md->start_mouse, md->pivot_screen)), 1.0f);
        f32 f = a3_v2_len(a3_v2_sub(mouse, md->pivot_screen)) / d0;
        if (ctrl) f = a3_roundf(f * 10) / 10;
        A3Vec3 s = md->axis >= 0 && md->axis < 3 ? a3_v3(md->axis == 0 ? f : 1, md->axis == 1 ? f : 1, md->axis == 2 ? f : 1) : a3_v3s(f);
        A3Mat4 sc = a3_mat4_scale(s);
        A3Mat4 to = a3_mat4_translation(a3_v3_neg(md->pivot)), back = a3_mat4_translation(md->pivot);
        A3Mat4 t1 = a3_mat4_mul(&sc, &to), t2 = a3_mat4_mul(&back, &t1);
        A3Mat4 l = world_to_local(md, &t2);
        a3_emesh_transform_selected(&md->mesh, &l);
        a3_snprintf(md->status, sizeof(md->status), "Scale  %.3f", (f64)f);
    } else if (md->tool == TOOL_INSET) {
        f32 d0 = a3_maxf(a3_v2_len(a3_v2_sub(md->start_mouse, md->pivot_screen)), 1.0f);
        f32 amount = a3_clampf(1.0f - a3_v2_len(a3_v2_sub(mouse, md->pivot_screen)) / d0, 0.0f, 0.95f);
        a3_emesh_inset(&md->mesh, amount, 0);
        a3_snprintf(md->status, sizeof(md->status), "Inset  %.2f", (f64)amount);
    }
    refresh(ed);
}

/* ---- one-shot operations shared by keys and panel ---- */

static void op(A3Editor *ed, const char *spec) { ed_model_run(ed, spec); }

static void op_delete(A3Editor *ed) {
    EdModel *md = model(ed);
    op(ed, md->mesh.select_mode == A3_ESEL_FACE ? "delete-faces" : "delete-verts");
}

static void op_extrude_interactive(A3Editor *ed, A3Vec2 mouse) {
    EdModel *md = model(ed);
    a3_emesh_sync_selection(&md->mesh);
    b32 any = 0;
    for (u32 f = 0; f < md->mesh.fsel.count; ++f) any |= md->mesh.fsel.data[f];
    if (!any) { a3_ui_notify(ed->ui, 0, "Extrude works on faces: select faces (press 3) first"); return; }
    A3Vec3 nrm = a3_v3(0, 0, 0);
    for (u32 f = 0; f < md->mesh.fsel.count; ++f) if (md->mesh.fsel.data[f]) nrm = a3_v3_add(nrm, a3_emesh_face_normal(&md->mesh, f));
    a3_emesh_extrude(&md->mesh, 0.0f);
    committed(ed);
    if (!start_tool(ed, TOOL_GRAB, mouse)) return;
    md->axis = 3;
    md->custom_axis = a3_v3_norm(a3_mat4_mul_dir(&md->world, nrm));
    if (a3_v3_len_sq(md->custom_axis) == 0) md->axis = -1;
}

/* ---- drawing ---- */

static void draw(A3Editor *ed, A3Ui *ui) {
    EdModel *md = model(ed);
    A3EMesh *m = &md->mesh;
    a3_emesh_edges(m);
    a3_emesh_sync_selection(m);
    const u32 sel_col = a3_rgb(255, 160, 50), sel_fill = a3_rgba(255, 150, 40, 60), wire = a3_rgba(210, 214, 222, 150), wire_back = a3_rgba(210, 214, 222, 45);
    const u32 vert_col = a3_rgba(20, 22, 28, 255), vert_rim = a3_rgba(230, 232, 238, 255);
    a3_ui_push_clip(ui, ed->vp_rect);
    /* selected faces */
    u32 *tris, *face;
    u32 nt = a3_emesh_triangulate(m, &tris, &face);
    for (u32 t = 0; t < nt; ++t) {
        u32 f = face[t];
        if (!m->fsel.data[f] || (!md->front[f] && !md->xray)) continue;
        u32 a = tris[t * 3], b = tris[t * 3 + 1], c = tris[t * 3 + 2];
        if (!md->vis[a] && !md->xray) continue;
        a3_ui_triangle(ui, md->sp[a], md->sp[b], md->sp[c], sel_fill);
    }
    a3_free(tris);
    a3_free(face);
    /* edges */
    for (u32 e = 0; e < m->edges.count; ++e) {
        const A3EEdge *ee = &m->edges.data[e];
        b32 front = (ee->f0 != A3_ENONE && md->front[ee->f0]) || (ee->f1 != A3_ENONE && md->front[ee->f1]);
        if (!front && !md->xray) continue;
        b32 sel = m->vsel.data[ee->a] && m->vsel.data[ee->b];
        if (m->select_mode == A3_ESEL_FACE) sel = (ee->f0 != A3_ENONE && m->fsel.data[ee->f0]) || (ee->f1 != A3_ENONE && m->fsel.data[ee->f1]);
        u32 col = sel ? sel_col : front ? wire : wire_back;
        if (e == md->hover_edge) col = a3_rgb(255, 255, 255);
        a3_ui_line(ui, md->sp[ee->a], md->sp[ee->b], col, sel || e == md->hover_edge ? 2.0f : 1.2f);
    }
    /* vertices / face dots */
    if (m->select_mode == A3_ESEL_VERTEX) {
        for (u32 v = 0; v < m->pos.count; ++v) {
            if (!md->vis[v]) continue;
            if (m->vsel.data[v]) a3_ui_circle(ui, md->sp[v], 3.5f, sel_col);
            else { a3_ui_circle(ui, md->sp[v], 3.0f, vert_rim); a3_ui_circle(ui, md->sp[v], 2.0f, vert_col); }
        }
    } else if (m->select_mode == A3_ESEL_FACE) {
        for (u32 f = 0; f < a3_emesh_face_count(m); ++f) {
            if (!md->front[f] && !md->xray) continue;
            A3Vec2 s;
            if (!to_screen(ed, a3_mat4_mul_point(&md->world, a3_emesh_face_center(m, f)), &s)) continue;
            a3_ui_rect(ui, a3_rect(s.x - 2.5f, s.y - 2.5f, 5, 5), m->fsel.data[f] ? sel_col : vert_rim, 1);
        }
    }
    /* box select */
    if (md->box_active) {
        const A3InputState *in = a3_ui_input(ui);
        A3Vec2 a = md->box_start, b = in->mouse_pos;
        A3Rect r = a3_rect(a3_minf(a.x, b.x), a3_minf(a.y, b.y), a3_absf(b.x - a.x), a3_absf(b.y - a.y));
        a3_ui_rect(ui, r, a3_rgba(255, 255, 255, 25), 0);
        a3_ui_rect_outline(ui, r, a3_rgba(255, 255, 255, 180), 0, 1);
    }
    /* tool axis */
    if (md->tool == TOOL_GRAB || md->tool == TOOL_ROTATE || md->tool == TOOL_SCALE) {
        if (md->axis >= 0) {
            A3Vec3 ax = axis_vec(md);
            A3Vec2 a, b;
            if (to_screen(ed, a3_v3_sub(md->pivot, a3_v3_scale(ax, 1000)), &a) && to_screen(ed, a3_v3_add(md->pivot, a3_v3_scale(ax, 1000)), &b)) {
                u32 c = md->axis == 0 ? a3_rgb(230, 70, 70) : md->axis == 1 ? a3_rgb(100, 210, 80) : md->axis == 2 ? a3_rgb(80, 140, 240) : a3_rgb(240, 220, 90);
                a3_ui_line(ui, a, b, c, 1.5f);
            }
        }
        const A3InputState *in = a3_ui_input(ui);
        a3_ui_line(ui, md->pivot_screen, in->mouse_pos, a3_rgba(255, 255, 255, 90), 1);
    }
    /* status */
    A3EMeshStats st;
    a3_emesh_stats(m, &st);
    static const char *const modes[] = { "Vertex", "Edge", "Face" };
    char buf[256];
    a3_snprintf(buf, sizeof(buf), "EDIT MODE  %s   Verts %u/%u   Edges %u/%u   Faces %u/%u   Tris %u%s",
                modes[m->select_mode], st.selected_vertices, st.vertices, st.selected_edges, st.edges, st.selected_faces, st.faces, st.triangles, md->xray ? "   X-Ray" : "");
    A3Rect sr = a3_rect(ed->vp_rect.x + 8, ed->vp_rect.y + 46, a3_font_text_width(A3_FONT_UI, buf, -1) + 20, 24);
    a3_ui_rect(ui, sr, a3_rgba(20, 22, 28, 200), 6);
    a3_ui_text(ui, A3_FONT_UI, a3_v2(sr.x + 10, sr.y + 5), sel_col, buf);
    const char *hint = md->tool == TOOL_NONE ? "G move  R rotate  S scale  E extrude  I inset  Ctrl+R loop cut  X delete  F fill  1/2/3 mode  A all  Tab done"
                     : md->tool == TOOL_LOOPCUT ? "Click an edge to cut its quad ring   Mouse wheel: number of cuts   Right click: cancel"
                     : "Move the mouse   X / Y / Z: constrain   Ctrl: snap   Click or Enter: confirm   Right click or Esc: cancel";
    char hb[256];
    if (md->tool == TOOL_LOOPCUT) a3_snprintf(hb, sizeof(hb), "Loop cut: %u cut%s   %s", md->cuts, md->cuts == 1 ? "" : "s", hint);
    else if (md->tool) a3_snprintf(hb, sizeof(hb), "%s   %s", md->status, hint);
    else a3_strcpy(hb, sizeof(hb), hint);
    f32 hw = a3_font_text_width(A3_FONT_UI, hb, -1) + 20;
    A3Rect hr = a3_rect(ed->vp_rect.x + (ed->vp_rect.w - hw) * 0.5f, ed->vp_rect.y + ed->vp_rect.h - 34, hw, 24);
    a3_ui_rect(ui, hr, a3_rgba(20, 22, 28, 200), 6);
    a3_ui_text(ui, A3_FONT_UI, a3_v2(hr.x + 10, hr.y + 5), a3_rgb(225, 228, 235), hb);
    a3_ui_pop_clip(ui);
}

/* ---- selection clicks ---- */

static void click_select(A3Editor *ed, A3Vec2 p, b32 shift, b32 alt) {
    EdModel *md = model(ed);
    A3EMesh *m = &md->mesh;
    if (m->select_mode == A3_ESEL_VERTEX && !alt) {
        u32 v = nearest_vertex(ed, p, 14);
        if (!shift) a3_emesh_select_all(m, 0);
        if (v != A3_ENONE) a3_emesh_select_vertex(m, v, shift ? !m->vsel.data[v] : 1);
    } else if (m->select_mode == A3_ESEL_EDGE || alt) {
        u32 e = nearest_edge(ed, p, 10);
        if (!shift) a3_emesh_select_all(m, 0);
        if (e != A3_ENONE) {
            if (alt) a3_emesh_select_edge_loop(m, e);
            else {
                const A3EEdge *ee = &m->edges.data[e];
                a3_emesh_select_edge(m, e, shift ? !(m->vsel.data[ee->a] && m->vsel.data[ee->b]) : 1);
            }
        }
    } else {
        u32 f = pick_face(ed, ed->cam_pos, ray_dir(ed, p));
        if (!shift) a3_emesh_select_all(m, 0);
        if (f != A3_ENONE) a3_emesh_select_face(m, f, shift ? !m->fsel.data[f] : 1);
    }
    a3_emesh_sync_selection(m);
}

static void box_select(A3Editor *ed, A3Vec2 a, A3Vec2 b, b32 shift) {
    EdModel *md = model(ed);
    A3EMesh *m = &md->mesh;
    f32 x0 = a3_minf(a.x, b.x), x1 = a3_maxf(a.x, b.x), y0 = a3_minf(a.y, b.y), y1 = a3_maxf(a.y, b.y);
    if (!shift) a3_emesh_select_all(m, 0);
    if (m->select_mode == A3_ESEL_FACE) {
        for (u32 f = 0; f < a3_emesh_face_count(m); ++f) {
            if (!md->front[f] && !md->xray) continue;
            A3Vec2 s;
            if (to_screen(ed, a3_mat4_mul_point(&md->world, a3_emesh_face_center(m, f)), &s) && s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1) m->fsel.data[f] = 1;
        }
    } else {
        for (u32 v = 0; v < m->pos.count; ++v)
            if (md->vis[v] && md->sp[v].x >= x0 && md->sp[v].x <= x1 && md->sp[v].y >= y0 && md->sp[v].y <= y1) m->vsel.data[v] = 1;
    }
    a3_emesh_sync_selection(m);
}

/* Called by the viewport panel every frame while Edit Mode is on. vp = view-projection.
 * Returns true when it used the mouse this frame (the viewport then skips object picking). */
b32 ed_model_viewport(A3Editor *ed, A3Ui *ui, const A3Mat4 *vp, b32 input_ok) {
    EdModel *md = model(ed);
    A3Entity e = target(ed);
    if (!a3_entity_valid(ed->world, e) || ed->mode != ED_EDIT) { ed_model_exit(ed, 1); return 0; }
    a3_transform_system_update(ed->world);
    md->world = a3_transform_compute_world(ed->world, e);
    if (!a3_mat4_inverse(&md->world, &md->inv_world)) md->inv_world = a3_mat4_identity();
    md->vp = *vp;
    project(ed);
    const A3InputState *in = a3_ui_input(ui);
    b32 used = 0;
    b32 ctrl = (in->mods & A3_MOD_CTRL) != 0, shift = (in->mods & A3_MOD_SHIFT) != 0, alt = (in->mods & A3_MOD_ALT) != 0;
    b32 keys = input_ok && !a3_ui_wants_keyboard(ui) && !ed->cam_flying;
    md->hover_edge = A3_ENONE;
    if (md->tool == TOOL_LOOPCUT) {
        md->hover_edge = nearest_edge(ed, in->mouse_pos, 24);
        if (input_ok && in->scroll.y != 0) md->cuts = (u32)a3_clampi((i32)md->cuts + (in->scroll.y > 0 ? 1 : -1), 1, 16);
        if (input_ok && in->mouse_pressed[A3_MOUSE_LEFT]) {
            if (md->hover_edge != A3_ENONE && a3_emesh_loop_cut(&md->mesh, md->hover_edge, md->cuts)) committed(ed);
            md->tool = TOOL_NONE;
            used = 1;
        }
        if (in->mouse_pressed[A3_MOUSE_RIGHT] || in->keys_pressed[A3_KEY_ESCAPE]) { md->tool = TOOL_NONE; used = 1; }
    } else if (md->tool) {
        if (keys) {
            if (in->keys_pressed[A3_KEY_X]) md->axis = md->axis == 0 ? -1 : 0;
            if (in->keys_pressed[A3_KEY_Y]) md->axis = md->axis == 1 ? -1 : 1;
            if (in->keys_pressed[A3_KEY_Z]) md->axis = md->axis == 2 ? -1 : 2;
        }
        update_tool(ed, in->mouse_pos, ctrl);
        if (in->mouse_pressed[A3_MOUSE_LEFT] || in->keys_pressed[A3_KEY_ENTER]) { end_tool(ed, 1); used = 1; }
        else if (in->mouse_pressed[A3_MOUSE_RIGHT] || in->keys_pressed[A3_KEY_ESCAPE]) { end_tool(ed, 0); used = 1; }
        else used = 1;
    } else if (input_ok) {
        /* selection with the mouse */
        /* Alt + left drag orbits the camera; double click on an edge selects its loop */
        if (in->mouse_double_click[A3_MOUSE_LEFT] && !alt) {
            u32 le = nearest_edge(ed, in->mouse_pos, 10);
            if (le != A3_ENONE) {
                if (!shift) a3_emesh_select_all(&md->mesh, 0);
                if (md->mesh.select_mode == A3_ESEL_FACE) a3_emesh_set_select_mode(&md->mesh, A3_ESEL_EDGE);
                a3_emesh_select_edge_loop(&md->mesh, le);
            }
            md->box_pending = md->box_active = 0;
            used = 1;
        } else if (in->mouse_pressed[A3_MOUSE_LEFT] && !alt) { md->box_pending = 1; md->box_start = in->mouse_pos; used = 1; }
        if (md->box_pending && in->mouse[A3_MOUSE_LEFT] && a3_v2_len(a3_v2_sub(in->mouse_pos, md->box_start)) > 5) md->box_active = 1;
        if (md->box_pending && in->mouse_released[A3_MOUSE_LEFT]) {
            if (md->box_active) box_select(ed, md->box_start, in->mouse_pos, shift);
            else click_select(ed, in->mouse_pos, shift, 0);
            md->box_pending = md->box_active = 0;
            used = 1;
        }
        if (keys && !ctrl && !alt) {
            if (in->keys_pressed[A3_KEY_1]) a3_emesh_set_select_mode(&md->mesh, A3_ESEL_VERTEX);
            if (in->keys_pressed[A3_KEY_2]) a3_emesh_set_select_mode(&md->mesh, A3_ESEL_EDGE);
            if (in->keys_pressed[A3_KEY_3]) a3_emesh_set_select_mode(&md->mesh, A3_ESEL_FACE);
            if (in->keys_pressed[A3_KEY_A]) a3_emesh_select_all(&md->mesh, 1);
            if (in->keys_pressed[A3_KEY_L]) a3_emesh_select_linked(&md->mesh);
            if (in->keys_pressed[A3_KEY_G]) start_tool(ed, TOOL_GRAB, in->mouse_pos);
            if (in->keys_pressed[A3_KEY_R]) start_tool(ed, TOOL_ROTATE, in->mouse_pos);
            if (in->keys_pressed[A3_KEY_S]) start_tool(ed, TOOL_SCALE, in->mouse_pos);
            if (in->keys_pressed[A3_KEY_E]) op_extrude_interactive(ed, in->mouse_pos);
            if (in->keys_pressed[A3_KEY_I]) start_tool(ed, TOOL_INSET, in->mouse_pos);
            if (in->keys_pressed[A3_KEY_X] || in->keys_pressed[A3_KEY_DELETE]) op_delete(ed);
            if (in->keys_pressed[A3_KEY_F]) op(ed, "fill");
            if (in->keys_pressed[A3_KEY_M]) { char b[32]; a3_snprintf(b, sizeof(b), "merge:%g", (f64)md->merge_dist); op(ed, b); }
            if (shift && in->keys_pressed[A3_KEY_D]) { if (ed_model_run(ed, "duplicate")) start_tool(ed, TOOL_GRAB, in->mouse_pos); }
            if (shift && in->keys_pressed[A3_KEY_N]) op(ed, "recalc-normals");
            if (in->keys_pressed[A3_KEY_TAB]) ed_model_exit(ed, 1);
        }
        if (keys && alt && in->keys_pressed[A3_KEY_A]) a3_emesh_select_all(&md->mesh, 0);
        if (keys && alt && in->keys_pressed[A3_KEY_Z]) md->xray = !md->xray;
        if (keys && ctrl && in->keys_pressed[A3_KEY_I]) a3_emesh_select_invert(&md->mesh);
        if (keys && ctrl && in->keys_pressed[A3_KEY_R]) { md->tool = TOOL_LOOPCUT; a3_emesh_copy(&md->snap, &md->mesh); }
        if (keys && ctrl && in->keys_pressed[A3_KEY_2]) op(ed, "subdivide:smooth");
        if (keys && ctrl && in->keys_pressed[A3_KEY_EQUAL]) a3_emesh_select_more(&md->mesh);
    }
    if (md->active) draw(ed, ui);
    return used;
}

/* ======================================================================== */
/* Panel                                                                    */
/* ======================================================================== */

static b32 tool_button(A3Ui *ui, const char *label, const char *tip, f32 w) {
    b32 r = a3_ui_button_ex(ui, label, w, A3_BUTTON_SMALL);
    if (tip) a3_ui_tooltip(ui, tip);
    return r;
}

void ed_model_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    A3_UNUSED(r);
    EdModel *md = model(ed);
    A3UiTheme *th = a3_ui_theme(ui);
    if (!md->active) {
        a3_ui_heading(ui, "Modeling");
        f32 wdt = a3_ui_content_width(ui);
        const char *intro = "Edit the shape of an object: move vertices, extrude faces, cut loops and smooth the result. "
                            "Select an object in the viewport and press Tab (or the button below). Primitives are converted to a model "
                            "file in Assets/Models so you can keep editing it later.";
        A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt, intro));
        a3_ui_text_wrapped(ui, A3_FONT_UI, tr, th->colors[A3_UIC_TEXT_DIM], intro);
        a3_ui_spacing(ui, 6);
        b32 editing = ed->mode == ED_EDIT;
        if (a3_ui_button_ex(ui, "Edit Selected Mesh (Tab)", 0, A3_BUTTON_PRIMARY | (editing ? 0 : A3_BUTTON_DISABLED)) && editing) ed_model_enter(ed);
        if (a3_ui_button_ex(ui, "New Model", 0, editing ? 0 : A3_BUTTON_DISABLED) && editing) {
            A3Entity e = ed_create_entity(ed, "Model", A3_PRIM_CUBE, 0);
            ed_undo_end_frame(ed);
            ed_select(ed, e);
            ed_model_enter(ed);
        }
        a3_ui_tooltip(ui, "Creates a cube object and opens it in Edit Mode");
        return;
    }
    A3EMesh *m = &md->mesh;
    a3_ui_label_colored(ui, th->colors[A3_UIC_ACCENT], "Editing %s", md->path);
    if (a3_ui_button_ex(ui, "Done (Tab)", 100, A3_BUTTON_PRIMARY)) { ed_model_exit(ed, 1); return; }
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Undo", 60, md->undo_pos > 0 ? 0 : A3_BUTTON_DISABLED)) ed_model_undo(ed);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Redo", 60, md->undo_pos + 1 < md->undo_count ? 0 : A3_BUTTON_DISABLED)) ed_model_redo(ed);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Discard", 70, A3_BUTTON_FLAT)) { ed_model_exit(ed, 0); return; }
    a3_ui_tooltip(ui, "Leave Edit Mode without saving the changes");
    a3_ui_heading(ui, "Select");
    static const char *const modes[] = { "Vertex (1)", "Edge (2)", "Face (3)" };
    for (i32 i = 0; i < 3; ++i) {
        if (i) a3_ui_same_line(ui);
        if (a3_ui_button_ex(ui, modes[i], 88, A3_BUTTON_SMALL | (m->select_mode == i ? A3_BUTTON_TOGGLED : 0))) a3_emesh_set_select_mode(m, (A3ESelectMode)i);
    }
    if (tool_button(ui, "All", "A", 50)) a3_emesh_select_all(m, 1);
    a3_ui_same_line(ui);
    if (tool_button(ui, "None", "Alt+A", 50)) a3_emesh_select_all(m, 0);
    a3_ui_same_line(ui);
    if (tool_button(ui, "Invert", "Ctrl+I", 56)) a3_emesh_select_invert(m);
    a3_ui_same_line(ui);
    if (tool_button(ui, "Linked", "L: whole connected parts", 56)) a3_emesh_select_linked(m);
    a3_ui_same_line(ui);
    if (tool_button(ui, "More", "Ctrl+=: grow by one ring", 50)) a3_emesh_select_more(m);
    a3_ui_checkbox(ui, "X-Ray (Alt+Z): see and select through the mesh", &md->xray);

    a3_ui_heading(ui, "Transform");
    A3Vec2 mouse = a3_v2(ed->vp_rect.x + ed->vp_rect.w * 0.5f + 80, ed->vp_rect.y + ed->vp_rect.h * 0.5f);
    if (tool_button(ui, "Move (G)", "Then move the mouse over the viewport; X/Y/Z constrain", 80)) start_tool(ed, TOOL_GRAB, mouse);
    a3_ui_same_line(ui);
    if (tool_button(ui, "Rotate (R)", 0, 80)) start_tool(ed, TOOL_ROTATE, mouse);
    a3_ui_same_line(ui);
    if (tool_button(ui, "Scale (S)", 0, 80)) start_tool(ed, TOOL_SCALE, mouse);
    a3_ui_property(ui, "Move by", "Exact offset for the selection");
    a3_ui_drag_float_n(ui, "mv", &md->move_vec.x, 3, 0.05f, -1000, 1000, "%.2f");
    if (tool_button(ui, "Apply Move", 0, 100)) {
        char b[96];
        a3_snprintf(b, sizeof(b), "translate:%g,%g,%g", (f64)md->move_vec.x, (f64)md->move_vec.y, (f64)md->move_vec.z);
        op(ed, b);
    }

    a3_ui_heading(ui, "Mesh Tools");
    a3_ui_property(ui, "Extrude distance", "E extrudes interactively; this button uses the distance");
    a3_ui_drag_float(ui, "ex", &md->extrude_dist, 0.02f, -100, 100, "%.2f");
    if (tool_button(ui, "Extrude Faces", "E", 110)) { char b[48]; a3_snprintf(b, sizeof(b), "extrude:%g", (f64)md->extrude_dist); op(ed, b); }
    a3_ui_property(ui, "Inset / depth", "Amount toward each face center, then depth along its normal");
    a3_ui_slider_float(ui, "ia", &md->inset_amount, 0, 0.95f, "%.2f");
    a3_ui_drag_float(ui, "id", &md->inset_depth, 0.01f, -10, 10, "%.2f");
    if (tool_button(ui, "Inset Faces", "I", 110)) { char b[64]; a3_snprintf(b, sizeof(b), "inset:%g,%g", (f64)md->inset_amount, (f64)md->inset_depth); op(ed, b); }
    if (tool_button(ui, "Loop Cut (Ctrl+R)", "Then click an edge in the viewport", 140)) { md->tool = TOOL_LOOPCUT; a3_emesh_copy(&md->snap, m); }
    if (tool_button(ui, "Subdivide Smooth", "Ctrl+2: Catmull-Clark subdivision of the whole mesh", 140)) op(ed, "subdivide:smooth");
    a3_ui_same_line(ui);
    if (tool_button(ui, "Subdivide", "Split every face into quads, keeping the shape", 90)) op(ed, "subdivide:simple");
    if (tool_button(ui, "Delete (X)", 0, 90)) op_delete(ed);
    a3_ui_same_line(ui);
    if (tool_button(ui, "Fill (F)", "Face from the selected boundary loop or vertices", 70)) op(ed, "fill");
    a3_ui_same_line(ui);
    if (tool_button(ui, "Duplicate", "Shift+D", 80)) op(ed, "duplicate");
    a3_ui_property(ui, "Merge distance", 0);
    a3_ui_drag_float(ui, "md", &md->merge_dist, 0.0005f, 0.00001f, 10, "%.4f");
    if (tool_button(ui, "Merge by Distance (M)", 0, 160)) { char b[32]; a3_snprintf(b, sizeof(b), "merge:%g", (f64)md->merge_dist); op(ed, b); }
    if (tool_button(ui, "Flip Normals", 0, 100)) op(ed, "flip");
    a3_ui_same_line(ui);
    if (tool_button(ui, "Recalculate (Shift+N)", "Point all faces outward consistently", 150)) op(ed, "recalc-normals");
    a3_ui_label(ui, "Mirror");
    a3_ui_same_line(ui);
    if (tool_button(ui, "X", "Mirror across the YZ plane (vertices on it are shared)", 30)) op(ed, "mirror:x");
    a3_ui_same_line(ui);
    if (tool_button(ui, "Y", 0, 30)) op(ed, "mirror:y");
    a3_ui_same_line(ui);
    if (tool_button(ui, "Z", 0, 30)) op(ed, "mirror:z");

    a3_ui_heading(ui, "Add");
    static const char *const prims[] = { "Cube", "Plane", "Grid", "Cylinder", "Cone", "Sphere", "Torus" };
    a3_ui_combo(ui, "addp", &md->add_prim, prims, A3_EPRIM_COUNT);
    a3_ui_drag_float(ui, "adds", &md->add_size, 0.05f, 0.01f, 1000, "size %.2f");
    if (tool_button(ui, "Add to Mesh", "Adds the shape at the center of the object, selected", 110)) {
        char b[64];
        a3_snprintf(b, sizeof(b), "add:%s,%g", a3_eprim_names[md->add_prim], (f64)md->add_size);
        op(ed, b);
    }

    a3_ui_heading(ui, "Shading");
    if (a3_ui_button_ex(ui, "Flat", 60, A3_BUTTON_SMALL | (m->smooth ? 0 : A3_BUTTON_TOGGLED))) op(ed, "shading:flat");
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Smooth", 70, A3_BUTTON_SMALL | (m->smooth ? A3_BUTTON_TOGGLED : 0))) op(ed, "shading:smooth");

    A3EMeshStats st;
    a3_emesh_stats(m, &st);
    a3_ui_spacing(ui, 6);
    a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "%u vertices, %u edges, %u faces (%u triangles)", st.vertices, st.edges, st.faces, st.triangles);
    a3_ui_label_colored(ui, st.closed ? th->colors[A3_UIC_SUCCESS] : th->colors[A3_UIC_TEXT_DIM], "%s", st.closed ? "Closed (watertight) mesh" :
                        st.nonmanifold_edges ? "Has edges shared by more than two faces" : "Open mesh (has boundary edges)");
    a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "Kernels: %s", a3_mk_backend());
}
