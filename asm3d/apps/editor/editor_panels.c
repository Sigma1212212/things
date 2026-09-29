/*
 * ASM3D Editor - Hierarchy, Inspector, Console, Assets, Profiler, Docs and
 * Settings panels.
 *
 * The Inspector and Docs are generated from component reflection, so plugin
 * and custom components get UI, undo and documentation automatically.
 */
#include "editor.h"
#include "../../engine/script/a3_script_engine.h"
#include "../../engine/physics/a3_physics.h"
#include "../../engine/audio/a3_audio.h"
#include "../../engine/particles/a3_particles.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/scene/a3_scene_io.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/core/a3_json.h"
#include "../../engine/platform/a3_platform.h"

static u32 col(A3Ui *ui, A3UiColor c) { return a3_ui_theme(ui)->colors[c]; }
static b32 beginner(A3Editor *ed) { return ed->level == ED_LEVEL_BEGINNER; }

/* ======================================================================== */
/* Hierarchy                                                                */
/* ======================================================================== */

static u32 entity_icon(A3World *w, A3Entity e, u32 *color, A3Ui *ui) {
    *color = 0;
    if (a3_component_has(w, e, A3_T_CAMERA)) return A3_ICON_TARGET;
    A3CLight *l = (A3CLight *)a3_component_get(w, e, A3_T_LIGHT);
    if (l) { *color = col(ui, A3_UIC_WARNING); return l->type == A3_LIGHT_DIRECTIONAL ? A3_ICON_SUN : A3_ICON_LIGHT; }
    if (a3_component_has(w, e, A3_T_CHARACTER)) { *color = col(ui, A3_UIC_SUCCESS); return A3_ICON_HEART; }
    if (a3_component_has(w, e, A3_T_RIGIDBODY)) return A3_ICON_BOX;
    if (a3_component_has(w, e, A3_T_PARTICLE_EMITTER)) { *color = col(ui, A3_UIC_WARNING); return A3_ICON_STAR; }
    if (a3_component_has(w, e, A3_T_AUDIO_SOURCE) && !a3_component_has(w, e, A3_T_MESH_RENDERER)) { *color = col(ui, A3_UIC_ACCENT); return A3_ICON_MUSIC; }
    if (a3_component_has(w, e, A3_T_MESH_RENDERER)) return A3_ICON_CUBE;
    if (a3_component_has(w, e, A3_T_WORLD_SETTINGS)) return A3_ICON_CLOUD;
    return A3_ICON_DIAMOND_O;
}

typedef enum HierAction { HA_NONE = 0, HA_DELETE, HA_DUPLICATE, HA_CHILD, HA_UNPARENT, HA_REPARENT, HA_FOCUS } HierAction;
typedef struct HierCtx { A3Editor *ed; HierAction action; u64 target, other; } HierCtx;

static void reparent_with_undo(A3Editor *ed, u64 child, u64 parent) {
    A3World *w = ed->world;
    A3Entity c = a3_entity_find_by_guid(w, child);
    A3Entity p = parent ? a3_entity_find_by_guid(w, parent) : A3_ENTITY_NULL;
    if (!a3_entity_valid(w, c)) return;
    char *before = ed_snapshot(w, child);
    /* keep the world position when changing parents */
    A3Vec3 wp = a3_transform_world_position(w, c);
    if (!a3_entity_set_parent(w, c, p)) {
        a3_free(before);
        a3_ui_notify(ed->ui, col(ed->ui, A3_UIC_WARNING), "An object cannot become a child of its own child");
        return;
    }
    if (a3_transform(w, c)) { a3_transform_system_update(w); a3_transform_set_world_position(w, c, wp); }
    char *after = ed_snapshot(w, child);
    if (before && after) ed_undo_push(ed, ED_UNDO_MODIFY, parent ? "Parent" : "Unparent", child, before, after);
    else { a3_free(before); a3_free(after); }
}

static void hier_node(HierCtx *hc, A3Ui *ui, A3World *w, A3Entity e) {
    A3Editor *ed = hc->ed;
    A3EntityRecord *rec = a3_entity_record(w, e);
    if (!rec || (rec->flags & A3_ENTITY_EDITOR_ONLY)) return;
    char id[24];
    a3_guid_to_string(rec->guid, id);
    u32 icolor;
    u32 icon = entity_icon(w, e, &icolor, ui);
    u32 flags = 0;
    if (rec->guid == ed->selected) flags |= A3_TREE_SELECTED;
    if (a3_entity_is_null(rec->first_child)) flags |= A3_TREE_LEAF;
    if (!a3_entity_active(w, e)) flags |= A3_TREE_DIM;
    b32 clicked = 0;
    b32 open = a3_ui_tree_node(ui, id, rec->name[0] ? rec->name : "(unnamed)", icon, flags, &clicked);
    if (clicked) ed->selected = rec->guid;
    if (a3_ui_item_double_clicked(ui)) { ed->selected = rec->guid; hc->action = HA_FOCUS; }
    if (ed->mode == ED_EDIT) {
        a3_ui_drag_source(ui, "entity", &rec->guid, sizeof(u64), rec->name);
        u32 sz;
        const u64 *dropped = (const u64 *)a3_ui_drop_target(ui, "entity", &sz);
        if (dropped && sz == sizeof(u64) && *dropped != rec->guid) { hc->action = HA_REPARENT; hc->target = *dropped; hc->other = rec->guid; }
        char cm[40];
        a3_snprintf(cm, sizeof(cm), "ctx_%s", id);
        if (a3_ui_begin_context_menu(ui, cm)) {
            if (a3_ui_menu_item(ui, "Create Child", 0, 1)) { hc->action = HA_CHILD; hc->target = rec->guid; }
            if (a3_ui_menu_item(ui, "Duplicate", "Ctrl+D", 1)) { hc->action = HA_DUPLICATE; hc->target = rec->guid; }
            if (a3_ui_menu_item(ui, "Unparent", 0, !a3_entity_is_null(rec->parent))) { hc->action = HA_UNPARENT; hc->target = rec->guid; }
            if (a3_ui_menu_item(ui, "Focus", "F", 1)) { hc->action = HA_FOCUS; hc->target = rec->guid; }
            a3_ui_menu_separator(ui);
            if (a3_ui_menu_item(ui, "Delete", "Del", 1)) { hc->action = HA_DELETE; hc->target = rec->guid; }
            a3_ui_end_popup(ui);
        }
    }
    if (open) {
        if (!(flags & A3_TREE_LEAF))
            for (A3Entity c = a3_entity_first_child(w, e); !a3_entity_is_null(c); c = a3_entity_next_sibling(w, c)) hier_node(hc, ui, w, c);
        a3_ui_tree_pop(ui);
    }
}

void ed_hierarchy_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    A3_UNUSED(r);
    A3World *w = ed_active_world(ed);
    if (!w) return;
    a3_ui_input_text(ui, "hier_search", ed->hierarchy_filter, sizeof(ed->hierarchy_filter), A3_INPUT_SEARCH, "Search objects...");
    a3_ui_spacing(ui, 2);
    HierCtx hc = { ed, HA_NONE, 0, 0 };
    if (ed->hierarchy_filter[0]) {
        /* flat list of matches */
        for (u32 i = 0; i < w->high_water; ++i) {
            A3EntityRecord *rec = &w->entities[i];
            if (!rec->alive || (rec->flags & A3_ENTITY_EDITOR_ONLY) || !a3_stristr(rec->name, ed->hierarchy_filter)) continue;
            A3Entity e = { i, rec->gen };
            u32 icolor;
            u32 icon = entity_icon(w, e, &icolor, ui);
            a3_ui_push_id_int(ui, (i64)rec->guid);
            if (a3_ui_selectable_ex(ui, "row", rec->name, rec->guid == ed->selected, icon, icolor)) ed->selected = rec->guid;
            a3_ui_pop_id(ui);
        }
    } else {
        for (A3Entity e = w->first_root; !a3_entity_is_null(e); e = a3_entity_next_sibling(w, e)) hier_node(&hc, ui, w, e);
        /* empty area: drop here to unparent, right-click to create */
        A3Rect rest = a3_ui_next_rect(ui, 0, a3_maxf(a3_ui_panel_rect(ui).h - a3_ui_cursor_pos(ui).y + a3_ui_panel_rect(ui).y, 40));
        if (ed->mode == ED_EDIT) {
            u32 sz;
            const u64 *dropped = (const u64 *)a3_ui_drop_target_rect(ui, rest, "entity", &sz);
            if (dropped && sz == sizeof(u64)) { hc.action = HA_REPARENT; hc.target = *dropped; hc.other = 0; }
            a3_ui_invisible_button(ui, "hier_empty", rest);
            if (a3_ui_item_clicked(ui, A3_MOUSE_LEFT)) ed->selected = 0;
            if (a3_ui_begin_context_menu(ui, "hier_ctx")) {
                if (a3_ui_menu_item(ui, "Create Empty", 0, 1)) ed_create_entity(ed, "Empty", A3_PRIM_NONE, 0);
                if (a3_ui_menu_item(ui, "Create Cube", 0, 1)) ed_create_entity(ed, "Cube", A3_PRIM_CUBE, 0);
                if (a3_ui_menu_item(ui, "Create Light", 0, 1)) ed_create_entity(ed, "Point Light", A3_PRIM_NONE, "Light");
                a3_ui_end_popup(ui);
            }
            if (beginner(ed) && a3_world_entity_count(w) < 3)
                a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect_shrink(rest, 8), col(ui, A3_UIC_TEXT_DIM), "Your scene is almost empty. Use + Add in the toolbar to place shapes, lights and a player.");
        }
    }
    /* apply deferred actions (never modify the hierarchy while drawing it) */
    switch (hc.action) {
    case HA_DELETE: ed->selected = hc.target; ed_delete_selected(ed); break;
    case HA_DUPLICATE: ed->selected = hc.target; ed_duplicate_selected(ed); break;
    case HA_FOCUS: if (hc.target) ed->selected = hc.target; ed_focus_selected(ed); break;
    case HA_UNPARENT: reparent_with_undo(ed, hc.target, 0); break;
    case HA_REPARENT: reparent_with_undo(ed, hc.target, hc.other); break;
    case HA_CHILD: {
        A3Entity c = ed_create_entity(ed, "Child", A3_PRIM_NONE, 0);
        A3Entity p = a3_entity_find_by_guid(ed->world, hc.target);
        if (a3_entity_valid(ed->world, c) && a3_entity_valid(ed->world, p)) {
            a3_entity_set_parent(ed->world, c, p);
            A3CTransform *t = a3_transform(ed->world, c);
            if (t) t->position = a3_v3_zero();
        }
    } break;
    default: break;
    }
}

/* ======================================================================== */
/* Inspector                                                                */
/* ======================================================================== */

