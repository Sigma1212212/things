/*
 * ASM3D Editor - Animation panel (keyframe timeline).
 *
 * Works on the selected object's Animator clip: scrub or play to preview
 * (the scene is restored when you stop), move/rotate/scale the object or
 * change any setting, then press Key to store the value at the playhead.
 */
#include "editor.h"
#include "../../engine/anim/a3_anim.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/platform/a3_platform.h"

#define NAMES_W 200.0f
#define ROW_H 24.0f
#define RULER_H 24.0f

typedef struct EdAnim {
    u64 guid;
    char path[ED_PATH];
    A3AnimClip clip;
    b32 dirty;
    f32 time;
    b32 playing;
    char *restore;          /* object state before previewing */
    i32 sel_track, sel_key;
    b32 drag_key;
    b32 scrubbing;
} EdAnim;

static EdAnim g_an;

static u32 colr(A3Ui *ui, A3UiColor c) { return a3_ui_theme(ui)->colors[c]; }

static void preview_end(A3Editor *ed) {
    if (g_an.restore) {
        ed_restore_snapshot(ed->world, g_an.guid, g_an.restore);
        a3_free(g_an.restore);
        g_an.restore = 0;
    }
    g_an.playing = 0;
}

static void preview_apply(A3Editor *ed, A3Entity e) {
    if (ed->mode != ED_EDIT) return;
    if (!g_an.restore) g_an.restore = ed_snapshot(ed->world, g_an.guid);
    a3_anim_apply(&g_an.clip, ed->world, e, g_an.time);
    a3_transform_system_update(ed->world);
}

static b32 save_clip(A3Editor *ed) {
    if (!g_an.path[0]) return 0;
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_EDITOR);
    a3_anim_save_json(&g_an.clip, &sb);
    char abs[ED_PATH], dir[ED_PATH];
    ed_project_path(ed, g_an.path, abs, sizeof(abs));
    a3_path_dirname(abs, dir, sizeof(dir));
    a3_dir_create(dir);
    b32 ok = a3_file_write_atomic(abs, sb.data, sb.len) == A3_OK;
    a3_strbuf_free(&sb);
    if (ok) {
        g_an.dirty = 0;
        a3_anim_clip_invalidate(g_an.path);
        a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_SUCCESS), "Saved %s", a3_path_filename(g_an.path));
    } else a3_log_hint(A3_LOG_ERROR, "anim", "Check that the project folder is writable.", "could not save %s", g_an.path);
    return ok;
}

static void load_clip(A3Editor *ed, u64 guid, const char *path) {
    if (g_an.dirty) save_clip(ed); /* never lose keys */
    preview_end(ed);
    a3_anim_clip_free(&g_an.clip);
    g_an.guid = guid;
    a3_strcpy(g_an.path, sizeof(g_an.path), path);
    g_an.time = 0;
    g_an.sel_track = g_an.sel_key = -1;
    g_an.dirty = 0;
    char abs[ED_PATH], err[160];
    ed_project_path(ed, path, abs, sizeof(abs));
    A3FileData fd;
    if (a3_file_read_all(abs, A3_MEM_TEMP, &fd) == A3_OK) {
        if (!a3_anim_load_json(&g_an.clip, (const char *)fd.data, fd.size, err, sizeof(err))) {
            A3_ERROR("anim", "cannot open %s: %s", path, err);
            a3_anim_clip_init(&g_an.clip, a3_path_filename(path));
        }
        a3_free(fd.data);
    } else {
        char stem[64];
        a3_path_stem(path, stem, sizeof(stem));
        a3_anim_clip_init(&g_an.clip, stem);
    }
}

static void key_field(A3Editor *ed, A3Entity e, const char *component, const char *field) {
    f32 v[4];
    if (!a3_anim_read_field(ed->world, e, component, field, v)) return;
    i32 ti = a3_anim_track_add(&g_an.clip, "", component, field);
    if (ti < 0) { a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_WARNING), "Too many tracks (max %d)", A3_ANIM_MAX_TRACKS); return; }
    g_an.sel_track = ti;
    g_an.sel_key = a3_anim_key_set(&g_an.clip.tracks[ti], g_an.time, v, A3_INTERP_SMOOTH);
    if (g_an.time > g_an.clip.duration) g_an.clip.duration = g_an.time;
    g_an.dirty = 1;
}