static b32 asset_matches_kind(const char *path, u32 kind) {
    const char *ext = a3_path_extension(path);
    switch (kind) {
    case A3_ASSET_MESH: return a3_streq(ext, ".obj");
    case A3_ASSET_TEXTURE: return a3_streq(ext, ".png") || a3_streq(ext, ".tga") || a3_streq(ext, ".bmp") || a3_streq(ext, ".ppm");
    case A3_ASSET_MATERIAL: return a3_streq(ext, ".a3mat") || a3_streq(ext, ".a3shader");
    case A3_ASSET_SHADER: return a3_streq(ext, ".a3shader");
    case A3_ASSET_SCENE: return a3_streq(ext, ".a3scene");
    case A3_ASSET_SOUND: return a3_streq(ext, ".wav");
    default: return 1;
    }
}

void ed_draw_field(A3Editor *ed, A3Ui *ui, const A3FieldDesc *f, void *component, const char *comp_name) {
    u8 *p = (u8 *)component + f->offset;
    char label[160];
    a3_snprintf(label, sizeof(label), "%s %s", comp_name, f->label);
    a3_ui_property(ui, f->label, f->doc);
    a3_ui_push_id(ui, f->name);
    b32 changed = 0;
    b32 ro = (f->flags & A3_FIELD_FLAG_READONLY) != 0;
    b32 bounded = f->max > f->min;
    switch (f->type) {
    case A3_FIELD_BOOL: {
        b32 v = *(b32 *)p != 0;
        if (a3_ui_toggle(ui, "v", &v) && !ro) { *(b32 *)p = v; changed = 1; }
    } break;
    case A3_FIELD_I32:
        if (ro) a3_ui_label(ui, "%d", *(i32 *)p);
        else changed = a3_ui_drag_int(ui, "v", (i32 *)p, f->step > 0 ? f->step : 0.1f, bounded ? (i32)f->min : 0, bounded ? (i32)f->max : 0);
        break;
    case A3_FIELD_U32: {
        i32 v = (i32)*(u32 *)p;
        if (ro) a3_ui_label(ui, "%u", *(u32 *)p);
        else if (a3_ui_drag_int(ui, "v", &v, f->step > 0 ? f->step : 0.1f, bounded ? (i32)f->min : 0, bounded ? (i32)f->max : 0x7FFFFFFF)) { *(u32 *)p = (u32)a3_maxi(v, 0); changed = 1; }
    } break;
    case A3_FIELD_F32:
        if (ro) a3_ui_label(ui, "%.3f", (f64)*(f32 *)p);
        else if ((f->flags & A3_FIELD_FLAG_SLIDER) && bounded) changed = a3_ui_slider_float(ui, "v", (f32 *)p, f->min, f->max, "%.2f");
        else changed = a3_ui_drag_float(ui, "v", (f32 *)p, f->step > 0 ? f->step : 0.02f, bounded ? f->min : 0, bounded ? f->max : 0, "%.3f");
        break;
    case A3_FIELD_VEC2: changed = !ro && a3_ui_drag_float_n(ui, "v", (f32 *)p, 2, f->step > 0 ? f->step : 0.02f, bounded ? f->min : 0, bounded ? f->max : 0, "%.2f"); break;
    case A3_FIELD_VEC3: changed = !ro && a3_ui_drag_float_n(ui, "v", (f32 *)p, 3, f->step > 0 ? f->step : 0.02f, bounded ? f->min : 0, bounded ? f->max : 0, "%.2f"); break;
    case A3_FIELD_VEC4: changed = !ro && a3_ui_drag_float_n(ui, "v", (f32 *)p, 4, f->step > 0 ? f->step : 0.02f, bounded ? f->min : 0, bounded ? f->max : 0, "%.2f"); break;
    case A3_FIELD_COLOR: changed = !ro && a3_ui_color_edit(ui, "v", (A3Vec4 *)p, 0); break;
    case A3_FIELD_QUAT: {
        /* edited as euler degrees; the displayed angles are cached while dragging so they do not jump */
        static u64 cache_key;
        static f32 cache_deg[3];
        A3Quat *q = (A3Quat *)p;
        u64 key = (u64)(uintptr_t)p;
        A3Vec3 e = a3_quat_to_euler(*q);
        f32 deg[3] = { e.x * A3_RAD2DEG, e.y * A3_RAD2DEG, e.z * A3_RAD2DEG };
        if (cache_key == key) for (int i = 0; i < 3; ++i) deg[i] = cache_deg[i];
        if (!ro && a3_ui_drag_float_n(ui, "v", deg, 3, 0.5f, 0, 0, "%.1f")) {
            *q = a3_quat_normalize(a3_quat_euler(deg[0] * A3_DEG2RAD, deg[1] * A3_DEG2RAD, deg[2] * A3_DEG2RAD));
            changed = 1;
        }
        if (a3_ui_item_active(ui)) { cache_key = key; for (int i = 0; i < 3; ++i) cache_deg[i] = deg[i]; }
        else if (cache_key == key) cache_key = 0;
    } break;
    case A3_FIELD_STRING:
        changed = !ro && a3_ui_input_text(ui, "v", (char *)p, f->size ? f->size : A3_NAME_MAX, 0, 0);
        break;
    case A3_FIELD_ENUM: {
        i32 v = *(i32 *)p;
        if (f->enum_names && f->enum_count && !ro && a3_ui_combo(ui, "v", &v, f->enum_names, f->enum_count)) { *(i32 *)p = v; changed = 1; }
        else if (!f->enum_names) a3_ui_label(ui, "%d", v);
    } break;
    case A3_FIELD_ASSET: {
        A3AssetRef *ref = (A3AssetRef *)p;
        f32 wfull = a3_ui_content_width(ui);
        A3Vec2 cp = a3_ui_cursor_pos(ui);
        a3_ui_set_cursor_pos(ui, cp);
        if (!ro && a3_ui_input_text(ui, "v", ref->path, sizeof(ref->path), A3_INPUT_ENTER_RETURNS, "None (drag an asset here)")) { ref->handle = 0; changed = 1; }
        u32 sz;
        const char *drop = (const char *)a3_ui_drop_target(ui, "asset", &sz);
        if (drop && !ro) {
            if (asset_matches_kind(drop, f->asset_kind)) { a3_strcpy(ref->path, sizeof(ref->path), drop); ref->handle = 0; changed = 1; }
            else a3_ui_notify(ui, col(ui, A3_UIC_WARNING), "%s does not fit '%s'", a3_path_filename(drop), f->label);
        }
        A3_UNUSED(wfull);
        if (ref->path[0] && !ro) {
            a3_ui_same_line(ui);
            if (a3_ui_icon_button(ui, A3_ICON_CROSS, "Clear", A3_BUTTON_FLAT | A3_BUTTON_SMALL)) { ref->path[0] = 0; ref->handle = 0; changed = 1; }
        }
    } break;
    case A3_FIELD_ENTITY: {
        A3EntityRef *ref = (A3EntityRef *)p;
        A3World *w = ed_active_world(ed);
        A3Entity target = ref->guid ? a3_entity_find_by_guid(w, ref->guid) : A3_ENTITY_NULL;
        char text[96];
        a3_snprintf(text, sizeof(text), "%s", ref->guid ? (a3_entity_valid(w, target) ? a3_entity_name(w, target) : "(missing)") : "None (drag an object here)");
        if (a3_ui_button_ex(ui, text, -28, A3_BUTTON_FLAT) && a3_entity_valid(w, target)) ed->selected = ref->guid;
        u32 sz;
        const u64 *drop = (const u64 *)a3_ui_drop_target(ui, "entity", &sz);
        if (drop && sz == sizeof(u64) && !ro) { ref->guid = *drop; ref->cached_index = 0xFFFFFFFFu; changed = 1; }
        a3_ui_same_line(ui);
        if (a3_ui_icon_button(ui, A3_ICON_CROSS, "Clear", A3_BUTTON_FLAT | A3_BUTTON_SMALL) && !ro) { ref->guid = 0; changed = 1; }
    } break;
    default: a3_ui_label(ui, "(unsupported)"); break;
    }
    a3_ui_pop_id(ui);
    if (changed) ed_undo_mark_changed(ed, label);
}

static void add_component_popup(A3Editor *ed, A3Ui *ui, A3World *w, A3Entity e) {
    if (!a3_ui_begin_popup(ui, "add_component", 300)) return;
    a3_ui_input_text(ui, "ac_search", ed->add_component_filter, sizeof(ed->add_component_filter), A3_INPUT_SEARCH | A3_INPUT_FOCUS, "Search components...");
    const char *last_cat = 0;
    u32 shown = 0;
    for (u32 id = 0; id < A3_MAX_COMPONENT_TYPES; ++id) {
        A3ComponentType *t = a3_component_type(id);
        if (!t || !t->name[0] || (t->flags & A3_COMP_HIDDEN) || a3_component_has(w, e, id)) continue;
        if ((t->flags & A3_COMP_UNIQUE) && a3_component_count(w, id) > 0) continue;
        if (ed->add_component_filter[0] && !a3_stristr(t->name, ed->add_component_filter) && !a3_stristr(t->category, ed->add_component_filter)) continue;
        if (!last_cat || !a3_streq(last_cat, t->category)) {
            a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "%s", t->category[0] ? t->category : "Other");
            last_cat = t->category;
        }
        a3_ui_push_id_int(ui, (i64)id);
        if (a3_ui_menu_item(ui, t->name, 0, 1)) {
            if (a3_component_add(w, e, id)) {
                ed_undo_mark_changed(ed, "Add Component");
                ed_show_tip_for_component(ed, id);
                A3_INFO("editor", "added %s to %s", t->name, a3_entity_name(w, e));
            }
            ed->add_component_filter[0] = 0;
            a3_ui_close_popup(ui);
        }
        if (t->doc) a3_ui_tooltip(ui, t->doc);
        a3_ui_pop_id(ui);
        ++shown;
    }
    if (!shown) a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "No matching components.");
    a3_ui_end_popup(ui);
}

/* Beginner guidance derived from what the object has (and lacks). */
static void contextual_tips(A3Editor *ed, A3Ui *ui, A3World *w, A3Entity e) {
    const char *tip = 0, *action = 0;
    u32 add = 0xFFFFFFFFu;
    if (ed->tip[0] && ed->tip_action[0] && !a3_component_get_by_name(w, e, ed->tip_action)) {
        tip = ed->tip; action = ed->tip_action; add = a3_component_id(ed->tip_action);
    } else if (a3_component_has(w, e, A3_T_RIGIDBODY) && !a3_component_has(w, e, A3_T_COLLIDER)) {
        tip = "This object has physics but no shape. Add a Collider so it can hit things."; action = "Collider"; add = A3_T_COLLIDER;
    } else if (a3_component_has(w, e, A3_T_MESH_RENDERER) && !a3_component_has(w, e, A3_T_COLLIDER) && !a3_component_has(w, e, A3_T_CHARACTER) && beginner(ed)) {
        tip = "Players can walk through this object. Add a Collider to make it solid."; action = "Collider"; add = A3_T_COLLIDER;
    } else if (a3_component_has(w, e, A3_T_COLLIDER) && !a3_component_has(w, e, A3_T_RIGIDBODY) && beginner(ed)) {
        tip = "This object is solid but never moves. Add a RigidBody to make it fall and get pushed around."; action = "RigidBody"; add = A3_T_RIGIDBODY;
    }
    if (!tip || ed->mode != ED_EDIT) return;
    a3_ui_spacing(ui, 6);
    A3Vec2 cp = a3_ui_cursor_pos(ui);
    f32 wdt = a3_ui_content_width(ui);
    f32 th = a3_ui_text_wrapped_height(A3_FONT_UI, wdt - 40, tip) + 44;
    A3Rect box = a3_rect(cp.x, cp.y, wdt, th);
    a3_ui_rect(ui, box, a3_color_alpha(col(ui, A3_UIC_ACCENT), 0.14f), 8);
    a3_ui_rect_outline(ui, box, a3_color_alpha(col(ui, A3_UIC_ACCENT), 0.5f), 8, 1);
    a3_ui_icon(ui, A3_ICON_STAR, a3_v2(box.x + 16, box.y + 16), col(ui, A3_UIC_ACCENT), A3_FONT_UI);
    a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect(box.x + 30, box.y + 8, box.w - 40, box.h), col(ui, A3_UIC_TEXT), tip);
    char btn[80];
    a3_snprintf(btn, sizeof(btn), "Add %s", action);
    if (a3_ui_button_rect(ui, btn, a3_rect(box.x + 30, box.y + box.h - 32, 150, 26), A3_BUTTON_PRIMARY | A3_BUTTON_SMALL) && add != 0xFFFFFFFFu) {
        if (a3_component_add(w, e, add)) { ed_undo_mark_changed(ed, "Add Component"); ed_show_tip_for_component(ed, add); }
    }
    a3_ui_set_cursor_pos(ui, a3_v2(cp.x, cp.y + th + 6));
}

static void assets_refresh(A3Editor *ed, b32 force);

/* Script component: create / open the file, show its last error. */
static void script_inspector_extras(A3Editor *ed, A3Ui *ui, A3Entity e, A3CScript *sc) {
    A3World *w = ed_active_world(ed);
    if (!sc->script.path[0]) {
        if (a3_ui_button_ex(ui, "Create New Script", 0, A3_BUTTON_SMALL | A3_BUTTON_PRIMARY) && ed->mode == ED_EDIT) {
            char base[64], rel[ED_PATH], abs[ED_PATH];
            a3_strcpy(base, sizeof(base), a3_entity_name(w, e));
            for (char *c = base; *c; ++c) if (!a3_is_ident(*c) && *c != '-') *c = '_';
            for (int i = 0; i < 100; ++i) {
                if (i == 0) a3_snprintf(rel, sizeof(rel), "Assets/Scripts/%s.a3script", base);
                else a3_snprintf(rel, sizeof(rel), "Assets/Scripts/%s%d.a3script", base, i + 1);
                ed_project_path(ed, rel, abs, sizeof(abs));
                if (!a3_file_exists(abs)) break;
            }
            char dir[ED_PATH];
            a3_path_dirname(abs, dir, sizeof(dir));
            a3_dir_create(dir);
            const char *tpl = ed_script_template();
            if (a3_file_write_atomic(abs, tpl, a3_strlen(tpl)) == A3_OK) {
                a3_strcpy(sc->script.path, sizeof(sc->script.path), rel);
                ed_undo_mark_changed(ed, "Create Script");
                assets_refresh(ed, 1);
                if (ed_code_open(ed, rel) >= 0) a3_dock_show(&ed->dock, "Code");
            }
        }
        a3_ui_tooltip(ui, "Makes a .a3script file for this object and opens it in the code editor");
        return;
    }
    if (a3_ui_button_ex(ui, "Edit Script", 0, A3_BUTTON_SMALL)) { if (ed_code_open(ed, sc->script.path) >= 0) a3_dock_show(&ed->dock, "Code"); }
    a3_ui_tooltip(ui, "Open the script in the code editor (saving it reloads running objects)");
    /* newest error for this file */
    for (u32 i = a3_scripts_error_count(); i-- > 0;) {
        const A3ScriptErrorInfo *er = a3_scripts_error(i);
        if (!a3_streq(er->path, sc->script.path)) continue;
        char msg[480];
        a3_snprintf(msg, sizeof(msg), "Line %d: %s%s%s", er->error.line, er->error.message, er->error.hint[0] ? ". " : "", er->error.hint);
        f32 wdt = a3_ui_content_width(ui);
        A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt, msg));
        a3_ui_text_wrapped(ui, A3_FONT_UI, tr, col(ui, A3_UIC_ERROR), msg);
        break;
    }
}

void ed_inspector_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    A3_UNUSED(r);
    A3World *w = ed_active_world(ed);
    A3Entity e = ed_selected(ed);
    if (!w || !a3_entity_valid(w, e)) {
        a3_ui_spacing(ui, 10);
        a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "Nothing selected.");
        a3_ui_label_wrapped(ui, "Click an object in the viewport or the Hierarchy to see and change its settings here.");
        a3_ui_spacing(ui, 10);
        if (w && a3_ui_button_ex(ui, "Scene Settings (sky, fog, gravity)", 0, 0)) {
            a3_world_settings(w);
            u32 n = 0;
            const A3Entity *ents = 0;
            a3_component_array(w, A3_T_WORLD_SETTINGS, &n, &ents);
            if (n) ed_select(ed, ents[0]);
        }
        return;
    }
    A3EntityRecord *rec = a3_entity_record(w, e);
    if (ed->mode != ED_EDIT) {
        A3Vec2 cp = a3_ui_cursor_pos(ui);
        A3Rect banner = a3_rect(cp.x, cp.y, a3_ui_content_width(ui), 26);
        a3_ui_rect(ui, banner, a3_color_alpha(col(ui, A3_UIC_WARNING), 0.25f), 6);
        a3_ui_text_in_rect(ui, A3_FONT_UI, banner, A3_ALIGN_CENTER, col(ui, A3_UIC_TEXT), "Playing: changes here are undone when you stop");
        a3_ui_set_cursor_pos(ui, a3_v2(cp.x, cp.y + 32));
    }
    /* header: active toggle + name */
    b32 active = (rec->flags & A3_ENTITY_ACTIVE) != 0;
    if (a3_ui_toggle(ui, "active", &active)) { a3_entity_set_active(w, e, active); ed_undo_mark_changed(ed, "Toggle Active"); }
    a3_ui_tooltip(ui, "Inactive objects are hidden and skipped by all systems");
    a3_ui_same_line(ui);
    char name[A3_NAME_MAX];
    a3_strcpy(name, sizeof(name), rec->name);
    if (a3_ui_input_text(ui, "name", name, sizeof(name), 0, "Name")) { a3_entity_set_name(w, e, name); ed_undo_mark_changed(ed, "Rename"); }
    if (!beginner(ed)) {
        b32 is_static = (rec->flags & A3_ENTITY_STATIC) != 0;
        if (a3_ui_checkbox(ui, "Static", &is_static)) {
            if (is_static) rec->flags |= A3_ENTITY_STATIC; else rec->flags &= ~(u32)A3_ENTITY_STATIC;
            ed_undo_mark_changed(ed, "Static");
        }
        a3_ui_tooltip(ui, "Static objects never move, which lets the engine batch and bake them");
        a3_ui_same_line(ui);
        char g[17];
        a3_guid_to_string(rec->guid, g);
        a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DISABLED), "id %s", g);
        if (rec->prefab[0]) a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "Prefab: %s", rec->prefab);
    }
    a3_ui_spacing(ui, 4);
    /* components */
    u32 remove_id = 0xFFFFFFFFu;
    for (u32 id = 0; id < A3_MAX_COMPONENT_TYPES; ++id) {
        A3ComponentType *t = a3_component_type(id);
        if (!t || !t->name[0]) continue;
        void *data = a3_component_get(w, e, id);
        if (!data) continue;
        a3_ui_push_id_int(ui, (i64)id);
        u32 icon = id == A3_T_TRANSFORM ? A3_ICON_MOVE_H : id == A3_T_LIGHT ? A3_ICON_SUN : id == A3_T_CAMERA ? A3_ICON_TARGET
                 : id == A3_T_MESH_RENDERER ? A3_ICON_CUBE : (t->flags & A3_COMP_CUSTOM) ? A3_ICON_PENCIL : A3_ICON_GEAR;
        b32 open = a3_ui_collapsing_header(ui, "hdr", t->name, icon, 1);
        if (t->doc) a3_ui_tooltip(ui, t->doc);
        if (id != A3_T_TRANSFORM && a3_ui_begin_context_menu(ui, "comp_ctx")) {
            if (a3_ui_menu_item(ui, "Remove Component", 0, 1)) remove_id = id;
            if (a3_ui_menu_item(ui, "Reset to Defaults", 0, t->defaults != 0)) { a3_memcpy(data, t->defaults, t->size); ed_undo_mark_changed(ed, "Reset Component"); }
            a3_ui_end_popup(ui);
        }
        if (open) {
            if (beginner(ed) && t->doc) {
                f32 wdt = a3_ui_content_width(ui);
                A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt, t->doc));
                a3_ui_text_wrapped(ui, A3_FONT_UI, tr, col(ui, A3_UIC_TEXT_DIM), t->doc);
            }
            u32 hidden_adv = 0;
            for (u32 fi = 0; fi < t->field_count; ++fi) {
                const A3FieldDesc *f = &t->fields[fi];
                if (f->flags & A3_FIELD_FLAG_HIDDEN) continue;
                if (beginner(ed) && (f->flags & A3_FIELD_FLAG_ADVANCED)) { hidden_adv++; continue; }
                ed_draw_field(ed, ui, f, data, t->name);
            }
            if (hidden_adv) a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DISABLED), "%u advanced settings hidden (switch to Advanced Mode)", hidden_adv);
            if (id == A3_T_PARTICLE_EMITTER) {
                if (a3_ui_button_ex(ui, "Restart", 90, A3_BUTTON_SMALL)) a3_particles_restart(w, e);
                a3_ui_tooltip(ui, "Replay the effect from the beginning (bursts, timed effects)");
                a3_ui_same_line(ui);
                if (a3_ui_button_ex(ui, "Presets...", 100, A3_BUTTON_SMALL)) a3_ui_open_popup(ui, "fx_presets");
                if (a3_ui_begin_popup(ui, "fx_presets", 180)) {
                    for (u32 p = 0; p < A3_PARTICLES_PRESET_COUNT; ++p)
                        if (a3_ui_menu_item(ui, a3_particle_preset_names[p], 0, 1)) {
                            a3_particles_preset((A3CParticleEmitter *)data, p);
                            a3_particles_restart(w, e);
                            ed_undo_mark_changed(ed, "Particle Preset");
                        }
                    a3_ui_end_popup(ui);
                }
                a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "%u particles alive", a3_particles_count(w));
            }
            if (id == A3_T_AUDIO_SOURCE) {
                if (a3_ui_button_ex(ui, "\xE2\x96\xB6 Preview Sound", 0, A3_BUTTON_SMALL)) {
                    if (!a3_audio_preview_source((A3CAudioSource *)data)) a3_ui_notify(ui, col(ui, A3_UIC_WARNING), "No sound selected (pick a Built-in Sound or a .wav Clip)");
                    else if (!a3_audio_device_ok()) a3_ui_notify(ui, col(ui, A3_UIC_WARNING), "No audio device found - connect speakers or headphones");
                }
                a3_ui_tooltip(ui, "Plays the sound once, without 3D effects");
            }
            if (id == A3_T_SCRIPT) script_inspector_extras(ed, ui, e, (A3CScript *)data);
            if (id != A3_T_TRANSFORM && ed->mode == ED_EDIT) {
                if (a3_ui_button_ex(ui, "Remove", 0, A3_BUTTON_SMALL | A3_BUTTON_FLAT)) remove_id = id;
            }
            a3_ui_spacing(ui, 4);
        }
        a3_ui_pop_id(ui);
    }
    if (rec->unknown_components) {
        a3_ui_label_colored(ui, col(ui, A3_UIC_WARNING), "%s Some components come from a plugin that is not loaded.", "\xE2\x9A\xA0");
        a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "They are kept unchanged and saved back.");
    }
    if (remove_id != 0xFFFFFFFFu) {
        A3ComponentType *t = a3_component_type(remove_id);
        a3_component_remove(w, e, remove_id);
        ed_undo_mark_changed(ed, "Remove Component");
        A3_INFO("editor", "removed %s from %s", t ? t->name : "component", rec->name);
    }
    contextual_tips(ed, ui, w, e);
    a3_ui_spacing(ui, 8);
    if (a3_ui_button_ex(ui, "Add Component", 0, A3_BUTTON_PRIMARY)) { ed->add_component_filter[0] = 0; a3_ui_open_popup(ui, "add_component"); }
    add_component_popup(ed, ui, w, e);
}