void ed_anim_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    A3UiTheme *th = a3_ui_theme(ui);
    const A3InputState *in = a3_ui_input(ui);
    if (!ed->has_project || !ed->world) return;
    if (ed->mode != ED_EDIT) {
        preview_end(ed);
        a3_ui_label_colored(ui, colr(ui, A3_UIC_TEXT_DIM), "Animations play in the game. Stop the game to edit them.");
        return;
    }
    A3Entity e = ed_selected(ed);
    if (!a3_entity_valid(ed->world, e)) {
        if (g_an.guid) { if (g_an.dirty) save_clip(ed); preview_end(ed); g_an.guid = 0; }
        a3_ui_spacing(ui, 8);
        a3_ui_label_colored(ui, colr(ui, A3_UIC_TEXT_DIM), "Select an object to animate it.");
        a3_ui_label_wrapped(ui, "Animations change settings over time: move a platform along a path, open a door, flicker a light, fade a color. For simple spinning or bobbing, add a Motion component instead.");
        return;
    }
    A3CAnimator *an = (A3CAnimator *)a3_component_get(ed->world, e, A3_T_ANIMATOR);
    if (!an || !an->clip.path[0]) {
        if (g_an.guid) { preview_end(ed); g_an.guid = 0; }
        a3_ui_spacing(ui, 8);
        a3_ui_label(ui, "'%s' has no animation yet.", a3_entity_name(ed->world, e));
        if (a3_ui_button_ex(ui, "Create Animation", 170, A3_BUTTON_PRIMARY)) {
            char rel[ED_PATH], abs[ED_PATH], base[64];
            a3_strcpy(base, sizeof(base), a3_entity_name(ed->world, e));
            for (char *p = base; *p; ++p) if (!a3_is_alnum(*p) && *p != '-' && *p != '_') *p = '_';
            for (int i = 1; i < 100; ++i) {
                if (i == 1) a3_snprintf(rel, sizeof(rel), "Assets/Animations/%s.a3anim", base);
                else a3_snprintf(rel, sizeof(rel), "Assets/Animations/%s_%d.a3anim", base, i);
                ed_project_path(ed, rel, abs, sizeof(abs));
                if (!a3_file_exists(abs)) break;
            }
            ed_undo_begin_frame(ed);
            an = (A3CAnimator *)a3_component_add(ed->world, e, A3_T_ANIMATOR);
            if (an) {
                a3_strcpy(an->clip.path, sizeof(an->clip.path), rel);
                ed_undo_mark_changed(ed, "Create Animation");
                load_clip(ed, ed->selected, rel);
                key_field(ed, e, "Transform", "position"); /* a first key so the timeline is not empty */
                save_clip(ed);
            }
        }
        return;
    }
    if (g_an.guid != ed->selected || !a3_streq(g_an.path, an->clip.path)) load_clip(ed, ed->selected, an->clip.path);
    A3AnimClip *c = &g_an.clip;
    /* ---- toolbar ---- */
    if (a3_ui_icon_button(ui, g_an.playing ? 0x1 : A3_ICON_PLAY, g_an.playing ? "Pause preview" : "Play preview", g_an.playing ? A3_BUTTON_TOGGLED : 0)) g_an.playing = !g_an.playing;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_STOP, "Stop preview (the object goes back to normal)", g_an.restore ? 0 : A3_BUTTON_DISABLED)) { preview_end(ed); g_an.time = 0; }
    a3_ui_same_line(ui);
    a3_ui_label(ui, "%.2f s", (f64)g_an.time);
    a3_ui_same_line(ui);
    a3_ui_set_next_width(ui, 90);
    if (a3_ui_drag_float(ui, "anim_len", &c->duration, 0.02f, 0.1f, 600, "len %.2f s")) g_an.dirty = 1;
    a3_ui_tooltip(ui, "Length of the animation in seconds");
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Key Transform", 120, A3_BUTTON_PRIMARY | A3_BUTTON_SMALL)) {
        key_field(ed, e, "Transform", "position");
        key_field(ed, e, "Transform", "rotation");
        key_field(ed, e, "Transform", "scale");
    }
    a3_ui_tooltip(ui, "Stores the object's position, rotation and scale at the playhead");
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Key Setting...", 110, A3_BUTTON_SMALL)) a3_ui_open_popup(ui, "anim_fields");
    if (a3_ui_begin_popup(ui, "anim_fields", 280)) {
        for (u32 id = 0; id < A3_MAX_COMPONENT_TYPES; ++id) {
            A3ComponentType *t = a3_component_type(id);
            if (!t || !t->name[0] || !a3_component_has(ed->world, e, id) || id == A3_T_ANIMATOR) continue;
            if (a3_ui_begin_menu(ui, t->name)) {
                for (u32 f = 0; f < t->field_count; ++f) {
                    const A3FieldDesc *fd = &t->fields[f];
                    if (!a3_anim_field_animatable(fd->type) || (fd->flags & (A3_FIELD_FLAG_HIDDEN | A3_FIELD_FLAG_TRANSIENT))) continue;
                    if (a3_ui_menu_item(ui, fd->label, 0, 1)) key_field(ed, e, t->name, fd->name);
                }
                a3_ui_end_menu(ui);
            }
        }
        a3_ui_end_popup(ui);
    }
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, g_an.dirty ? "Save *" : "Save", 70, A3_BUTTON_SMALL | (g_an.dirty ? A3_BUTTON_PRIMARY : 0))) save_clip(ed);
    /* selected key: interpolation + delete */
    b32 key_ok = g_an.sel_track >= 0 && g_an.sel_track < (i32)c->track_count && g_an.sel_key >= 0 && g_an.sel_key < (i32)c->tracks[g_an.sel_track].keys.count;
    if (key_ok) {
        A3AnimKey *k = &c->tracks[g_an.sel_track].keys.data[g_an.sel_key];
        a3_ui_same_line(ui);
        static const char *const interp[] = { "Smooth", "Linear", "Step" };
        a3_ui_set_next_width(ui, 100);
        if (a3_ui_combo(ui, "key_interp", &k->interp, interp, A3_INTERP_COUNT)) g_an.dirty = 1;
        a3_ui_tooltip(ui, "How the value changes on the way to the next key");
        a3_ui_same_line(ui);
        if (a3_ui_icon_button(ui, A3_ICON_CROSS, "Delete key (Del)", A3_BUTTON_SMALL)) {
            a3_anim_key_remove(&c->tracks[g_an.sel_track], (u32)g_an.sel_key);
            g_an.sel_key = -1;
            g_an.dirty = 1;
        }
    }
    /* ---- timeline ---- */
    A3Vec2 cp = a3_ui_cursor_pos(ui);
    A3Rect area = a3_rect(r.x, cp.y, r.w, r.y + r.h - cp.y);
    if (area.h < RULER_H + 10) return;
    A3Rect names = a3_rect(area.x, area.y + RULER_H, NAMES_W, area.h - RULER_H);
    A3Rect keys_r = a3_rect(area.x + NAMES_W, area.y + RULER_H, area.w - NAMES_W, area.h - RULER_H);
    A3Rect ruler = a3_rect(keys_r.x, area.y, keys_r.w, RULER_H);
    f32 len = a3_maxf(c->duration, a3_maxf(a3_anim_clip_length(c), 0.1f));
    f32 pps = (keys_r.w - 16) / len;
    #define T2X(t) (keys_r.x + 8 + (t) * pps)
    a3_ui_rect(ui, area, th->colors[A3_UIC_BG], 0);
    a3_ui_rect(ui, a3_rect(area.x, area.y, NAMES_W, area.h), th->colors[A3_UIC_PANEL_ALT], 0);
    /* ruler with second / tenth ticks */
    a3_ui_rect(ui, ruler, th->colors[A3_UIC_HEADER], 0);
    f32 step = len > 20 ? 5.0f : (len > 6 ? 1.0f : 0.5f);
    for (f32 t = 0; t <= len + 1e-3f; t += step / 5) {
        f32 x = T2X(t);
        b32 major = a3_absf(a3_fmodf(t + 1e-4f, step)) < 2e-3f;
        a3_ui_rect(ui, a3_rect(x, ruler.y + (major ? 8 : 16), 1, major ? 16 : 8), th->colors[A3_UIC_TEXT_DIM], 0);
        if (major) { char lb[16]; a3_snprintf(lb, sizeof(lb), "%.1f", (f64)t); a3_ui_text(ui, A3_FONT_UI, a3_v2(x + 3, ruler.y + 2), th->colors[A3_UIC_TEXT_DIM], lb); }
    }
    a3_ui_rect(ui, a3_rect(T2X(c->duration), ruler.y, 2, area.h), a3_color_alpha(th->colors[A3_UIC_WARNING], 0.5f), 0);
    /* scrub */
    a3_ui_invisible_button(ui, "anim_ruler", ruler);
    if (a3_ui_item_active(ui)) {
        g_an.time = a3_clampf((in->mouse_pos.x - keys_r.x - 8) / pps, 0, len);
        g_an.playing = 0;
        preview_apply(ed, e);
    }
    /* rows */
    i32 remove_track = -1;
    a3_ui_push_clip(ui, a3_rect(area.x, keys_r.y, area.w, keys_r.h));
    for (u32 ti = 0; ti < c->track_count; ++ti) {
        A3AnimTrack *t = &c->tracks[ti];
        f32 y = keys_r.y + (f32)ti * ROW_H;
        if ((ti & 1) == 0) a3_ui_rect(ui, a3_rect(area.x, y, area.w, ROW_H), a3_color_alpha(th->colors[A3_UIC_PANEL], 0.5f), 0);
        char label[160];
        const A3ComponentType *ct = a3_component_type_by_name(t->component);
        const A3FieldDesc *fd = ct ? a3_component_find_field(ct, t->field) : 0;
        a3_snprintf(label, sizeof(label), "%s%s%s %s", t->target, t->target[0] ? " / " : "", t->component, fd ? fd->label : t->field);
        a3_ui_push_id_int(ui, (i64)ti);
        A3Rect nr = a3_rect(names.x, y, NAMES_W, ROW_H);
        a3_ui_invisible_button(ui, "track", nr);
        if (a3_ui_item_clicked(ui, A3_MOUSE_LEFT)) { g_an.sel_track = (i32)ti; g_an.sel_key = -1; }
        if (a3_ui_begin_context_menu(ui, "track_ctx")) {
            if (a3_ui_menu_item(ui, "Delete Track", 0, 1)) remove_track = (i32)ti;
            a3_ui_end_popup(ui);
        }
        a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(nr.x + 8, nr.y, nr.w - 12, nr.h), A3_ALIGN_LEFT,
                           g_an.sel_track == (i32)ti ? th->colors[A3_UIC_ACCENT] : (fd ? th->colors[A3_UIC_TEXT] : th->colors[A3_UIC_ERROR]), label);
        if (!fd) a3_ui_tooltip(ui, "This setting no longer exists on the object; the track is ignored.");
        /* keys */
        for (u32 k = 0; k < t->keys.count; ++k) {
            f32 x = T2X(t->keys.data[k].time), cy = y + ROW_H * 0.5f;
            b32 sel = g_an.sel_track == (i32)ti && g_an.sel_key == (i32)k;
            a3_ui_push_id_int(ui, 1000 + (i64)k);
            A3Rect kr = a3_rect(x - 7, cy - 7, 14, 14);
            a3_ui_invisible_button(ui, "key", kr);
            if (a3_ui_item_hovered(ui) && in->mouse_pressed[A3_MOUSE_LEFT]) {
                g_an.sel_track = (i32)ti;
                g_an.sel_key = (i32)k;
                g_an.drag_key = 1;
                g_an.time = t->keys.data[k].time;
                preview_apply(ed, e);
            }
            if (sel && g_an.drag_key && a3_ui_item_active(ui) && in->mouse_delta.x != 0) {
                /* drag the key in time (snaps to 1/30 s); re-insert to keep keys sorted */
                A3AnimKey moved = t->keys.data[k];
                f32 nt = a3_clampf(a3_floorf(((in->mouse_pos.x - keys_r.x - 8) / pps) * 30.0f + 0.5f) / 30.0f, 0, 600);
                a3_anim_key_remove(t, k);
                g_an.sel_key = a3_anim_key_set(t, nt, moved.value, moved.interp);
                g_an.time = nt;
                g_an.dirty = 1;
                preview_apply(ed, e);
            }
            u32 kc = sel ? th->colors[A3_UIC_WARNING] : (t->keys.data[k].interp == A3_INTERP_STEP ? th->colors[A3_UIC_TEXT_DIM] : th->colors[A3_UIC_ACCENT]);
            A3Vec2 c2 = a3_v2(x, cy);
            a3_ui_triangle(ui, a3_v2(c2.x, c2.y - 6), a3_v2(c2.x + 6, c2.y), a3_v2(c2.x, c2.y + 6), kc);
            a3_ui_triangle(ui, a3_v2(c2.x, c2.y - 6), a3_v2(c2.x, c2.y + 6), a3_v2(c2.x - 6, c2.y), kc);
            a3_ui_pop_id(ui);
        }
        a3_ui_pop_id(ui);
    }
    a3_ui_pop_clip(ui);
    if (!in->mouse[A3_MOUSE_LEFT]) g_an.drag_key = 0;
    if (remove_track >= 0) { a3_anim_track_remove(c, (u32)remove_track); g_an.sel_track = g_an.sel_key = -1; g_an.dirty = 1; }
    if (!c->track_count)
        a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect(keys_r.x + 12, keys_r.y + 8, keys_r.w - 24, 60), th->colors[A3_UIC_TEXT_DIM],
                           "Move the playhead, change the object (move it, change a color...), then press Key Transform or Key Setting.");
    /* playhead */
    f32 px = T2X(g_an.time);
    a3_ui_rect(ui, a3_rect(px - 1, area.y, 2, area.h), th->colors[A3_UIC_ERROR], 0);
    a3_ui_triangle(ui, a3_v2(px - 6, area.y), a3_v2(px + 6, area.y), a3_v2(px, area.y + 8), th->colors[A3_UIC_ERROR]);
    #undef T2X
    /* playback */
    if (g_an.playing) {
        g_an.time += a3_maxf(ed->fps > 0 ? 1.0f / ed->fps : 1.0f / 60.0f, 0);
        if (g_an.time > len) g_an.time = 0;
        preview_apply(ed, e);
    }
    /* keyboard shortcuts while the pointer is over the timeline */
    if (a3_rect_contains(area, in->mouse_pos) && !a3_ui_wants_keyboard(ui)) {
        if (in->keys_pressed[A3_KEY_DELETE] && key_ok) {
            a3_anim_key_remove(&c->tracks[g_an.sel_track], (u32)g_an.sel_key);
            g_an.sel_key = -1;
            g_an.dirty = 1;
        }
        if (in->keys_pressed[A3_KEY_SPACE]) g_an.playing = !g_an.playing;
        if (in->keys_pressed[A3_KEY_K]) { key_field(ed, e, "Transform", "position"); key_field(ed, e, "Transform", "rotation"); key_field(ed, e, "Transform", "scale"); }
    }
}

void ed_anim_shutdown(A3Editor *ed) {
    if (g_an.dirty && ed->has_project) save_clip(ed);
    a3_free(g_an.restore);
    g_an.restore = 0;
    a3_anim_clip_free(&g_an.clip);
    a3_zero_struct(&g_an);
}

/* Before playing, saving or leaving: make the clip file current and put the object back. */
void ed_anim_flush(A3Editor *ed) {
    if (g_an.dirty && ed->has_project) save_clip(ed);
    preview_end(ed);
}