/* ======================================================================== */
/* Console                                                                  */
/* ======================================================================== */

static b32 log_visible(A3Editor *ed, const A3LogEntry *le) {
    if (le->level <= A3_LOG_INFO && !ed->log_filter.info) return 0;
    if (le->level == A3_LOG_WARN && !ed->log_filter.warn) return 0;
    if (le->level >= A3_LOG_ERROR && !ed->log_filter.error) return 0;
    if (le->level < A3_LOG_INFO && ed->level == ED_LEVEL_BEGINNER) return 0;
    if (ed->log_filter.text[0] && !a3_stristr(le->message, ed->log_filter.text) && !a3_stristr(le->category, ed->log_filter.text)) return 0;
    return 1;
}

void ed_console_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    u32 n = a3_log_count(), ni = 0, nw = 0, ne = 0;
    for (u32 i = 0; i < n; ++i) {
        const A3LogEntry *le = a3_log_get(i);
        if (le->level >= A3_LOG_ERROR) ne++; else if (le->level == A3_LOG_WARN) nw++; else if (le->level >= A3_LOG_INFO) ni++;
    }
    char b[48];
    a3_snprintf(b, sizeof(b), "Info %u", ni);
    if (a3_ui_button_ex(ui, b, 84, A3_BUTTON_SMALL | (ed->log_filter.info ? A3_BUTTON_TOGGLED : 0))) ed->log_filter.info = !ed->log_filter.info;
    a3_ui_same_line(ui);
    a3_snprintf(b, sizeof(b), "Warnings %u", nw);
    if (a3_ui_button_ex(ui, b, 110, A3_BUTTON_SMALL | (ed->log_filter.warn ? A3_BUTTON_TOGGLED : 0))) ed->log_filter.warn = !ed->log_filter.warn;
    a3_ui_same_line(ui);
    a3_snprintf(b, sizeof(b), "Errors %u", ne);
    if (a3_ui_button_ex(ui, b, 94, A3_BUTTON_SMALL | (ed->log_filter.error ? A3_BUTTON_TOGGLED : 0))) ed->log_filter.error = !ed->log_filter.error;
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Clear", 70, A3_BUTTON_SMALL)) a3_log_clear();
    a3_ui_same_line(ui);
    a3_ui_checkbox(ui, "Auto-scroll", &ed->console_autoscroll);
    a3_ui_same_line(ui);
    a3_ui_input_text(ui, "log_search", ed->log_filter.text, sizeof(ed->log_filter.text), A3_INPUT_SEARCH, "Filter...");
    A3Vec2 cp = a3_ui_cursor_pos(ui);
    A3Rect list = a3_rect(r.x, cp.y, r.w, r.y + r.h - cp.y);
    if (a3_ui_begin_panel(ui, "log_list", list, A3_PANEL_BORDER)) {
        u32 total = a3_log_count();
        b32 new_entries = total != ed->log_seen;
        ed->log_seen = total;
        A3Rect clip = a3_ui_clip(ui);
        for (u32 i = 0; i < total; ++i) {
            const A3LogEntry *le = a3_log_get(i);
            if (!log_visible(ed, le)) continue;
            b32 has_hint = le->hint[0] != 0;
            A3Rect row = a3_ui_next_rect(ui, 0, has_hint ? 38 : 20);
            if (row.y + row.h < clip.y || row.y > clip.y + clip.h) continue; /* off screen */
            u32 c = le->level >= A3_LOG_ERROR ? col(ui, A3_UIC_ERROR) : le->level == A3_LOG_WARN ? col(ui, A3_UIC_WARNING)
                  : le->level < A3_LOG_INFO ? col(ui, A3_UIC_TEXT_DISABLED) : col(ui, A3_UIC_TEXT);
            if (le->level >= A3_LOG_WARN) a3_ui_rect(ui, row, a3_color_alpha(c, 0.08f), 3);
            u32 icon = le->level >= A3_LOG_ERROR ? A3_ICON_CROSS : le->level == A3_LOG_WARN ? A3_ICON_WARNING : A3_ICON_BULLET;
            a3_ui_icon(ui, icon, a3_v2(row.x + 9, row.y + 10), c, A3_FONT_UI);
            char head[40];
            a3_snprintf(head, sizeof(head), "[%s]", le->category);
            a3_ui_text(ui, A3_FONT_MONO, a3_v2(row.x + 20, row.y + 2), col(ui, A3_UIC_TEXT_DIM), head);
            a3_ui_text(ui, A3_FONT_UI, a3_v2(row.x + 104, row.y + 1), c, le->message);
            if (has_hint) a3_ui_text(ui, A3_FONT_UI, a3_v2(row.x + 104, row.y + 19), col(ui, A3_UIC_TEXT_DIM), le->hint);
        }
        if (ed->console_autoscroll && new_entries) a3_ui_scroll_here(ui);
    }
    a3_ui_end_panel(ui);
}

/* ======================================================================== */
/* Assets                                                                   */
/* ======================================================================== */

typedef struct AssetEntry { char name[128]; b32 is_dir; u64 size; } AssetEntry;
static struct {
    AssetEntry items[512];
    u32 count;
    char dir[ED_PATH];
    f64 listed_at;
    char project[ED_PATH];
} g_assets;

static b32 asset_visit(const char *dir, const A3DirEntry *e, void *user) {
    A3_UNUSED(dir); A3_UNUSED(user);
    if (e->name[0] == '.') return 1;
    if (g_assets.count >= A3_ARRAY_COUNT(g_assets.items)) return 0;
    AssetEntry *a = &g_assets.items[g_assets.count++];
    a3_strcpy(a->name, sizeof(a->name), e->name);
    a->is_dir = e->is_dir;
    a->size = e->size;
    return 1;
}

static void assets_refresh(A3Editor *ed, b32 force) {
    f64 now = a3_time_seconds();
    if (!force && a3_streq(g_assets.dir, ed->asset_dir) && a3_streq(g_assets.project, ed->project_dir) && now - g_assets.listed_at < 1.0) return;
    g_assets.count = 0;
    a3_strcpy(g_assets.dir, sizeof(g_assets.dir), ed->asset_dir);
    a3_strcpy(g_assets.project, sizeof(g_assets.project), ed->project_dir);
    g_assets.listed_at = now;
    char abs[ED_PATH];
    ed_project_path(ed, ed->asset_dir, abs, sizeof(abs));
    a3_dir_list(abs, asset_visit, 0);
    /* folders first */
    for (u32 i = 1; i < g_assets.count; ++i) {
        AssetEntry t = g_assets.items[i];
        u32 j = i;
        while (j > 0 && !g_assets.items[j - 1].is_dir && t.is_dir) { g_assets.items[j] = g_assets.items[j - 1]; --j; }
        g_assets.items[j] = t;
    }
}

static u32 file_icon(const char *name, u32 *color, A3Ui *ui) {
    const char *ext = a3_path_extension(name);
    *color = col(ui, A3_UIC_TEXT);
    if (a3_streq(ext, ".obj")) { *color = col(ui, A3_UIC_AXIS_Z); return A3_ICON_CUBE; }
    if (a3_streq(ext, ".a3scene")) { *color = col(ui, A3_UIC_SUCCESS); return A3_ICON_HOME; }
    if (a3_streq(ext, ".a3shader") || a3_streq(ext, ".a3mat") || a3_streq(ext, ".glsl")) { *color = col(ui, A3_UIC_ACCENT); return A3_ICON_DIAMOND; }
    if (a3_streq(ext, ".wav") || a3_streq(ext, ".ogg")) return A3_ICON_MUSIC;
    if (a3_streq(ext, ".a3script") || a3_streq(ext, ".txt") || a3_streq(ext, ".json") || a3_streq(ext, ".md")) { *color = col(ui, A3_UIC_WARNING); return A3_ICON_PENCIL; }
    if (a3_streq(ext, ".a3comp")) return A3_ICON_GEAR;
    return A3_ICON_SQUARE_O;
}

static b32 is_image(const char *name) {
    const char *ext = a3_path_extension(name);
    return a3_streq(ext, ".png") || a3_streq(ext, ".tga") || a3_streq(ext, ".bmp") || a3_streq(ext, ".ppm");
}

static b32 is_text(const char *name) {
    const char *ext = a3_path_extension(name);
    return a3_streq(ext, ".a3script") || a3_streq(ext, ".txt") || a3_streq(ext, ".json") || a3_streq(ext, ".md") || a3_streq(ext, ".glsl")
        || a3_streq(ext, ".a3comp") || a3_streq(ext, ".a3mat") || a3_streq(ext, ".a3anim");
}

static void asset_open(A3Editor *ed, const char *rel) {
    const char *ext = a3_path_extension(rel);
    if (a3_streq(ext, ".a3scene")) {
        if (ed->mode != ED_EDIT) ed_stop(ed);
        if (ed->dirty) ed_scene_save(ed);
        ed_scene_open(ed, rel);
    } else if (a3_streq(ext, ".a3shader")) {
        if (ed_shader_open(ed, rel)) a3_dock_show(&ed->dock, "Shader Maker");
    } else if (a3_streq(ext, ".wav")) {
        u32 clip = a3_audio_clip(rel);
        if (clip) a3_audio_play(clip, 1.0f, 1.0f, 0);
        a3_ui_notify(ed->ui, 0, clip ? "Playing %s (%.1f s)" : "Could not play %s", a3_path_filename(rel), (f64)a3_audio_clip_seconds(clip));
    } else if (is_text(rel)) {
        if (ed_code_open(ed, rel) >= 0) a3_dock_show(&ed->dock, "Code");
    } else {
        a3_ui_notify(ed->ui, 0, "Drag %s into the scene or onto a field", a3_path_filename(rel));
    }
}

static void new_file(A3Editor *ed, const char *base, const char *ext, const char *content) {
    char rel[ED_PATH], abs[ED_PATH];
    for (int i = 0; i < 100; ++i) {
        if (i == 0) a3_snprintf(rel, sizeof(rel), "%s/%s%s", ed->asset_dir, base, ext);
        else a3_snprintf(rel, sizeof(rel), "%s/%s %d%s", ed->asset_dir, base, i + 1, ext);
        ed_project_path(ed, rel, abs, sizeof(abs));
        if (!a3_file_exists(abs)) break;
    }
    if (a3_file_write_atomic(abs, content, a3_strlen(content)) == A3_OK) {
        A3_INFO("editor", "created %s", rel);
        assets_refresh(ed, 1);
        if (is_text(rel)) { if (ed_code_open(ed, rel) >= 0) a3_dock_show(&ed->dock, "Code"); }
    }
}

void ed_assets_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    if (!ed->has_project) return;
    if (!ed->asset_dir[0]) a3_strcpy(ed->asset_dir, sizeof(ed->asset_dir), "Assets");
    assets_refresh(ed, 0);
    /* toolbar: breadcrumb + actions */
    if (a3_ui_icon_button(ui, A3_ICON_ARROW_UP, "Parent folder", a3_streq(ed->asset_dir, "Assets") ? A3_BUTTON_DISABLED : 0)) {
        char parent[ED_PATH];
        a3_path_dirname(ed->asset_dir, parent, sizeof(parent));
        if (parent[0]) a3_strcpy(ed->asset_dir, sizeof(ed->asset_dir), parent);
    }
    a3_ui_same_line(ui);
    a3_ui_label(ui, "%s", ed->asset_dir);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "+ New", 70, A3_BUTTON_SMALL)) a3_ui_open_popup(ui, "asset_new");
    if (a3_ui_begin_popup(ui, "asset_new", 200)) {
        if (a3_ui_menu_item(ui, "Folder", 0, 1)) {
            char rel[ED_PATH], abs[ED_PATH];
            a3_snprintf(rel, sizeof(rel), "%s/New Folder", ed->asset_dir);
            ed_project_path(ed, rel, abs, sizeof(abs));
            a3_dir_create(abs);
            assets_refresh(ed, 1);
        }
        if (a3_ui_menu_item(ui, "Script", 0, 1)) new_file(ed, "NewScript", ".a3script", ed_script_template());
        if (a3_ui_menu_item(ui, "Shader Graph", 0, 1)) { ed_shader_new(ed); a3_dock_show(&ed->dock, "Shader Maker"); }
        if (a3_ui_menu_item(ui, "Text Note", 0, 1)) new_file(ed, "Notes", ".txt", "");
        if (a3_ui_menu_item(ui, "Scene", 0, 1)) {
            char rel[ED_PATH], abs[ED_PATH];
            a3_snprintf(rel, sizeof(rel), "%s/New Scene.a3scene", ed->asset_dir);
            ed_project_path(ed, rel, abs, sizeof(abs));
            A3World *w = a3_world_create("New Scene");
            ed_build_template_scene(w, 0);
            a3_scene_save_file(w, abs, 0);
            a3_world_destroy(w);
            assets_refresh(ed, 1);
        }
        a3_ui_end_popup(ui);
    }
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_REFRESH, "Refresh", A3_BUTTON_SMALL)) assets_refresh(ed, 1);
    a3_ui_same_line(ui);
    a3_ui_input_text(ui, "asset_search", ed->asset_filter, sizeof(ed->asset_filter), A3_INPUT_SEARCH, "Search this folder...");
    A3Vec2 cp = a3_ui_cursor_pos(ui);
    A3Rect area = a3_rect(r.x, cp.y, r.w, r.y + r.h - cp.y);
    char open_dir[ED_PATH] = { 0 }, open_file[ED_PATH] = { 0 }, trash[ED_PATH] = { 0 };
    if (a3_ui_begin_panel(ui, "asset_grid", area, A3_PANEL_BORDER)) {
        A3Rect pr = a3_ui_panel_rect(ui);
        f32 cell = 92, gap = 8;
        i32 cols = a3_maxi((i32)((pr.w + gap) / (cell + gap)), 1);
        i32 k = 0;
        A3Vec2 start = a3_ui_cursor_pos(ui);
        for (u32 i = 0; i < g_assets.count; ++i) {
            AssetEntry *a = &g_assets.items[i];
            if (ed->asset_filter[0] && !a3_stristr(a->name, ed->asset_filter)) continue;
            char rel[ED_PATH];
            a3_snprintf(rel, sizeof(rel), "%s/%s", ed->asset_dir, a->name);
            A3Rect c = a3_rect(start.x + (f32)(k % cols) * (cell + gap), start.y + (f32)(k / cols) * (cell + 22 + gap), cell, cell + 22);
            ++k;
            a3_ui_push_id(ui, a->name);
            b32 clicked = a3_ui_invisible_button(ui, "cell", c);
            b32 hov = a3_ui_item_hovered(ui);
            if (hov) a3_ui_rect(ui, c, col(ui, A3_UIC_WIDGET_HOVER), 8);
            A3Rect thumb = a3_rect(c.x + 10, c.y + 6, cell - 20, cell - 20);
            if (a->is_dir) {
                a3_ui_draw_icon_folder(ui, a3_v2(thumb.x + thumb.w * 0.5f, thumb.y + thumb.h * 0.5f), thumb.w * 0.7f, col(ui, A3_UIC_WARNING));
                if (a3_ui_item_double_clicked(ui)) a3_strcpy(open_dir, sizeof(open_dir), rel);
                u32 sz;
                const char *dropped = (const char *)a3_ui_drop_target(ui, "asset", &sz);
                if (dropped) {
                    char src[ED_PATH], dst[ED_PATH], drel[ED_PATH];
                    ed_project_path(ed, dropped, src, sizeof(src));
                    a3_snprintf(drel, sizeof(drel), "%s/%s", rel, a3_path_filename(dropped));
                    ed_project_path(ed, drel, dst, sizeof(dst));
                    if (a3_file_move(src, dst) == A3_OK) A3_INFO("editor", "moved %s to %s", dropped, rel);
                    assets_refresh(ed, 1);
                }
            } else {
                if (is_image(a->name)) {
                    u32 tex = a3_assets_texture(rel);
                    a3_ui_rect(ui, thumb, col(ui, A3_UIC_PANEL_ALT), 6);
                    a3_ui_image(ui, a3_assets_texture_rhi(tex), a3_rect_shrink(thumb, 3), a3_v2(0, 0), a3_v2(1, 1), 0xFFFFFFFFu);
                } else {
                    u32 ic;
                    u32 icon = file_icon(a->name, &ic, ui);
                    a3_ui_rect(ui, thumb, col(ui, A3_UIC_PANEL_ALT), 10);
                    a3_ui_icon(ui, icon, a3_v2(thumb.x + thumb.w * 0.5f, thumb.y + thumb.h * 0.5f), ic, A3_FONT_TITLE);
                }
                a3_ui_drag_source(ui, "asset", rel, (u32)a3_strlen(rel) + 1, a->name);
                if (a3_ui_item_double_clicked(ui)) a3_strcpy(open_file, sizeof(open_file), rel);
            }
            A3_UNUSED(clicked);
            char tip[ED_PATH + 64];
            if (a->is_dir) a3_snprintf(tip, sizeof(tip), "%s (double-click to open)", rel);
            else a3_snprintf(tip, sizeof(tip), "%s  -  %.1f KB\nDrag into the scene or onto a field. Double-click to open.", rel, (f64)a->size / 1024.0);
            a3_ui_tooltip(ui, tip);
            if (a3_ui_begin_context_menu(ui, "asset_ctx")) {
                if (a3_ui_menu_item(ui, "Open", 0, 1)) { if (a->is_dir) a3_strcpy(open_dir, sizeof(open_dir), rel); else a3_strcpy(open_file, sizeof(open_file), rel); }
                if (a3_ui_menu_item(ui, "Move to Trash", 0, 1)) a3_strcpy(trash, sizeof(trash), rel);
                a3_ui_end_popup(ui);
            }
            a3_ui_push_clip(ui, c);
            a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(c.x + 2, c.y + cell - 10, c.w - 4, 26), A3_ALIGN_CENTER, col(ui, A3_UIC_TEXT), a->name);
            a3_ui_pop_clip(ui);
            a3_ui_pop_id(ui);
        }
        i32 rows = (k + cols - 1) / cols;
        a3_ui_set_cursor_pos(ui, a3_v2(start.x, start.y + (f32)rows * (cell + 22 + gap)));
        a3_ui_next_rect(ui, 0, 4);
        if (k == 0) a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), ed->asset_filter[0] ? "No matches." : "This folder is empty. Copy files into it or use + New.");
    }
    a3_ui_end_panel(ui);
    if (open_dir[0]) a3_strcpy(ed->asset_dir, sizeof(ed->asset_dir), open_dir);
    if (open_file[0]) asset_open(ed, open_file);
    if (trash[0]) {
        /* never delete: move into the project trash so mistakes are recoverable */
        char src[ED_PATH], dst[ED_PATH], tdir[ED_PATH], name[ED_PATH];
        ed_project_path(ed, trash, src, sizeof(src));
        ed_project_path(ed, ".asm3d/trash", tdir, sizeof(tdir));
        a3_dir_create(tdir);
        a3_snprintf(name, sizeof(name), "%llu_%s", (unsigned long long)a3_wall_clock_unix(), a3_path_filename(trash));
        a3_path_join(dst, sizeof(dst), tdir, name);
        if (a3_file_move(src, dst) == A3_OK) a3_ui_notify(ui, 0, "Moved %s to .asm3d/trash", a3_path_filename(trash));
        assets_refresh(ed, 1);
    }
}

/* ======================================================================== */
/* Profiler                                                                 */
/* ======================================================================== */

static const char *const phase_names[] = { "Early", "Fixed", "Update", "Late" };

void ed_profiler_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    A3_UNUSED(r);
    f32 mx = 0;
    for (u32 i = 0; i < A3_ARRAY_COUNT(ed->frame_ms); ++i) mx = a3_maxf(mx, ed->frame_ms[i]);
    a3_ui_label(ui, "%.1f FPS   frame %.2f ms   worst %.2f ms", (f64)ed->fps, (f64)(ed->fps > 0 ? 1000.0f / ed->fps : 0), (f64)mx);
    a3_ui_plot_lines(ui, "Frame time (ms)", ed->frame_ms, A3_ARRAY_COUNT(ed->frame_ms), ed->frame_ms_pos, 0, a3_maxf(mx * 1.2f, 20), 70, col(ui, A3_UIC_SUCCESS));
    if (a3_ui_collapsing_header(ui, "prof_render", "Rendering", A3_ICON_CUBE, 1)) {
        const A3RenderFrameInfo *fi = a3_renderer_frame_info(a3_engine_renderer(ed->engine));
        const A3RhiStats *rs = a3_rhi_stats();
        a3_ui_property(ui, "Renderer CPU", 0); a3_ui_label(ui, "%.2f ms", fi->cpu_ms);
        a3_ui_property(ui, "Objects", "Visible after frustum culling (SIMD assembly kernel)"); a3_ui_label(ui, "%u visible / %u total (%u culled)", fi->visible, fi->renderables, fi->culled);
        a3_ui_property(ui, "Batches", "Instanced draw batches"); a3_ui_label(ui, "%u", fi->batches);
        a3_ui_property(ui, "Draw calls", 0); a3_ui_label(ui, "%u", rs->draw_calls);
        a3_ui_property(ui, "Triangles", 0); a3_ui_label(ui, "%u", rs->triangles);
        a3_ui_property(ui, "Lights", 0); a3_ui_label(ui, "%u (%u shadow casters)", fi->lights, fi->shadow_casters);
        a3_ui_property(ui, "Particles", "Live particles drawn this frame (SSE assembly simulation)"); a3_ui_label(ui, "%u", fi->particles);
        a3_ui_property(ui, "UI vertices", 0); a3_ui_label(ui, "%u", a3_ui_vertex_count(ui));
    }
    if (a3_ui_collapsing_header(ui, "prof_sys", "Systems (last frame)", A3_ICON_BOLT, 1)) {
        for (u32 i = 0; i < a3_systems_count(); ++i) {
            const A3SystemDesc *s = a3_systems_get(i);
            char lbl[96];
            a3_snprintf(lbl, sizeof(lbl), "%s", s->name ? s->name : "system");
            a3_ui_property(ui, lbl, 0);
            a3_ui_label(ui, "%.3f ms  (%s)", a3_systems_time_ms(i), phase_names[s->phase < A3_PHASE_COUNT ? s->phase : 0]);
        }
    }
    A3World *w = ed_active_world(ed);
    if (w && a3_ui_collapsing_header(ui, "prof_phys", "Physics", A3_ICON_BOX, 1)) {
        const A3PhysicsStats *ps = a3_physics_stats(w);
        if (ps) {
            a3_ui_property(ui, "Bodies", 0); a3_ui_label(ui, "%u (%u dynamic, %u sleeping)", ps->bodies, ps->dynamic_bodies, ps->sleeping_bodies);
            a3_ui_property(ui, "Pairs / contacts", 0); a3_ui_label(ui, "%u / %u", ps->broadphase_pairs, ps->contacts);
            a3_ui_property(ui, "Step", "Integration, broadphase, narrowphase and solver"); a3_ui_label(ui, "%.3f ms", ps->step_ms);
            a3_ui_property(ui, "  Broadphase", "Sweep and prune (assembly)"); a3_ui_label(ui, "%.3f ms", ps->broadphase_ms);
            a3_ui_property(ui, "  Narrowphase", 0); a3_ui_label(ui, "%.3f ms", ps->narrowphase_ms);
            a3_ui_property(ui, "  Solver", "Sequential impulses (assembly)"); a3_ui_label(ui, "%.3f ms (%u rows)", ps->solver_ms, ps->solver_rows);
        }
    }
    if (a3_ui_collapsing_header(ui, "prof_audio", "Audio", A3_ICON_MUSIC, 0)) {
        a3_ui_property(ui, "Device", 0); a3_ui_label(ui, "%s", a3_audio_device_name());
        a3_ui_property(ui, "Sample Rate", 0); a3_ui_label(ui, "%u Hz", a3_audio_sample_rate());
        a3_ui_property(ui, "Playing", "Sounds playing right now (max 64)"); a3_ui_label(ui, "%u voices", a3_audio_active_voices());
    }
    if (a3_ui_collapsing_header(ui, "prof_mem", "Memory", A3_ICON_HEX, ed->level != ED_LEVEL_BEGINNER)) {
        A3MemStats ms;
        a3_mem_get_stats(&ms);
        a3_ui_property(ui, "OS reserved", 0); a3_ui_label(ui, "%.1f MB", (f64)ms.os_reserved_bytes / (1024.0 * 1024.0));
        for (u32 t = 0; t < A3_MEM_TAG_COUNT; ++t) {
            if (!ms.tags[t].peak_bytes) continue;
            a3_ui_property(ui, a3_mem_tag_name((A3MemTag)t), 0);
            a3_ui_label(ui, "%.2f MB (peak %.2f)  %llu live", (f64)ms.tags[t].current_bytes / (1024.0 * 1024.0), (f64)ms.tags[t].peak_bytes / (1024.0 * 1024.0), (unsigned long long)ms.tags[t].live_allocs);
        }
        if (ms.invalid_frees) a3_ui_label_colored(ui, col(ui, A3_UIC_ERROR), "%llu invalid frees detected", (unsigned long long)ms.invalid_frees);
    }
}

/* ======================================================================== */
/* Docs                                                                     */
/* ======================================================================== */

typedef struct DocTopic { const char *title; const char *text; } DocTopic;
static const DocTopic g_topics[] = {
    { "Getting Started",
      "Welcome to ASM3D!\n\n"
      "1. Press Play (F5) to try your game. Press F5 again to stop - the scene goes back to how it was.\n"
      "2. Move around the viewport: hold the right mouse button and use W A S D (Q and E go down and up). Scroll to change speed.\n"
      "3. Click an object to select it. Drag the red, green and blue arrows to move it. Press E to rotate and R to scale.\n"
      "4. Use + Add in the toolbar to place shapes, lights, cameras and a player.\n"
      "5. The Inspector on the right shows the selected object's components. Every setting has a tooltip.\n"
      "6. Ctrl+Z undoes anything. Your work is autosaved every 30 seconds.\n"
      "7. When you are happy, open Build and press Build Game to make a program you can share." },
    { "Objects and Components",
      "Everything in a scene is an object (an entity). Objects have components that give them abilities:\n\n"
      "Transform - position, rotation and size.\nMesh Renderer - makes it visible.\nCollider - makes it solid.\n"
      "RigidBody - makes it fall and get pushed.\nLight - lights the scene.\nCamera - what the player sees.\n"
      "CharacterController - a walking player with a camera.\n\n"
      "Drag objects onto each other in the Hierarchy to make one a child of another: children move with their parent." },
    { "Physics",
      "ASM3D has built-in rigid body physics. The hot loops - integration, broadphase (sweep and prune), bounding boxes, the constraint "
      "solver and ray casts - are hand-written x86-64 SSE assembly with bit-identical C reference versions used on other CPUs.\n\n"
      "To make something fall: add a Collider and a RigidBody. Objects with only a Collider are static (walls, floors).\n"
      "Mark a Collider as a Trigger to detect overlaps without collisions (pickups, doors).\n"
      "Mass, friction and bounciness (restitution) are on the RigidBody. Resting objects go to sleep to save time." },
    { "Shader Maker",
      "The Shader Maker builds materials by connecting nodes instead of writing code.\n\n"
      "Add nodes with right-click, drag from an output (right side) to an input (left side) to connect them. The graph compiles to a "
      "GLSL surface function automatically and the preview updates live. If something is wrong, the node with the problem is outlined in red "
      "and the Console explains why. Apply the material to the selected object with 'Apply to Selection'." },
    { "Building Your Game",
      "Open the Build panel (Ctrl+B builds immediately).\n\n"
      "Check Project looks for common problems first: missing assets, scenes without a camera, lights without shadows, very large textures.\n"
      "Build Game creates a folder in Builds/ containing the game program and a data folder with your project. Zip that folder to share it.\n\n"
      "Builds are made for the system the editor runs on: Windows (x64) or Linux (x86-64). macOS is planned; see docs/STATUS.md." },
    { "Code Editor",
      "The Code panel edits scripts, shaders and text files with syntax highlighting, line numbers, undo, find and replace.\n"
      "Ctrl+S saves, Ctrl+F finds, Ctrl+Z / Ctrl+Y undo and redo, Tab indents.\n\n"
      "Scripts (.a3script) are checked while you type: a red dot in the margin marks the line with a problem; hover it to read the "
      "explanation. Saving a script while the game is playing reloads it on every object that uses it, keeping their variables." },
    { "Scripting",
      "A3Script is the ASM3D scripting language. Add a Script component to an object and press 'Create New Script'.\n\n"
      "let speed = 3                  // variables written at the top belong to this object\n"
      "fn on_start() { }              // runs once when the game starts\n"
      "fn on_update(dt) {             // runs every frame; dt = seconds since the last frame\n"
      "    if key_down(\"space\") { self.position += vec3(0, speed * dt, 0) }\n"
      "}\n"
      "fn on_trigger_enter(other) { } // another object entered this object's trigger collider\n"
      "fn on_collision(other) { }     // this object hit something\n"
      "fn on_fixed_update(dt) { }     // every physics step\n\n"
      "Values: numbers (3, 2.5), text (\"hi\"), true/false, nil, lists ([1, 2, 3]), vec3(x, y, z) and objects.\n"
      "Control: if / else if / else, while, for i in 0..10, for item in list, break, continue, return, and / or / not.\n"
      "Objects: self is the object running the script. obj.position, obj.rotation (degrees), obj.scale, obj.name, obj.active and "
      "obj.velocity are shortcuts. Any component field works too: self.Light.intensity = 2, self.RigidBody.mass = 5. "
      "You can even read another object's script variables: find(\"Player\").health.\n\n"
      "Safety: a script that loops forever is stopped after a few million steps instead of freezing the game. When a script has an "
      "error, the Console shows the file, line and a plain explanation (often with a 'did you mean' suggestion); only that object's "
      "script stops, and fixing the file restarts it.\n\n"
      "Every built-in function is listed under Script API below." },
    { "Modeling",
      "Edit Mode changes the shape of an object, polygon by polygon, like in dedicated 3D modeling programs.\n\n"
      "Select an object and press Tab (or Modeling > Edit Selected Mesh). Primitives become a model file in Assets/Models.\n"
      "1 / 2 / 3: vertex, edge or face selection. Click to select, Shift+click to add, drag for a box, double-click an edge for its loop. "
      "A selects all, Alt+A nothing, L connected parts, Ctrl+I inverts.\n"
      "G move, R rotate, S scale: move the mouse, press X, Y or Z to lock an axis, hold Ctrl to snap, click to confirm, right click to cancel.\n"
      "E extrude the selected faces (then move the mouse), I inset, Ctrl+R loop cut (wheel for more cuts), Ctrl+2 smooth subdivision, "
      "X delete, F fill a hole, M merge close vertices, Shift+D duplicate, Shift+N fix normals, Alt+Z X-ray.\n"
      "Ctrl+Z undoes mesh edits while in Edit Mode. Tab saves the model and returns to the scene.\n\n"
      "The heavy math (moving thousands of vertices, normals, picking faces with the mouse) runs in hand-written x86-64 assembly. "
      "Not available yet: bevel, knife, booleans, UV unwrapping and sculpting. The same operations are scriptable with 'asm3d_cli mesh edit'." },
    { "Keyboard Shortcuts",
      "Ctrl+S save   Ctrl+Z undo   Ctrl+Y redo   Ctrl+D duplicate   Delete remove\n"
      "W move tool   E rotate tool   R scale tool   F focus   Ctrl while dragging: snap\n"
      "Right mouse + WASD/QE fly   Alt + left drag orbit   Middle drag pan\n"
      "F5 play/stop   F6 pause   F10 step one frame   Escape releases the mouse while playing\n"
      "Ctrl+P command palette   Ctrl+B build   F1 documentation" },
    { "What Works Today",
      "ASM3D is honest about its status. Working now: the editor, scenes, undo/redo, autosave and crash recovery, the PBR renderer "
      "with shadows, physics with assembly kernels, the character controller, audio (assembly mixer), keyframe animation, particles "
      "(assembly simulation), the A3Script scripting language with HUD drawing, polygon modeling (Edit Mode), the asm3d_cli command line, "
      "templates, the code editor, the Shader Maker and desktop builds for Windows and Linux.\n\n"
      "Not implemented yet: visual scripting, terrain and world streaming, AI navigation, vehicles, weather, skeletal animation, "
      "bevel/booleans/UV unwrapping in the modeler and macOS. docs/STATUS.md in the engine source lists exactly what is implemented, partial and planned." },
};

#define DOC_API_BASE 100000

/* Script API categories in registration order (from the natives table). */
static u32 api_categories(const char **out, u32 cap) {
    u32 n = 0;
    for (u32 i = 0; i < a3s_native_count(); ++i) {
        const char *c = a3s_native_get(i)->category;
        if (!c) continue;
        b32 seen = 0;
        for (u32 k = 0; k < n; ++k) if (a3_streq(out[k], c)) seen = 1;
        if (!seen && n < cap) out[n++] = c;
    }
    return n;
}

static b32 api_category_matches(const char *cat, const char *q) {
    if (a3_stristr(cat, q)) return 1;
    for (u32 i = 0; i < a3s_native_count(); ++i) {
        const A3SNative *n = a3s_native_get(i);
        if (n->category && a3_streq(n->category, cat) && (a3_stristr(n->name, q) || (n->doc && a3_stristr(n->doc, q)))) return 1;
    }
    return 0;
}

void ed_docs_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    static i32 topic = 0;       /* >= 0: guide topic, < 0: component -(id+1) */
    f32 list_w = a3_minf(230, r.w * 0.35f);
    if (a3_ui_begin_panel(ui, "doc_list", a3_rect(r.x, r.y, list_w, r.h), A3_PANEL_BORDER)) {
        a3_ui_input_text(ui, "doc_search", ed->search_query, sizeof(ed->search_query), A3_INPUT_SEARCH, "Search docs...");
        a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "Guides");
        for (u32 i = 0; i < A3_ARRAY_COUNT(g_topics); ++i) {
            if (ed->search_query[0] && !a3_stristr(g_topics[i].title, ed->search_query) && !a3_stristr(g_topics[i].text, ed->search_query)) continue;
            a3_ui_push_id_int(ui, (i64)i);
            if (a3_ui_selectable(ui, g_topics[i].title, topic == (i32)i)) topic = (i32)i;
            a3_ui_pop_id(ui);
        }
        a3_ui_spacing(ui, 6);
        a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "Script API");
        {
            const char *cats[16];
            u32 nc = api_categories(cats, 16);
            for (u32 c = 0; c < nc; ++c) {
                if (ed->search_query[0] && !api_category_matches(cats[c], ed->search_query)) continue;
                a3_ui_push_id_int(ui, 5000 + (i64)c);
                if (a3_ui_selectable(ui, cats[c], topic == DOC_API_BASE + (i32)c)) topic = DOC_API_BASE + (i32)c;
                a3_ui_pop_id(ui);
            }
        }
        a3_ui_spacing(ui, 6);
        a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "Component Reference");
        for (u32 id = 0; id < A3_MAX_COMPONENT_TYPES; ++id) {
            A3ComponentType *t = a3_component_type(id);
            if (!t || !t->name[0] || (t->flags & A3_COMP_HIDDEN)) continue;
            if (ed->search_query[0] && !a3_stristr(t->name, ed->search_query) && !(t->doc && a3_stristr(t->doc, ed->search_query))) continue;
            a3_ui_push_id_int(ui, 1000 + (i64)id);
            if (a3_ui_selectable(ui, t->name, topic == -(i32)(id + 1))) topic = -(i32)(id + 1);
            a3_ui_pop_id(ui);
        }
    }
    a3_ui_end_panel(ui);
    if (a3_ui_begin_panel(ui, "doc_body", a3_rect(r.x + list_w + 6, r.y, r.w - list_w - 6, r.h), 0)) {
        f32 wdt = a3_ui_content_width(ui);
        if (topic >= DOC_API_BASE) {
            const char *cats[16];
            u32 nc = api_categories(cats, 16);
            u32 c = (u32)(topic - DOC_API_BASE);
            if (c < nc) {
                a3_ui_label_font(ui, A3_FONT_HEADING, col(ui, A3_UIC_TEXT), cats[c]);
                for (u32 i = 0; i < a3s_native_count(); ++i) {
                    const A3SNative *n = a3s_native_get(i);
                    if (!n->category || !a3_streq(n->category, cats[c])) continue;
                    if (ed->search_query[0] && !a3_stristr(n->name, ed->search_query) && !(n->doc && a3_stristr(n->doc, ed->search_query))) continue;
                    a3_ui_label_font(ui, A3_FONT_MONO, col(ui, A3_UIC_ACCENT), n->signature ? n->signature : n->name);
                    if (n->doc) { A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt - 16, n->doc)); a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect(tr.x + 16, tr.y, tr.w - 16, tr.h), col(ui, A3_UIC_TEXT_DIM), n->doc); }
                    a3_ui_spacing(ui, 2);
                }
            }
        } else if (topic >= 0 && topic < (i32)A3_ARRAY_COUNT(g_topics)) {
            a3_ui_label_font(ui, A3_FONT_HEADING, col(ui, A3_UIC_TEXT), g_topics[topic].title);
            A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt, g_topics[topic].text));
            a3_ui_text_wrapped(ui, A3_FONT_UI, tr, col(ui, A3_UIC_TEXT), g_topics[topic].text);
        } else {
            A3ComponentType *t = a3_component_type((u32)(-topic - 1));
            if (t) {
                a3_ui_label_font(ui, A3_FONT_HEADING, col(ui, A3_UIC_TEXT), t->name);
                a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "Category: %s%s", t->category, (t->flags & A3_COMP_CUSTOM) ? "  (custom)" : "");
                if (t->doc) { A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt, t->doc)); a3_ui_text_wrapped(ui, A3_FONT_UI, tr, col(ui, A3_UIC_TEXT), t->doc); }
                for (u32 i = 0; i < 4; ++i) if (t->requires[i][0]) a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "Requires: %s", t->requires[i]);
                a3_ui_spacing(ui, 6);
                a3_ui_heading(ui, "Settings");
                for (u32 fi = 0; fi < t->field_count; ++fi) {
                    const A3FieldDesc *f = &t->fields[fi];
                    if (f->flags & A3_FIELD_FLAG_HIDDEN) continue;
                    a3_ui_label_font(ui, A3_FONT_UI_BOLD, col(ui, A3_UIC_TEXT), f->label);
                    a3_ui_same_line(ui);
                    a3_ui_label_colored(ui, col(ui, A3_UIC_TEXT_DIM), "%s  (%s%s)", f->name, a3_field_type_name(f->type), (f->flags & A3_FIELD_FLAG_ADVANCED) ? ", advanced" : "");
                    if (f->doc) { A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt - 16, f->doc)); a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect(tr.x + 16, tr.y, tr.w - 16, tr.h), col(ui, A3_UIC_TEXT_DIM), f->doc); }
                }
            }
        }
    }
    a3_ui_end_panel(ui);
}

/* ======================================================================== */
/* Settings (editor, rendering, physics, input, custom components)          */
/* ======================================================================== */

static struct {
    char name[A3_NAME_MAX];
    A3CustomFieldDef fields[12];
    i32 types[12];
    u32 count;
} g_newcomp;

static const char *const g_custom_types[] = { "Bool", "Integer", "Number", "Vector3", "Color", "Text" };
static const A3FieldType g_custom_type_map[] = { A3_FIELD_BOOL, A3_FIELD_I32, A3_FIELD_F32, A3_FIELD_VEC3, A3_FIELD_COLOR, A3_FIELD_STRING };

void ed_custom_component_save(A3Editor *ed, const char *name, const A3CustomFieldDef *fields, u32 count) {
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_EDITOR);
    A3JsonWriter jw;
    a3_jw_init(&jw, &sb, 0);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", "asm3d.component");
    a3_jw_kv_int(&jw, "version", 1);
    a3_jw_kv_string(&jw, "name", name);
    a3_jw_key(&jw, "fields");
    a3_jw_begin_array(&jw);
    for (u32 i = 0; i < count; ++i) {
        a3_jw_begin_object(&jw);
        a3_jw_kv_string(&jw, "name", fields[i].name);
        a3_jw_kv_string(&jw, "type", a3_field_type_name(fields[i].type));
        a3_jw_kv_floats(&jw, "default", fields[i].default_value, 4);
        if (fields[i].default_string[0]) a3_jw_kv_string(&jw, "defaultText", fields[i].default_string);
        a3_jw_end_object(&jw);
    }
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    char rel[ED_PATH], abs[ED_PATH], dir[ED_PATH];
    a3_snprintf(rel, sizeof(rel), "Assets/Components/%s.a3comp", name);
    ed_project_path(ed, rel, abs, sizeof(abs));
    a3_path_dirname(abs, dir, sizeof(dir));
    a3_dir_create(dir);
    if (a3_file_write_atomic(abs, sb.data, sb.len) != A3_OK) A3_ERROR("editor", "could not save %s", rel);
    a3_strbuf_free(&sb);
}

static b32 comp_file_visit(const char *dir, const A3DirEntry *e, void *user) {
    A3_UNUSED(user);
    if (e->is_dir || !a3_str_ends_with(e->name, ".a3comp")) return 1;
    char path[ED_PATH];
    a3_path_join(path, sizeof(path), dir, e->name);
    A3FileData fd;
    if (a3_file_read_all(path, A3_MEM_TEMP, &fd) != A3_OK) return 1;
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, 8192);
    A3JsonError err;
    A3Json *root = a3_json_parse((const char *)fd.data, fd.size, &ar, &err);
    if (!root) A3_ERROR("editor", "%s: line %d: %s", e->name, err.line, err.message);
    else {
        A3CustomFieldDef defs[A3_MAX_FIELDS];
        u32 n = 0;
        A3_JSON_FOREACH(f, a3_json_get(root, "fields")) {
            if (n >= A3_MAX_FIELDS) break;
            A3CustomFieldDef *d = &defs[n];
            a3_zero_struct(d);
            a3_strcpy(d->name, sizeof(d->name), a3_json_get_string(f, "name", "value"));
            d->type = a3_field_type_from_name(a3_json_get_string(f, "type", "f32"));
            a3_json_get_floats(a3_json_get(f, "default"), d->default_value, 4);
            a3_strcpy(d->default_string, sizeof(d->default_string), a3_json_get_string(f, "defaultText", ""));
            ++n;
        }
        const char *name = a3_json_get_string(root, "name", 0);
        if (name && a3_custom_component_define(name, defs, n, "Custom component defined in this project.") != 0xFFFFFFFFu)
            A3_DEBUG("editor", "loaded custom component %s", name);
    }
    a3_arena_release(&ar);
    a3_free(fd.data);
    return 1;
}

void ed_custom_components_load(A3Editor *ed) {
    char dir[ED_PATH];
    ed_project_path(ed, "Assets/Components", dir, sizeof(dir));
    if (a3_dir_exists(dir)) a3_dir_list(dir, comp_file_visit, ed);
}

void ed_settings_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    A3_UNUSED(r);
    if (a3_ui_collapsing_header(ui, "set_editor", "Editor", A3_ICON_GEAR, 1)) {
        i32 lvl = ed->level == ED_LEVEL_BEGINNER ? 0 : (ed->level == ED_LEVEL_ADVANCED ? 1 : 2);
        static const char *const lv[] = { "Beginner", "Advanced", "Engine Developer" };
        a3_ui_property(ui, "Mode", "Beginner hides advanced settings and shows tips");
        if (a3_ui_combo(ui, "mode", &lvl, lv, 3)) ed->level = (EdUserLevel)lvl;
        a3_ui_property(ui, "Dark Theme", 0);
        if (a3_ui_toggle(ui, "dark", &ed->dark_theme)) ed_apply_theme(ed);
        a3_ui_property(ui, "Camera Speed", "Fly speed in meters per second (scroll while flying)");
        a3_ui_slider_float(ui, "camspeed", &ed->cam_speed, 0.5f, 100, "%.1f");
        a3_ui_property(ui, "Snap", "Snap moves to 1 m, rotations to 15 degrees");
        a3_ui_toggle(ui, "snap", &ed->snap);
        a3_ui_property(ui, "Scene Icons", 0);
        a3_ui_toggle(ui, "icons", &ed->show_icons);
        a3_ui_property(ui, "Show Bounds", 0);
        a3_ui_toggle(ui, "bounds", &ed->show_bounds);
    }
    if (a3_ui_collapsing_header(ui, "set_render", "Rendering", A3_ICON_CUBE, 1)) {
        A3RenderSettings *rs = a3_renderer_settings(a3_engine_renderer(ed->engine));
        a3_ui_property(ui, "Shadows", 0); a3_ui_toggle(ui, "shadows", &rs->shadows);
        if (ed->level != ED_LEVEL_BEGINNER) {
            static const char *const sizes[] = { "512", "1024", "2048", "4096" };
            i32 si = rs->shadow_map_size >= 4096 ? 3 : rs->shadow_map_size >= 2048 ? 2 : rs->shadow_map_size >= 1024 ? 1 : 0;
            a3_ui_property(ui, "Shadow Resolution", "Higher is sharper but slower");
            if (a3_ui_combo(ui, "smsize", &si, sizes, 4)) rs->shadow_map_size = 512 << si;
            a3_ui_property(ui, "Shadow Distance", "Meters around the camera that get sun shadows");
            a3_ui_slider_float(ui, "smdist", &rs->shadow_distance, 10, 300, "%.0f m");
            a3_ui_property(ui, "Frustum Culling", "Skips objects outside the view (SIMD assembly)"); a3_ui_toggle(ui, "cull", &rs->frustum_culling);
            a3_ui_property(ui, "Max Lights", 0); a3_ui_slider_int(ui, "maxl", &rs->max_lights, 0, 16);
        }
        a3_ui_property(ui, "Anti-aliasing (FXAA)", 0); a3_ui_toggle(ui, "fxaa", &rs->fxaa);
        a3_ui_property(ui, "Vignette", 0); a3_ui_slider_float(ui, "vig", &rs->vignette, 0, 1, "%.2f");
    }
    if (a3_ui_collapsing_header(ui, "set_audio", "Audio", A3_ICON_MUSIC, 1)) {
        f32 mv = a3_audio_master_volume();
        a3_ui_property(ui, "Master Volume", "Overall volume of the editor and the game preview");
        if (a3_ui_slider_float(ui, "master_vol", &mv, 0, 1.5f, "%.2f")) a3_audio_set_master_volume(mv);
        a3_ui_property(ui, "Output", 0);
        a3_ui_label_colored(ui, a3_audio_device_ok() ? col(ui, A3_UIC_TEXT) : col(ui, A3_UIC_WARNING), "%s", a3_audio_device_ok() ? a3_audio_device_name() : "No audio device (silent)");
    }
    A3World *w = ed_active_world(ed);
    if (w && a3_ui_collapsing_header(ui, "set_phys", "Physics", A3_ICON_BOX, ed->level != ED_LEVEL_BEGINNER)) {
        A3PhysicsSettings *ps = a3_physics_settings(w);
        A3CWorldSettings *ws = a3_world_settings(w);
        a3_ui_property(ui, "Gravity", "Meters per second squared");
        if (a3_ui_drag_float_n(ui, "grav", &ws->gravity.x, 3, 0.05f, 0, 0, "%.2f")) ed_undo_mark_changed(ed, "Gravity");
        if (ps) {
            a3_ui_property(ui, "Solver Iterations", "More iterations make stacks steadier"); a3_ui_slider_int(ui, "iters", &ps->solver_iterations, 1, 30);
            a3_ui_property(ui, "Sleeping", "Resting bodies stop simulating until touched"); a3_ui_toggle(ui, "sleep", &ps->sleeping);
            if (ed->level != ED_LEVEL_BEGINNER) {
                a3_ui_property(ui, "Correction", "Baumgarte penetration correction"); a3_ui_slider_float(ui, "baum", &ps->baumgarte, 0, 1, "%.2f");
                a3_ui_property(ui, "Slop", "Allowed penetration in meters"); a3_ui_slider_float(ui, "slop", &ps->slop, 0, 0.05f, "%.3f");
            }
        }
    }
    if (a3_ui_collapsing_header(ui, "set_input", "Input Actions", A3_ICON_COMMAND, 0)) {
        const A3InputMap *map = a3_engine_input_map(ed->engine);
        for (u32 i = 0; i < map->count; ++i) {
            const A3Action *a = &map->actions[i];
            char keys[128] = { 0 };
            for (u32 b = 0; b < a->binding_count; ++b) {
                const A3Binding *bd = &a->bindings[b];
                char one[32];
                if (bd->kind == A3_BIND_KEY) a3_snprintf(one, sizeof(one), "%s%s", a3_key_name(bd->code), bd->scale < 0 ? "(-)" : "");
                else a3_snprintf(one, sizeof(one), "Mouse %d", bd->code + 1);
                if (b) a3_strcat(keys, sizeof(keys), ", ");
                a3_strcat(keys, sizeof(keys), one);
            }
            a3_ui_property(ui, a->name, a->description[0] ? a->description : 0);
            a3_ui_label(ui, "%s", keys);
        }
    }
    if (ed->level != ED_LEVEL_BEGINNER && a3_ui_collapsing_header(ui, "set_custom", "Custom Components", A3_ICON_PENCIL, 0)) {
        a3_ui_label_wrapped(ui, "Define your own component (data only). It appears in Add Component, is saved in scenes and documented automatically.");
        a3_ui_property(ui, "Name", 0);
        a3_ui_input_text(ui, "cc_name", g_newcomp.name, sizeof(g_newcomp.name), 0, "Health");
        for (u32 i = 0; i < g_newcomp.count; ++i) {
            a3_ui_push_id_int(ui, (i64)i);
            a3_ui_input_text(ui, "fname", g_newcomp.fields[i].name, sizeof(g_newcomp.fields[i].name), 0, "fieldName");
            a3_ui_same_line(ui);
            a3_ui_combo(ui, "ftype", &g_newcomp.types[i], g_custom_types, A3_ARRAY_COUNT(g_custom_types));
            a3_ui_pop_id(ui);
        }
        if (g_newcomp.count < A3_ARRAY_COUNT(g_newcomp.fields) && a3_ui_button_ex(ui, "+ Field", 0, A3_BUTTON_SMALL)) {
            A3CustomFieldDef *f = &g_newcomp.fields[g_newcomp.count];
            a3_zero_struct(f);
            a3_snprintf(f->name, sizeof(f->name), "value%u", g_newcomp.count + 1);
            g_newcomp.types[g_newcomp.count] = 2;
            g_newcomp.count++;
        }
        a3_ui_same_line(ui);
        b32 ok = g_newcomp.name[0] && g_newcomp.count > 0 && a3_is_alpha(g_newcomp.name[0]);
        if (a3_ui_button_ex(ui, "Create Component", 0, ok ? A3_BUTTON_PRIMARY : A3_BUTTON_DISABLED) && ok) {
            for (u32 i = 0; i < g_newcomp.count; ++i) g_newcomp.fields[i].type = g_custom_type_map[g_newcomp.types[i]];
            if (a3_custom_component_define(g_newcomp.name, g_newcomp.fields, g_newcomp.count, "Custom component defined in this project.") != 0xFFFFFFFFu) {
                ed_custom_component_save(ed, g_newcomp.name, g_newcomp.fields, g_newcomp.count);
                a3_ui_notify(ui, col(ui, A3_UIC_SUCCESS), "Component '%s' created", g_newcomp.name);
                a3_zero_struct(&g_newcomp);
            } else {
                a3_ui_notify(ui, col(ui, A3_UIC_ERROR), "Could not create the component (name in use by a built-in?)");
            }
        }
    }
}
