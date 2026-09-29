/*
 * ASM3D - a3_ui.c
 * Immediate-mode widgets, layout, popups, text editing, drag and drop.
 */
#include "a3_ui_internal.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"

/* ======================================================================== */
/* Themes                                                                   */
/* ======================================================================== */

void a3_ui_theme_dark(A3UiTheme *t) {
    a3_zero_struct(t);
    u32 *c = t->colors;
    c[A3_UIC_BG] = a3_rgb(22, 24, 29);
    c[A3_UIC_PANEL] = a3_rgb(33, 36, 43);
    c[A3_UIC_PANEL_ALT] = a3_rgb(38, 42, 50);
    c[A3_UIC_HEADER] = a3_rgb(27, 29, 35);
    c[A3_UIC_BORDER] = a3_rgb(52, 57, 68);
    c[A3_UIC_TEXT] = a3_rgb(222, 226, 234);
    c[A3_UIC_TEXT_DIM] = a3_rgb(145, 152, 166);
    c[A3_UIC_TEXT_DISABLED] = a3_rgb(92, 98, 110);
    c[A3_UIC_WIDGET] = a3_rgb(46, 50, 60);
    c[A3_UIC_WIDGET_HOVER] = a3_rgb(58, 63, 76);
    c[A3_UIC_WIDGET_ACTIVE] = a3_rgb(66, 72, 88);
    c[A3_UIC_ACCENT] = a3_rgb(64, 140, 255);
    c[A3_UIC_ACCENT_HOVER] = a3_rgb(92, 160, 255);
    c[A3_UIC_ACCENT_TEXT] = a3_rgb(255, 255, 255);
    c[A3_UIC_SELECTION] = a3_rgba(64, 140, 255, 90);
    c[A3_UIC_POPUP] = a3_rgb(40, 44, 53);
    c[A3_UIC_SHADOW] = a3_rgba(0, 0, 0, 160);
    c[A3_UIC_SCROLLBAR] = a3_rgba(255, 255, 255, 40);
    c[A3_UIC_SUCCESS] = a3_rgb(80, 200, 120);
    c[A3_UIC_WARNING] = a3_rgb(240, 180, 60);
    c[A3_UIC_ERROR] = a3_rgb(240, 90, 90);
    c[A3_UIC_AXIS_X] = a3_rgb(226, 80, 80);
    c[A3_UIC_AXIS_Y] = a3_rgb(110, 200, 80);
    c[A3_UIC_AXIS_Z] = a3_rgb(80, 130, 240);
    t->rounding = 4;
    t->panel_rounding = 6;
    t->padding = 8;
    t->spacing = 5;
    t->row_height = 24;
    t->indent = 16;
    t->label_width = 0.38f;
    t->scrollbar_width = 10;
}

void a3_ui_theme_light(A3UiTheme *t) {
    a3_ui_theme_dark(t);
    u32 *c = t->colors;
    c[A3_UIC_BG] = a3_rgb(214, 218, 226);
    c[A3_UIC_PANEL] = a3_rgb(242, 244, 248);
    c[A3_UIC_PANEL_ALT] = a3_rgb(232, 235, 241);
    c[A3_UIC_HEADER] = a3_rgb(226, 229, 236);
    c[A3_UIC_BORDER] = a3_rgb(196, 201, 212);
    c[A3_UIC_TEXT] = a3_rgb(30, 34, 42);
    c[A3_UIC_TEXT_DIM] = a3_rgb(96, 104, 118);
    c[A3_UIC_TEXT_DISABLED] = a3_rgb(150, 156, 168);
    c[A3_UIC_WIDGET] = a3_rgb(224, 228, 236);
    c[A3_UIC_WIDGET_HOVER] = a3_rgb(210, 216, 228);
    c[A3_UIC_WIDGET_ACTIVE] = a3_rgb(196, 204, 220);
    c[A3_UIC_POPUP] = a3_rgb(250, 251, 253);
    c[A3_UIC_SHADOW] = a3_rgba(0, 0, 0, 70);
    c[A3_UIC_SCROLLBAR] = a3_rgba(0, 0, 0, 50);
}

#define COL(ui, c) ((ui)->theme.colors[(c)])

/* ======================================================================== */
/* Context                                                                  */
/* ======================================================================== */

A3Ui *a3_ui_create(void) {
    A3Ui *ui = A3_NEW(A3Ui, A3_MEM_UI);
    if (!ui) return 0;
    a3_ui_theme_dark(&ui->theme);
    a3_hashmap_init(&ui->state_map, 256, A3_MEM_UI);
    ui->width = 1280;
    ui->height = 720;
    ui->layer = A3_LAYER_MAIN;
    ui_draw_init(ui);
    return ui;
}

void a3_ui_destroy(A3Ui *ui) {
    if (!ui) return;
    ui_draw_shutdown(ui);
    a3_array_free(ui->panel_states);
    a3_array_free(ui->states);
    a3_hashmap_free(&ui->state_map);
    a3_free(ui);
}

A3UiTheme *a3_ui_theme(A3Ui *ui) { return &ui->theme; }
void a3_ui_set_clipboard_fns(A3Ui *ui, A3UiSetClipboard set, A3UiGetClipboard get, void *user) { ui->set_clip = set; ui->get_clip = get; ui->clip_user = user; }
A3Cursor a3_ui_cursor(A3Ui *ui) { return ui->cursor; }
b32 a3_ui_wants_mouse(A3Ui *ui) { return ui->wants_mouse; }
b32 a3_ui_wants_keyboard(A3Ui *ui) { return ui->wants_keyboard; }
void a3_ui_claim_keyboard(A3Ui *ui, A3UiId id) { ui->edit_id = id; ui->focus = id; ui->focus_next = id; ui->wants_keyboard = 1; }
b32  a3_ui_has_keyboard(A3Ui *ui, A3UiId id) { return id && ui->edit_id == id; }
void a3_ui_release_keyboard(A3Ui *ui, A3UiId id) { if (ui->edit_id == id) ui->edit_id = 0; }
void a3_ui_set_clipboard(A3Ui *ui, const char *text) { if (ui->set_clip) ui->set_clip(ui->clip_user, text); }
const char *a3_ui_get_clipboard(A3Ui *ui) { const char *t = ui->get_clip ? ui->get_clip(ui->clip_user) : 0; return t ? t : ""; }
f64 a3_ui_time(A3Ui *ui) { return ui->time; }
A3Vec2 a3_ui_mouse(A3Ui *ui) { return ui->in.mouse_pos; }
const A3InputState *a3_ui_input(A3Ui *ui) { return &ui->in; }

UiState *ui_state(A3Ui *ui, A3UiId id, i32 di, f32 df) {
    u64 idx;
    if (a3_hashmap_get(&ui->state_map, (u64)id | (1ull << 40), &idx)) {
        UiState *s = &ui->states.data[idx - 1];
        s->frame = ui->frame;
        return s;
    }
    UiState s = { id, di, df, ui->frame };
    if (!a3_array_push(ui->states, s, A3_MEM_UI)) { static UiState dummy; return &dummy; }
    a3_hashmap_put(&ui->state_map, (u64)id | (1ull << 40), ui->states.count);
    return &a3_array_last(ui->states);
}

static UiPanelState *panel_state(A3Ui *ui, A3UiId id) {
    for (u32 i = 0; i < ui->panel_states.count; ++i) if (ui->panel_states.data[i].id == id) return &ui->panel_states.data[i];
    UiPanelState s;
    a3_zero_struct(&s);
    s.id = id;
    if (!a3_array_push(ui->panel_states, s, A3_MEM_UI)) { static UiPanelState dummy; return &dummy; }
    return &a3_array_last(ui->panel_states);
}

void a3_ui_begin_frame(A3Ui *ui, const A3InputState *in, i32 w, i32 h, f32 dt) {
    if (in) { ui->in = *in; ui->have_input = 1; }
    else a3_zero_struct(&ui->in);
    ui->width = w > 0 ? w : 1;
    ui->height = h > 0 ? h : 1;
    ui->dt = dt;
    ui->time += dt;
    ui->frame++;
    ui_draw_reset(ui);
    ui->layer = A3_LAYER_MAIN;
    ui->hot = ui->hot_next;
    ui->hot_next = 0;
    ui->focus = ui->focus_next ? ui->focus_next : ui->focus;
    ui->focus_next = 0;
    ui->id_depth = 0;
    ui->panel_depth = 0;
    ui->popup_depth = 0;
    ui->tooltip_set = 0;
    ui->mouse_consumed = 0;
    ui->wheel_consumed = 0;
    ui->any_hovered = 0;
    ui->cursor = A3_CURSOR_ARROW;
    ui->active_released = 0;
    ui->released_id = 0;
    ui->drag_released = ui->dragging && !ui->in.mouse[A3_MOUSE_LEFT];
    if (ui->active && !ui->in.mouse[A3_MOUSE_LEFT] && !ui->in.mouse[A3_MOUSE_RIGHT] && !ui->in.mouse[A3_MOUSE_MIDDLE] && ui->active != ui->edit_id) {
        ui->released_id = ui->active;
        ui->active_released = 1;
        ui->active = 0;
    }
    /* clicking anywhere outside open popups closes them */
    if (ui->popup_count && (ui->in.mouse_pressed[A3_MOUSE_LEFT] || ui->in.mouse_pressed[A3_MOUSE_RIGHT])) {
        b32 inside = 0;
        for (u32 i = 0; i < ui->popup_rects_prev_count; ++i) if (a3_rect_contains(ui->popup_rects_prev[i], ui->in.mouse_pos)) inside = 1;
        if (a3_rect_contains(ui->menubar_rect, ui->in.mouse_pos) && ui->menubar_open) inside = 1;
        if (!inside) { ui->popup_count = 0; ui->menubar_open = 0; }
    }
    if (ui->popup_count && ui->in.keys_pressed[A3_KEY_ESCAPE]) { ui->popup_count--; if (!ui->popup_count) ui->menubar_open = 0; }
    /* expire untouched per-widget state */
    if ((ui->frame & 255) == 0) {
        for (u32 i = 0; i < ui->states.count;) {
            if (ui->frame - ui->states.data[i].frame > 600) {
                a3_hashmap_remove(&ui->state_map, (u64)ui->states.data[i].id | (1ull << 40));
                u32 last = ui->states.count - 1;
                if (i != last) {
                    ui->states.data[i] = ui->states.data[last];
                    a3_hashmap_put(&ui->state_map, (u64)ui->states.data[i].id | (1ull << 40), i + 1);
                }
                ui->states.count--;
            } else ++i;
        }
    }
}

static void draw_tooltip_and_toasts(A3Ui *ui) {
    a3_ui_set_layer(ui, A3_LAYER_OVERLAY);
    if (ui->tooltip_set && ui->tooltip[0] && !ui->dragging) {
        f32 maxw = 320;
        f32 w = a3_minf(a3_font_text_width(A3_FONT_UI, ui->tooltip, -1), maxw);
        f32 h = a3_ui_text_wrapped_height(A3_FONT_UI, w + 1, ui->tooltip);
        A3Vec2 m = ui->in.mouse_pos;
        A3Rect r = a3_rect(m.x + 14, m.y + 18, w + 16, h + 12);
        if (r.x + r.w > ui->width) r.x = (f32)ui->width - r.w - 4;
        if (r.y + r.h > ui->height) r.y = m.y - r.h - 6;
        a3_ui_shadow(ui, r, 10, 6, COL(ui, A3_UIC_SHADOW));
        a3_ui_rect(ui, r, COL(ui, A3_UIC_POPUP), 6);
        a3_ui_rect_outline(ui, r, COL(ui, A3_UIC_BORDER), 6, 1);
        a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect(r.x + 8, r.y + 6, w + 1, h), COL(ui, A3_UIC_TEXT), ui->tooltip);
    }
    if (ui->dragging && ui->drag_label[0]) {
        A3Vec2 m = ui->in.mouse_pos;
        f32 w = a3_font_text_width(A3_FONT_UI, ui->drag_label, -1) + 16;
        A3Rect r = a3_rect(m.x + 12, m.y + 8, w, 24);
        a3_ui_rect(ui, r, a3_color_alpha(COL(ui, A3_UIC_ACCENT), 0.9f), 5);
        a3_ui_text_in_rect(ui, A3_FONT_UI, r, A3_ALIGN_CENTER, COL(ui, A3_UIC_ACCENT_TEXT), ui->drag_label);
    }
    /* toasts (bottom right, fade out) */
    f32 y = (f32)ui->height - 12;
    for (u32 i = 0; i < ui->toast_count;) {
        UiToast *t = &ui->toasts[i];
        t->time -= ui->dt;
        if (t->time <= 0) {
            a3_memmove(&ui->toasts[i], &ui->toasts[i + 1], sizeof(UiToast) * (ui->toast_count - i - 1));
            ui->toast_count--;
            continue;
        }
        f32 a = a3_saturate(t->time / 0.4f);
        f32 w = a3_minf(a3_font_text_width(A3_FONT_UI, t->text, -1) + 36, 460);
        A3Rect r = a3_rect((f32)ui->width - w - 12, y - 36, w, 32);
        a3_ui_shadow(ui, r, 10, 6, a3_color_alpha(COL(ui, A3_UIC_SHADOW), a));
        a3_ui_rect(ui, r, a3_color_alpha(COL(ui, A3_UIC_POPUP), a), 6);
        a3_ui_rect(ui, a3_rect(r.x, r.y, 4, r.h), a3_color_alpha(t->color, a), 2);
        a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(r.x + 14, r.y, r.w - 20, r.h), A3_ALIGN_LEFT, a3_color_alpha(COL(ui, A3_UIC_TEXT), a), t->text);
        y -= 40;
        ++i;
    }
    a3_ui_set_layer(ui, A3_LAYER_MAIN);
}

void a3_ui_end_frame(A3Ui *ui) {
    draw_tooltip_and_toasts(ui);
    /* remember popup rects for next frame's hover blocking */
    ui->popup_rects_prev_count = 0;
    for (u32 i = 0; i < ui->popup_count && i < 8; ++i) ui->popup_rects_prev[ui->popup_rects_prev_count++] = ui->popups[i].rect;
    /* drag and drop ends on release */
    if (ui->dragging && !ui->in.mouse[A3_MOUSE_LEFT]) ui->dragging = 0;
    if (ui->dragging) ui->cursor = A3_CURSOR_HAND;
    /* hover delay for tooltips */
    if (ui->hot == ui->hover_id && ui->hot) ui->hover_time += ui->dt;
    else { ui->hover_id = ui->hot; ui->hover_time = 0; }
    ui->wants_mouse = ui->any_hovered || ui->active != 0 || ui->popup_count > 0 || ui->modal_open != 0 || ui->dragging;
    ui->wants_keyboard = ui->edit_id != 0;
    if (ui->edit_id && ui->edit_id != ui->focus) ui->edit_id = 0;
}

/* ======================================================================== */
/* IDs                                                                      */
/* ======================================================================== */

static A3UiId id_seed(A3Ui *ui) { return ui->id_depth ? ui->id_stack[ui->id_depth - 1] : 0x811C9DC5u; }

A3UiId a3_ui_id(A3Ui *ui, const char *s) {
    u32 h = id_seed(ui);
    /* "Label##id": only the part after ## identifies the widget */
    const char *hash = a3_strstr(s, "##");
    if (hash) s = hash + 2;
    for (; *s; ++s) { h ^= (u8)*s; h *= 16777619u; }
    return h ? h : 1;
}

void a3_ui_push_id(A3Ui *ui, const char *s) { if (ui->id_depth < 64) ui->id_stack[ui->id_depth] = a3_ui_id(ui, s), ui->id_depth++; }
void a3_ui_push_id_int(A3Ui *ui, i64 v) {
    u32 h = id_seed(ui) ^ (u32)(v * 2654435761u) ^ (u32)(v >> 32);
    h *= 16777619u;
    if (ui->id_depth < 64) ui->id_stack[ui->id_depth++] = h ? h : 1;
}
void a3_ui_pop_id(A3Ui *ui) { if (ui->id_depth) ui->id_depth--; }

static const char *display_label(const char *s) {
    /* hide the "##id" suffix */
    return s;
}

static i32 label_len(const char *s) {
    const char *h = a3_strstr(s, "##");
    return h ? (i32)(h - s) : (i32)a3_strlen(s);
}

/* ======================================================================== */
/* Interaction                                                              */
/* ======================================================================== */

static b32 blocked_by_popup(A3Ui *ui) {
    if (ui->layer >= A3_LAYER_POPUP) return 0;
    if (ui->modal_open && !ui->in_modal) return 1;
    for (u32 i = 0; i < ui->popup_rects_prev_count; ++i) if (a3_rect_contains(ui->popup_rects_prev[i], ui->in.mouse_pos)) return 1;
    return 0;
}

static b32 mouse_over(A3Ui *ui, A3Rect r) {
    return a3_rect_contains(r, ui->in.mouse_pos) && a3_rect_contains(a3_ui_clip(ui), ui->in.mouse_pos) && !blocked_by_popup(ui) && !ui->in.mouse_captured;
}

typedef struct Behavior { b32 hovered, held, pressed, clicked, right_clicked, double_clicked; } Behavior;

static Behavior behave(A3Ui *ui, A3UiId id, A3Rect r) {
    Behavior b;
    a3_zero_struct(&b);
    ui->last_id = id;
    ui->last_rect = r;
    b.hovered = mouse_over(ui, r) && (ui->active == 0 || ui->active == id);
    if (b.hovered) { ui->hot_next = id; ui->any_hovered = 1; }
    ui->last_hovered = b.hovered;
    if (b.hovered && ui->hot == id) {
        if (ui->in.mouse_pressed[A3_MOUSE_LEFT]) {
            ui->active = id;
            b.pressed = 1;
            ui->drag_start_mouse = ui->in.mouse_pos;
            if (ui->edit_id && ui->edit_id != id) ui->edit_id = 0;
            ui->focus_next = id;
        }
        if (ui->in.mouse_double_click[A3_MOUSE_LEFT]) b.double_clicked = 1;
        if (ui->in.mouse_pressed[A3_MOUSE_RIGHT]) b.right_clicked = 1;
    }
    b.held = ui->active == id && ui->in.mouse[A3_MOUSE_LEFT];
    if (ui->released_id == id && ui->active_released && mouse_over(ui, r)) b.clicked = 1;
    return b;
}

A3Rect a3_ui_last_rect(A3Ui *ui) { return ui->last_rect; }
A3UiId a3_ui_last_id(A3Ui *ui) { return ui->last_id; }
b32 a3_ui_item_hovered(A3Ui *ui) { return ui->last_hovered; }
b32 a3_ui_item_clicked(A3Ui *ui, A3MouseButton b) { return ui->last_hovered && ui->in.mouse_pressed[b] && !blocked_by_popup(ui); }
b32 a3_ui_item_double_clicked(A3Ui *ui) { return ui->last_hovered && ui->in.mouse_double_click[A3_MOUSE_LEFT]; }
b32 a3_ui_item_active(A3Ui *ui) { return ui->active == ui->last_id; }
b32 a3_ui_item_released(A3Ui *ui) { return ui->active_released && ui->released_id == ui->last_id; }

void a3_ui_tooltip(A3Ui *ui, const char *text) {
    if (!text || !*text || !ui->last_hovered || ui->active) return;
    if (ui->hover_id == ui->last_id && ui->hover_time > 0.45f) a3_ui_tooltip_now(ui, text);
}

void a3_ui_tooltip_now(A3Ui *ui, const char *text) {
    a3_strcpy(ui->tooltip, sizeof(ui->tooltip), text);
    ui->tooltip_set = 1;
}

/* ======================================================================== */
/* Panels & layout                                                          */
/* ======================================================================== */

static UiPanel *cur_panel(A3Ui *ui) {
    static UiPanel root;
    if (!ui->panel_depth) {
        a3_zero_struct(&root);
        root.inner = root.rect = a3_rect(0, 0, (f32)ui->width, (f32)ui->height);
        root.cursor = a3_v2(0, 0);
        return &root;
    }
    return &ui->panels[ui->panel_depth - 1];
}

b32 a3_ui_begin_panel(A3Ui *ui, const char *id_str, A3Rect r, u32 flags) {
    if (ui->panel_depth >= 32) return 0;
    A3UiId id = a3_ui_id(ui, id_str);
    UiPanelState *ps = panel_state(ui, id);
    UiPanel *p = &ui->panels[ui->panel_depth++];
    a3_zero_struct(p);
    p->id = id;
    p->rect = r;
    p->flags = flags;
    f32 pad = (flags & A3_PANEL_NO_PADDING) ? 0 : ui->theme.padding;
    b32 vscroll = !(flags & A3_PANEL_NO_SCROLL) && ps->content_h > r.h - 2 * pad + 0.5f;
    f32 sbw = vscroll ? ui->theme.scrollbar_width : 0;
    p->inner = a3_rect(r.x + pad, r.y + pad, r.w - 2 * pad - sbw, r.h - 2 * pad);
    f32 max_scroll = a3_maxf(ps->content_h - p->inner.h, 0);
    ps->scroll_y = a3_clampf(ps->scroll_y, 0, max_scroll);
    if (flags & A3_PANEL_HORIZONTAL_SCROLL) ps->scroll_x = a3_clampf(ps->scroll_x, 0, a3_maxf(ps->content_w - p->inner.w, 0));
    else ps->scroll_x = 0;
    p->scroll_y = ps->scroll_y;
    p->scroll_x = ps->scroll_x;
    p->cursor = a3_v2(p->inner.x - p->scroll_x, p->inner.y - p->scroll_y);
    p->content_bottom = p->cursor.y;
    p->content_right = p->cursor.x;
    if (!(flags & A3_PANEL_NO_BACKGROUND)) a3_ui_rect(ui, r, COL(ui, A3_UIC_PANEL), 0);
    if (flags & A3_PANEL_BORDER) a3_ui_rect_outline(ui, r, COL(ui, A3_UIC_BORDER), ui->theme.rounding, 1);
    a3_ui_push_clip(ui, a3_rect(r.x, r.y, r.w, r.h));
    a3_ui_push_id(ui, id_str);
    return r.w > 0 && r.h > 0;
}

void a3_ui_end_panel(A3Ui *ui) {
    if (!ui->panel_depth) return;
    UiPanel *p = &ui->panels[ui->panel_depth - 1];
    UiPanelState *ps = panel_state(ui, p->id);
    ps->content_h = (p->content_bottom + p->scroll_y) - p->inner.y;
    ps->content_w = (p->content_right + p->scroll_x) - p->inner.x;
    ps->view_h = p->inner.h;
    ps->frame = ui->frame;
    /* mouse wheel scrolling (innermost panel under the mouse wins) */
    b32 over = a3_rect_contains(p->rect, ui->in.mouse_pos) && !blocked_by_popup(ui);
    if (over) ui->any_hovered = 1;
    if (over && !ui->wheel_consumed && !(p->flags & A3_PANEL_NO_SCROLL)) {
        if (ui->in.scroll.y != 0 && ps->content_h > p->inner.h) {
            ps->scroll_y -= ui->in.scroll.y * 48.0f;
            ui->wheel_consumed = 1;
        }
        if (ui->in.scroll.x != 0 && (p->flags & A3_PANEL_HORIZONTAL_SCROLL)) {
            ps->scroll_x += ui->in.scroll.x * 48.0f;
            ui->wheel_consumed = 1;
        }
    }
    /* scrollbar */
    if (!(p->flags & A3_PANEL_NO_SCROLL) && ps->content_h > p->inner.h + 0.5f) {
        f32 track_h = p->rect.h - 4;
        f32 thumb_h = a3_maxf(track_h * p->inner.h / ps->content_h, 24);
        f32 max_scroll = ps->content_h - p->inner.h;
        f32 t = max_scroll > 0 ? ps->scroll_y / max_scroll : 0;
        A3Rect track = a3_rect(p->rect.x + p->rect.w - ui->theme.scrollbar_width, p->rect.y + 2, ui->theme.scrollbar_width - 2, track_h);
        A3Rect thumb = a3_rect(track.x + 2, track.y + (track_h - thumb_h) * t, track.w - 3, thumb_h);
        A3UiId sid = a3_ui_id(ui, "#scrollbar");
        Behavior b = behave(ui, sid, track);
        if (b.pressed && !a3_rect_contains(thumb, ui->in.mouse_pos)) {
            f32 rel = (ui->in.mouse_pos.y - track.y - thumb_h * 0.5f) / (track_h - thumb_h);
            ps->scroll_y = a3_clampf(rel, 0, 1) * max_scroll;
        }
        if (b.held) {
            f32 dy = ui->in.mouse_delta.y;
            ps->scroll_y += dy * max_scroll / a3_maxf(track_h - thumb_h, 1);
        }
        u32 c = b.held ? COL(ui, A3_UIC_ACCENT) : (b.hovered ? a3_color_alpha(COL(ui, A3_UIC_TEXT_DIM), 0.6f) : COL(ui, A3_UIC_SCROLLBAR));
        a3_ui_rect(ui, thumb, c, 3);
    }
    a3_ui_pop_id(ui);
    a3_ui_pop_clip(ui);
    ui->panel_depth--;
}

A3Rect a3_ui_panel_rect(A3Ui *ui) { return cur_panel(ui)->inner; }

void a3_ui_scroll_to_top(A3Ui *ui) {
    if (!ui->panel_depth) return;
    panel_state(ui, cur_panel(ui)->id)->scroll_y = 0;
}

void a3_ui_scroll_here(A3Ui *ui) {
    if (!ui->panel_depth) return;
    UiPanel *p = cur_panel(ui);
    UiPanelState *ps = panel_state(ui, p->id);
    f32 y = p->cursor.y + p->scroll_y - p->inner.y;
    if (y < ps->scroll_y) ps->scroll_y = y;
    else if (y + ui->theme.row_height > ps->scroll_y + p->inner.h) ps->scroll_y = y + ui->theme.row_height - p->inner.h;
}

A3Vec2 a3_ui_cursor_pos(A3Ui *ui) { return cur_panel(ui)->cursor; }
void a3_ui_set_cursor_pos(A3Ui *ui, A3Vec2 p) { UiPanel *pn = cur_panel(ui); pn->cursor = p; pn->same_line = 0; pn->cursor_x_set = 1; }

f32 a3_ui_content_width(A3Ui *ui) {
    UiPanel *p = cur_panel(ui);
    return p->inner.x + p->inner.w - p->cursor.x;
}

A3Rect a3_ui_next_rect(A3Ui *ui, f32 w, f32 h) {
    UiPanel *p = cur_panel(ui);
    if (p->property) {
        p->property = 0;
        p->cursor_x_set = 0;
        A3Rect r = p->property_rect;
        if (h > r.h) r.h = h;
        p->cursor.y = r.y + r.h + ui->theme.spacing;
        p->content_bottom = a3_maxf(p->content_bottom, r.y + r.h);
        p->same_line = 0;
        p->line_h = 0;
        return r;
    }
    if (p->same_line) {
        p->cursor = a3_v2(p->last_item_right + ui->theme.spacing, p->last_line_pos.y);
        p->same_line = 0;
    } else if (p->cursor_x_set) {
        p->cursor_x_set = 0;
    } else {
        p->cursor.x = p->inner.x + p->indent - p->scroll_x;
    }
    f32 avail = p->inner.x + p->inner.w - p->cursor.x;
    if (w <= 0) w = avail + w;
    if (w < 1) w = 1;
    A3Rect r = a3_rect(p->cursor.x, p->cursor.y, w, h);
    p->last_line_pos = p->cursor;
    p->last_item_right = r.x + r.w;
    p->line_h = a3_maxf(p->line_h, h);
    p->last_item_h = h;
    /* next line by default */
    p->cursor.y += h + ui->theme.spacing;
    p->content_bottom = a3_maxf(p->content_bottom, r.y + r.h);
    p->content_right = a3_maxf(p->content_right, r.x + r.w);
    return r;
}

void a3_ui_same_line(A3Ui *ui) {
    UiPanel *p = cur_panel(ui);
    p->same_line = 1;
    p->cursor.y = p->last_line_pos.y;
}

void a3_ui_spacing(A3Ui *ui, f32 h) { a3_ui_next_rect(ui, 1, h > ui->theme.spacing ? h - ui->theme.spacing : 0.0f); }

void a3_ui_separator(A3Ui *ui) {
    A3Rect r = a3_ui_next_rect(ui, 0, 5);
    a3_ui_rect(ui, a3_rect(r.x, r.y + 2, r.w, 1), COL(ui, A3_UIC_BORDER), 0);
}

void a3_ui_indent(A3Ui *ui, f32 w) { cur_panel(ui)->indent = a3_maxf(cur_panel(ui)->indent + w, 0); }

void a3_ui_property(A3Ui *ui, const char *label, const char *tooltip) {
    A3Rect row = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    UiPanel *p = cur_panel(ui);
    f32 lw = ui->theme.label_width <= 1.0f ? row.w * ui->theme.label_width : ui->theme.label_width;
    lw = a3_clampf(lw, 60, row.w - 60);
    A3Rect lr = a3_rect(row.x, row.y, lw - 6, row.h);
    A3UiId lid = a3_ui_id(ui, label);
    Behavior b = behave(ui, lid ^ 0x51ED, lr);
    a3_ui_text_in_rect(ui, A3_FONT_UI, lr, A3_ALIGN_LEFT, b.hovered ? COL(ui, A3_UIC_TEXT) : COL(ui, A3_UIC_TEXT_DIM), label);
    if (tooltip) a3_ui_tooltip(ui, tooltip);
    /* the next widget occupies the value column of this row */
    p->cursor.y = row.y;
    p->property = 1;
    p->property_rect = a3_rect(row.x + lw, row.y, row.w - lw, row.h);
}

/* ======================================================================== */
/* Basic widgets                                                            */
/* ======================================================================== */

static void draw_button_bg(A3Ui *ui, A3Rect r, Behavior b, u32 flags) {
    u32 c;
    if (flags & A3_BUTTON_PRIMARY) c = b.held ? COL(ui, A3_UIC_ACCENT) : (b.hovered ? COL(ui, A3_UIC_ACCENT_HOVER) : COL(ui, A3_UIC_ACCENT));
    else if (flags & A3_BUTTON_DANGER) c = b.hovered ? a3_color_lerp(COL(ui, A3_UIC_ERROR), 0xFFFFFFFFu, 0.1f) : COL(ui, A3_UIC_ERROR);
    else if (flags & A3_BUTTON_TOGGLED) c = b.hovered ? COL(ui, A3_UIC_ACCENT_HOVER) : a3_color_alpha(COL(ui, A3_UIC_ACCENT), 0.85f);
    else if (flags & A3_BUTTON_FLAT) c = b.held ? COL(ui, A3_UIC_WIDGET_ACTIVE) : (b.hovered ? COL(ui, A3_UIC_WIDGET_HOVER) : 0);
    else c = b.held ? COL(ui, A3_UIC_WIDGET_ACTIVE) : (b.hovered ? COL(ui, A3_UIC_WIDGET_HOVER) : COL(ui, A3_UIC_WIDGET));
    if (flags & A3_BUTTON_DISABLED) c = a3_color_alpha(COL(ui, A3_UIC_WIDGET), 0.5f);
    if (c >> 24) a3_ui_rect(ui, r, c, ui->theme.rounding);
    if (!(flags & (A3_BUTTON_FLAT | A3_BUTTON_PRIMARY | A3_BUTTON_DANGER | A3_BUTTON_TOGGLED)))
        a3_ui_rect_gradient(ui, a3_rect(r.x + 1, r.y + 1, r.w - 2, r.h * 0.5f), a3_rgba(255, 255, 255, 10), a3_rgba(255, 255, 255, 0));
}

b32 a3_ui_button_rect(A3Ui *ui, const char *label, A3Rect r, u32 flags) {
    A3UiId id = a3_ui_id(ui, label);
    Behavior b = behave(ui, id, r);
    if (flags & A3_BUTTON_DISABLED) { b.clicked = 0; b.held = 0; }
    if (b.hovered && !(flags & A3_BUTTON_DISABLED)) ui->cursor = A3_CURSOR_HAND;
    draw_button_bg(ui, r, b, flags);
    u32 tc = (flags & (A3_BUTTON_PRIMARY | A3_BUTTON_DANGER | A3_BUTTON_TOGGLED)) ? COL(ui, A3_UIC_ACCENT_TEXT) : COL(ui, A3_UIC_TEXT);
    if (flags & A3_BUTTON_DISABLED) tc = COL(ui, A3_UIC_TEXT_DISABLED);
    char text[256];
    a3_str_to_buf(a3_str_n(label, (usize)label_len(label)), text, sizeof(text));
    a3_ui_text_in_rect(ui, A3_FONT_UI, r, A3_ALIGN_CENTER, tc, text);
    return b.clicked;
}

b32 a3_ui_button_ex(A3Ui *ui, const char *label, f32 width, u32 flags) {
    f32 h = (flags & A3_BUTTON_SMALL) ? ui->theme.row_height - 4 : ui->theme.row_height;
    if (width == 0) width = a3_font_text_width(A3_FONT_UI, label, label_len(label)) + 24;
    A3Rect r = a3_ui_next_rect(ui, width, h);
    return a3_ui_button_rect(ui, label, r, flags);
}

b32 a3_ui_button(A3Ui *ui, const char *label) { return a3_ui_button_ex(ui, label, 0, 0); }

b32 a3_ui_icon_button(A3Ui *ui, u32 icon, const char *tooltip, u32 flags) {
    f32 s = ui->theme.row_height;
    A3Rect r = a3_ui_next_rect(ui, s, s);
    char idb[48];
    a3_snprintf(idb, sizeof(idb), "icon%u_%s", icon, tooltip ? tooltip : "");
    A3UiId id = a3_ui_id(ui, idb);
    Behavior b = behave(ui, id, r);
    if (flags & A3_BUTTON_DISABLED) { b.clicked = 0; b.held = 0; }
    if (b.hovered) ui->cursor = A3_CURSOR_HAND;
    draw_button_bg(ui, r, b, flags | ((flags & A3_BUTTON_TOGGLED) ? 0 : A3_BUTTON_FLAT));
    u32 col = (flags & A3_BUTTON_TOGGLED) ? COL(ui, A3_UIC_ACCENT_TEXT) : (flags & A3_BUTTON_DISABLED) ? COL(ui, A3_UIC_TEXT_DISABLED) : COL(ui, A3_UIC_TEXT);
    A3Vec2 c = a3_v2(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    if (icon == 0x1u) a3_ui_draw_icon_pause(ui, c, 14, col);
    else if (icon == 0x2u) a3_ui_draw_icon_step(ui, c, 14, col);
    else if (icon == 0x3u) a3_ui_draw_icon_search(ui, c, 16, col);
    else if (icon == 0x4u) a3_ui_draw_icon_folder(ui, c, 16, col);
    else a3_ui_icon(ui, icon, c, col, A3_FONT_UI);
    if (tooltip) a3_ui_tooltip(ui, tooltip);
    return b.clicked;
}

b32 a3_ui_invisible_button(A3Ui *ui, const char *id, A3Rect r) {
    Behavior b = behave(ui, a3_ui_id(ui, id), r);
    return b.clicked;
}

void a3_ui_label_font(A3Ui *ui, A3FontId font, u32 color, const char *text) {
    f32 h = a3_maxf(a3_font_line_height(font), ui->theme.row_height - 4);
    A3Rect r = a3_ui_next_rect(ui, a3_font_text_width(font, text, -1) + 2, h);
    ui->last_rect = r;
    ui->last_id = a3_ui_id(ui, text) ^ 0x1abe1;
    ui->last_hovered = mouse_over(ui, r);
    if (ui->last_hovered) ui->hot_next = ui->hot_next ? ui->hot_next : ui->last_id;
    a3_ui_text_in_rect(ui, font, r, A3_ALIGN_LEFT, color, text);
}

void a3_ui_label(A3Ui *ui, const char *fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    a3_vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    a3_ui_label_font(ui, A3_FONT_UI, COL(ui, A3_UIC_TEXT), buf);
}

void a3_ui_label_colored(A3Ui *ui, u32 color, const char *fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    a3_vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    a3_ui_label_font(ui, A3_FONT_UI, color, buf);
}

void a3_ui_label_wrapped(A3Ui *ui, const char *text) {
    f32 w = a3_ui_content_width(ui);
    f32 h = a3_ui_text_wrapped_height(A3_FONT_UI, w, text);
    A3Rect r = a3_ui_next_rect(ui, w, h);
    a3_ui_text_wrapped(ui, A3_FONT_UI, r, COL(ui, A3_UIC_TEXT_DIM), text);
}

void a3_ui_heading(A3Ui *ui, const char *text) {
    a3_ui_label_font(ui, A3_FONT_HEADING, COL(ui, A3_UIC_TEXT), text);
}

b32 a3_ui_checkbox(A3Ui *ui, const char *label, b32 *v) {
    const char *shown = display_label(label);
    i32 ll = label_len(shown);
    f32 s = 18;
    f32 w = s + (ll ? a3_font_text_width(A3_FONT_UI, shown, ll) + 8 : 0);
    A3Rect r = a3_ui_next_rect(ui, w, ui->theme.row_height);
    A3UiId id = a3_ui_id(ui, label);
    Behavior b = behave(ui, id, r);
    if (b.clicked) *v = !*v;
    A3Rect box = a3_rect(r.x, r.y + (r.h - s) * 0.5f, s, s);
    u32 bg = *v ? (b.hovered ? COL(ui, A3_UIC_ACCENT_HOVER) : COL(ui, A3_UIC_ACCENT)) : (b.hovered ? COL(ui, A3_UIC_WIDGET_HOVER) : COL(ui, A3_UIC_WIDGET));
    a3_ui_rect(ui, box, bg, 4);
    if (!*v) a3_ui_rect_outline(ui, box, COL(ui, A3_UIC_BORDER), 4, 1);
    if (*v) {
        A3Vec2 pts[3] = { { box.x + 4, box.y + 9 }, { box.x + 7.5f, box.y + 12.5f }, { box.x + 14, box.y + 5.5f } };
        a3_ui_polyline(ui, pts, 3, COL(ui, A3_UIC_ACCENT_TEXT), 2.0f, 0);
    }
    if (ll) {
        char text[256];
        a3_str_to_buf(a3_str_n(shown, (usize)ll), text, sizeof(text));
        a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(r.x + s + 8, r.y, r.w - s - 8, r.h), A3_ALIGN_LEFT, COL(ui, A3_UIC_TEXT), text);
    }
    return b.clicked;
}

b32 a3_ui_toggle(A3Ui *ui, const char *id_str, b32 *v) {
    A3Rect r = a3_ui_next_rect(ui, 36, ui->theme.row_height);
    Behavior b = behave(ui, a3_ui_id(ui, id_str), r);
    if (b.clicked) *v = !*v;
    UiState *st = ui_state(ui, ui->last_id, 0, *v ? 1.0f : 0.0f);
    st->f = a3_lerpf(st->f, *v ? 1.0f : 0.0f, a3_damp_factor(18.0f, ui->dt));
    A3Rect track = a3_rect(r.x, r.y + (r.h - 18) * 0.5f, 34, 18);
    a3_ui_rect(ui, track, a3_color_lerp(COL(ui, A3_UIC_WIDGET_ACTIVE), COL(ui, A3_UIC_ACCENT), st->f), 9);
    a3_ui_circle(ui, a3_v2(track.x + 9 + st->f * 16, track.y + 9), 7, 0xFFFFFFFFu);
    return b.clicked;
}



b32 a3_ui_slider_float(A3Ui *ui, const char *id_str, f32 *v, f32 min, f32 max, const char *fmt) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    A3UiId id = a3_ui_id(ui, id_str);
    Behavior b = behave(ui, id, r);
    f32 old = *v;
    if (b.held && max > min) {
        f32 t = a3_saturate((ui->in.mouse_pos.x - r.x - 6) / a3_maxf(r.w - 12, 1));
        *v = min + (max - min) * t;
        ui->cursor = A3_CURSOR_RESIZE_H;
    }
    f32 t = max > min ? a3_saturate((*v - min) / (max - min)) : 0;
    a3_ui_rect(ui, r, b.hovered || b.held ? COL(ui, A3_UIC_WIDGET_HOVER) : COL(ui, A3_UIC_WIDGET), ui->theme.rounding);
    a3_ui_rect(ui, a3_rect(r.x, r.y, 6 + (r.w - 12) * t, r.h), a3_color_alpha(COL(ui, A3_UIC_ACCENT), 0.55f), ui->theme.rounding);
    a3_ui_rect(ui, a3_rect(r.x + 3 + (r.w - 12) * t, r.y + 3, 6, r.h - 6), COL(ui, A3_UIC_ACCENT_TEXT), 3);
    char buf[64];
    a3_snprintf(buf, sizeof(buf), fmt ? fmt : "%.2f", (f64)*v);
    a3_ui_text_in_rect(ui, A3_FONT_UI, r, A3_ALIGN_CENTER, COL(ui, A3_UIC_TEXT), buf);
    return *v != old;
}

b32 a3_ui_slider_int(A3Ui *ui, const char *id, i32 *v, i32 min, i32 max) {
    f32 f = (f32)*v;
    b32 ch = a3_ui_slider_float(ui, id, &f, (f32)min, (f32)max, "%.0f");
    i32 nv = (i32)a3_floorf(f + 0.5f);
    if (nv != *v) { *v = nv; return 1; }
    return ch && 0;
}

/* Text entry shared by drag widgets (double click to type a value). */
static b32 text_edit_field(A3Ui *ui, A3UiId id, A3Rect r, char *buf, usize cap, u32 flags, const char *placeholder, b32 *committed);

static b32 drag_scalar(A3Ui *ui, A3UiId id, A3Rect r, f32 *v, f32 speed, f32 min, f32 max, const char *fmt, u32 accent) {
    b32 changed = 0;
    UiState *st = ui_state(ui, id, 0, 0);
    if (st->i == 1) {
        /* typing mode */
        char buf[64];
        if (ui->edit_id != id) {
            a3_snprintf(buf, sizeof(buf), "%.6g", (f64)*v);
            st->i = 2; /* initializing */
        }
        b32 committed = 0;
        if (st->i == 2) { a3_strcpy(ui->edit_buf, sizeof(ui->edit_buf), buf); st->i = 1; }
        char tmp[64];
        a3_strcpy(tmp, sizeof(tmp), ui->edit_id == id ? ui->edit_buf : buf);
        text_edit_field(ui, id, r, tmp, sizeof(tmp), A3_INPUT_ENTER_RETURNS | A3_INPUT_SELECT_ALL | A3_INPUT_FOCUS | A3_INPUT_NUMERIC, 0, &committed);
        if (committed || ui->edit_id != id) {
            f64 d;
            if (a3_parse_f64(tmp, a3_strlen(tmp), &d)) {
                f32 nv = (f32)d;
                if (max > min) nv = a3_clampf(nv, min, max);
                if (nv != *v) { *v = nv; changed = 1; }
            }
            st->i = 0;
        }
        return changed;
    }
    Behavior b = behave(ui, id, r);
    if (b.hovered || b.held) ui->cursor = A3_CURSOR_RESIZE_H;
    if (b.pressed) st->f = *v;
    if (b.held) {
        f32 mul = (ui->in.mods & A3_MOD_SHIFT) ? 10.0f : ((ui->in.mods & A3_MOD_ALT) ? 0.1f : 1.0f);
        f32 dx = ui->in.mouse_pos.x - ui->drag_start_mouse.x;
        f32 nv = st->f + dx * speed * mul;
        if (max > min) nv = a3_clampf(nv, min, max);
        if (nv != *v) { *v = nv; changed = 1; }
    }
    if (b.double_clicked || (b.clicked && a3_absf(ui->in.mouse_pos.x - ui->drag_start_mouse.x) < 2 && (ui->in.mods & A3_MOD_CTRL))) {
        st->i = 1;
        ui->focus_next = id;
        ui->edit_id = 0;
    }
    u32 bg = b.held ? COL(ui, A3_UIC_WIDGET_ACTIVE) : (b.hovered ? COL(ui, A3_UIC_WIDGET_HOVER) : COL(ui, A3_UIC_WIDGET));
    a3_ui_rect(ui, r, bg, ui->theme.rounding);
    if (accent) a3_ui_rect_corners(ui, a3_rect(r.x, r.y, 3, r.h), accent, ui->theme.rounding, 1 | 8);
    char buf[64];
    a3_snprintf(buf, sizeof(buf), fmt ? fmt : "%.3f", (f64)*v);
    a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(r.x + 4, r.y, r.w - 8, r.h), A3_ALIGN_CENTER, COL(ui, A3_UIC_TEXT), buf);
    return changed;
}

b32 a3_ui_drag_float(A3Ui *ui, const char *id, f32 *v, f32 speed, f32 min, f32 max, const char *fmt) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    return drag_scalar(ui, a3_ui_id(ui, id), r, v, speed, min, max, fmt, 0);
}

b32 a3_ui_drag_float_n(A3Ui *ui, const char *id_str, f32 *v, u32 n, f32 speed, f32 min, f32 max, const char *fmt) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    b32 changed = 0;
    f32 gap = 4;
    f32 w = (r.w - gap * (f32)(n - 1)) / (f32)n;
    const u32 axis[4] = { COL(ui, A3_UIC_AXIS_X), COL(ui, A3_UIC_AXIS_Y), COL(ui, A3_UIC_AXIS_Z), COL(ui, A3_UIC_TEXT_DIM) };
    a3_ui_push_id(ui, id_str);
    for (u32 i = 0; i < n; ++i) {
        char sub[8];
        a3_snprintf(sub, sizeof(sub), "c%u", i);
        A3Rect cr = a3_rect(r.x + (w + gap) * (f32)i, r.y, w, r.h);
        changed |= drag_scalar(ui, a3_ui_id(ui, sub), cr, &v[i], speed, min, max, fmt, axis[i]);
    }
    a3_ui_pop_id(ui);
    ui->last_rect = r;
    return changed;
}

b32 a3_ui_drag_int(A3Ui *ui, const char *id, i32 *v, f32 speed, i32 min, i32 max) {
    f32 f = (f32)*v;
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    drag_scalar(ui, a3_ui_id(ui, id), r, &f, speed, (f32)min, (f32)max, "%.0f", 0);
    i32 nv = (i32)a3_floorf(f + 0.5f);
    if (nv != *v) { *v = nv; return 1; }
    return 0;
}

/* ======================================================================== */
/* Text editing                                                             */
/* ======================================================================== */

static i32 utf8_prev(const char *s, i32 pos) { if (pos <= 0) return 0; --pos; while (pos > 0 && ((u8)s[pos] & 0xC0) == 0x80) --pos; return pos; }
static i32 utf8_next(const char *s, i32 len, i32 pos) { if (pos >= len) return len; ++pos; while (pos < len && ((u8)s[pos] & 0xC0) == 0x80) ++pos; return pos; }
static b32 is_word(char c) { return a3_is_ident(c) || ((u8)c >= 0x80); }

static void edit_delete_selection(A3Ui *ui) {
    i32 a = A3_MIN(ui->edit_cursor, ui->edit_anchor), b = A3_MAX(ui->edit_cursor, ui->edit_anchor);
    if (a == b) return;
    a3_memmove(ui->edit_buf + a, ui->edit_buf + b, (usize)(ui->edit_len - b + 1));
    ui->edit_len -= b - a;
    ui->edit_cursor = ui->edit_anchor = a;
}

static void edit_insert(A3Ui *ui, const char *text, i32 n, usize cap) {
    edit_delete_selection(ui);
    i32 max = (i32)A3_MIN(cap, sizeof(ui->edit_buf)) - 1;
    if (ui->edit_len + n > max) n = max - ui->edit_len;
    if (n <= 0) return;
    a3_memmove(ui->edit_buf + ui->edit_cursor + n, ui->edit_buf + ui->edit_cursor, (usize)(ui->edit_len - ui->edit_cursor + 1));
    a3_memcpy(ui->edit_buf + ui->edit_cursor, text, (usize)n);
    ui->edit_len += n;
    ui->edit_cursor += n;
    ui->edit_anchor = ui->edit_cursor;
}

static i32 edit_pos_from_x(A3FontId font, const char *s, i32 len, f32 x) {
    f32 cx = 0;
    i32 i = 0;
    while (i < len) {
        u32 cp;
        u32 n = a3_utf8_decode(s + i, &cp);
        f32 adv = a3_font_glyph(font, cp)->advance;
        if (cx + adv * 0.5f > x) return i;
        cx += adv;
        i += (i32)n;
    }
    return len;
}

static b32 text_edit_field(A3Ui *ui, A3UiId id, A3Rect r, char *buf, usize cap, u32 flags, const char *placeholder, b32 *committed) {
    Behavior b = behave(ui, id, r);
    if (b.hovered) ui->cursor = A3_CURSOR_IBEAM;
    b32 changed = 0;
    if (committed) *committed = 0;
    b32 start_edit = (b.pressed && ui->edit_id != id) || ((flags & A3_INPUT_FOCUS) && ui->edit_id != id && ui->focus_next != id && ui->focus != id);
    if ((flags & A3_INPUT_FOCUS) && ui->edit_id != id) start_edit = 1;
    if (start_edit) {
        ui->edit_id = id;
        ui->focus_next = id;
        ui->focus = id;
        a3_strcpy(ui->edit_buf, sizeof(ui->edit_buf), buf);
        ui->edit_len = (i32)a3_strlen(ui->edit_buf);
        ui->edit_cursor = ui->edit_len;
        ui->edit_anchor = (flags & A3_INPUT_SELECT_ALL) ? 0 : ui->edit_len;
        ui->edit_scroll = 0;
        ui->edit_blink = ui->time;
        if (b.pressed && !(flags & A3_INPUT_SELECT_ALL)) {
            f32 pad = (flags & A3_INPUT_SEARCH) ? 26.0f : 6.0f;
            ui->edit_cursor = ui->edit_anchor = edit_pos_from_x(A3_FONT_UI, ui->edit_buf, ui->edit_len, ui->in.mouse_pos.x - r.x - pad);
        }
    }
    b32 editing = ui->edit_id == id;
    f32 pad = (flags & A3_INPUT_SEARCH) ? 26.0f : 6.0f;
    if (editing) {
        ui->active = id;
        /* mouse selection */
        if (b.pressed && !start_edit) {
            ui->edit_cursor = edit_pos_from_x(A3_FONT_UI, ui->edit_buf, ui->edit_len, ui->in.mouse_pos.x - r.x - pad + ui->edit_scroll);
            if (!(ui->in.mods & A3_MOD_SHIFT)) ui->edit_anchor = ui->edit_cursor;
        } else if (ui->in.mouse[A3_MOUSE_LEFT] && b.hovered && !b.pressed && !start_edit && ui->hot == id) {
            ui->edit_cursor = edit_pos_from_x(A3_FONT_UI, ui->edit_buf, ui->edit_len, ui->in.mouse_pos.x - r.x - pad + ui->edit_scroll);
        }
        if (b.double_clicked) { ui->edit_anchor = 0; ui->edit_cursor = ui->edit_len; }
        /* click outside ends editing */
        if (ui->in.mouse_pressed[A3_MOUSE_LEFT] && !a3_rect_contains(r, ui->in.mouse_pos)) {
            ui->edit_id = 0;
            if (committed) *committed = 1;
            if (a3_strcmp(buf, ui->edit_buf) != 0) { a3_strcpy(buf, cap, ui->edit_buf); changed = 1; }
            ui->active = 0;
            return (flags & A3_INPUT_ENTER_RETURNS) ? changed : changed;
        }
        const A3InputState *in = &ui->in;
        b32 ctrl = (in->mods & A3_MOD_CTRL) != 0, shift = (in->mods & A3_MOD_SHIFT) != 0;
        b32 edited = 0;
        for (u32 i = 0; i < in->text_count; ++i) {
            u32 cp = in->text[i];
            if ((flags & A3_INPUT_NUMERIC) && !(a3_is_digit((int)cp) || cp == '.' || cp == '-' || cp == '+' || cp == 'e' || cp == 'E')) continue;
            char u8b[4];
            u32 n = a3_utf8_encode(cp, u8b);
            edit_insert(ui, u8b, (i32)n, cap);
            edited = 1;
        }
        if (in->keys_repeat[A3_KEY_LEFT]) {
            if (ui->edit_cursor != ui->edit_anchor && !shift) ui->edit_cursor = A3_MIN(ui->edit_cursor, ui->edit_anchor);
            else if (ctrl) { i32 p = ui->edit_cursor; while (p > 0 && !is_word(ui->edit_buf[p - 1])) --p; while (p > 0 && is_word(ui->edit_buf[p - 1])) --p; ui->edit_cursor = p; }
            else ui->edit_cursor = utf8_prev(ui->edit_buf, ui->edit_cursor);
            if (!shift) ui->edit_anchor = ui->edit_cursor;
        }
        if (in->keys_repeat[A3_KEY_RIGHT]) {
            if (ui->edit_cursor != ui->edit_anchor && !shift) ui->edit_cursor = A3_MAX(ui->edit_cursor, ui->edit_anchor);
            else if (ctrl) { i32 p = ui->edit_cursor; while (p < ui->edit_len && is_word(ui->edit_buf[p])) ++p; while (p < ui->edit_len && !is_word(ui->edit_buf[p])) ++p; ui->edit_cursor = p; }
            else ui->edit_cursor = utf8_next(ui->edit_buf, ui->edit_len, ui->edit_cursor);
            if (!shift) ui->edit_anchor = ui->edit_cursor;
        }
        if (in->keys_repeat[A3_KEY_HOME]) { ui->edit_cursor = 0; if (!shift) ui->edit_anchor = 0; }
        if (in->keys_repeat[A3_KEY_END]) { ui->edit_cursor = ui->edit_len; if (!shift) ui->edit_anchor = ui->edit_len; }
        if (in->keys_repeat[A3_KEY_BACKSPACE]) {
            if (ui->edit_cursor == ui->edit_anchor) ui->edit_anchor = ctrl ? 0 : utf8_prev(ui->edit_buf, ui->edit_cursor);
            edit_delete_selection(ui);
            edited = 1;
        }
        if (in->keys_repeat[A3_KEY_DELETE]) {
            if (ui->edit_cursor == ui->edit_anchor) ui->edit_anchor = utf8_next(ui->edit_buf, ui->edit_len, ui->edit_cursor);
            edit_delete_selection(ui);
            edited = 1;
        }
        if (ctrl && in->keys_pressed[A3_KEY_A]) { ui->edit_anchor = 0; ui->edit_cursor = ui->edit_len; }
        if (ctrl && (in->keys_pressed[A3_KEY_C] || in->keys_pressed[A3_KEY_X]) && ui->edit_cursor != ui->edit_anchor && ui->set_clip) {
            i32 a = A3_MIN(ui->edit_cursor, ui->edit_anchor), bb = A3_MAX(ui->edit_cursor, ui->edit_anchor);
            char tmp[8192];
            a3_str_to_buf(a3_str_n(ui->edit_buf + a, (usize)(bb - a)), tmp, sizeof(tmp));
            ui->set_clip(ui->clip_user, tmp);
            if (in->keys_pressed[A3_KEY_X]) { edit_delete_selection(ui); edited = 1; }
        }
        if (ctrl && in->keys_pressed[A3_KEY_V] && ui->get_clip) {
            const char *clip = ui->get_clip(ui->clip_user);
            if (clip) {
                /* single-line field: stop at the first newline */
                i32 n = 0;
                while (clip[n] && clip[n] != '\n' && clip[n] != '\r') ++n;
                edit_insert(ui, clip, n, cap);
                edited = 1;
            }
        }
        if (edited) ui->edit_blink = ui->time;
        if (edited && !(flags & A3_INPUT_ENTER_RETURNS)) {
            if (a3_strcmp(buf, ui->edit_buf) != 0) { a3_strcpy(buf, cap, ui->edit_buf); changed = 1; }
        }
        if (in->keys_pressed[A3_KEY_ENTER] || in->keys_pressed[A3_KEY_TAB]) {
            if (a3_strcmp(buf, ui->edit_buf) != 0) { a3_strcpy(buf, cap, ui->edit_buf); changed = 1; }
            if (flags & A3_INPUT_ENTER_RETURNS) changed = 1;
            if (committed) *committed = 1;
            ui->edit_id = 0;
            ui->active = 0;
        }
        if (in->keys_pressed[A3_KEY_ESCAPE]) {
            if (committed) *committed = 1;
            ui->edit_id = 0; /* cancel: keep the original text */
            ui->active = 0;
        }
    }
    /* draw */
    b32 focused = ui->edit_id == id;
    a3_ui_rect(ui, r, focused ? COL(ui, A3_UIC_BG) : (b.hovered ? COL(ui, A3_UIC_WIDGET_HOVER) : COL(ui, A3_UIC_WIDGET)), ui->theme.rounding);
    if (focused) a3_ui_rect_outline(ui, r, COL(ui, A3_UIC_ACCENT), ui->theme.rounding, 1.5f);
    if (flags & A3_INPUT_SEARCH) a3_ui_draw_icon_search(ui, a3_v2(r.x + 13, r.y + r.h * 0.5f), 14, COL(ui, A3_UIC_TEXT_DIM));
    const char *text = focused ? ui->edit_buf : buf;
    i32 tlen = (i32)a3_strlen(text);
    A3Rect tr = a3_rect(r.x + pad, r.y, r.w - pad - 6, r.h);
    f32 lh = a3_font_line_height(A3_FONT_UI);
    f32 ty = r.y + (r.h - lh) * 0.5f + 1;
    a3_ui_push_clip(ui, tr);
    if (focused) {
        f32 cx = a3_font_text_width(A3_FONT_UI, text, ui->edit_cursor);
        if (cx - ui->edit_scroll > tr.w - 4) ui->edit_scroll = cx - tr.w + 4;
        if (cx - ui->edit_scroll < 0) ui->edit_scroll = cx;
        if (ui->edit_cursor != ui->edit_anchor) {
            i32 a = A3_MIN(ui->edit_cursor, ui->edit_anchor), bb = A3_MAX(ui->edit_cursor, ui->edit_anchor);
            f32 x0 = a3_font_text_width(A3_FONT_UI, text, a), x1 = a3_font_text_width(A3_FONT_UI, text, bb);
            a3_ui_rect(ui, a3_rect(tr.x + x0 - ui->edit_scroll, r.y + 3, x1 - x0, r.h - 6), COL(ui, A3_UIC_SELECTION), 2);
        }
        a3_ui_text(ui, A3_FONT_UI, a3_v2(tr.x - ui->edit_scroll, ty), COL(ui, A3_UIC_TEXT), text);
        if (a3_fmodf((f32)(ui->time - ui->edit_blink), 1.0f) < 0.6f)
            a3_ui_rect(ui, a3_rect(tr.x + cx - ui->edit_scroll, r.y + 4, 1.5f, r.h - 8), COL(ui, A3_UIC_ACCENT), 0);
    } else if (tlen) {
        a3_ui_text(ui, A3_FONT_UI, a3_v2(tr.x, ty), COL(ui, A3_UIC_TEXT), text);
    } else if (placeholder) {
        a3_ui_text(ui, A3_FONT_UI, a3_v2(tr.x, ty), COL(ui, A3_UIC_TEXT_DISABLED), placeholder);
    }
    a3_ui_pop_clip(ui);
    return changed;
}

b32 a3_ui_input_text(A3Ui *ui, const char *id, char *buf, usize cap, u32 flags, const char *placeholder) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    return text_edit_field(ui, a3_ui_id(ui, id), r, buf, cap, flags, placeholder, 0);
}

/* ======================================================================== */
/* Selection widgets                                                        */
/* ======================================================================== */

b32 a3_ui_selectable_ex(A3Ui *ui, const char *id_str, const char *label, b32 selected, u32 icon, u32 icon_color) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    A3UiId id = a3_ui_id(ui, id_str);
    Behavior b = behave(ui, id, r);
    if (selected) a3_ui_rect(ui, r, COL(ui, A3_UIC_SELECTION), ui->theme.rounding);
    else if (b.hovered) a3_ui_rect(ui, r, COL(ui, A3_UIC_WIDGET_HOVER), ui->theme.rounding);
    f32 x = r.x + 6;
    if (icon) {
        a3_ui_icon(ui, icon, a3_v2(x + 8, r.y + r.h * 0.5f), icon_color ? icon_color : COL(ui, A3_UIC_TEXT_DIM), A3_FONT_UI);
        x += 22;
    }
    a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(x, r.y, r.x + r.w - x - 4, r.h), A3_ALIGN_LEFT, COL(ui, A3_UIC_TEXT), label);
    return b.clicked;
}

b32 a3_ui_selectable(A3Ui *ui, const char *label, b32 selected) { return a3_ui_selectable_ex(ui, label, label, selected, 0, 0); }

b32 a3_ui_tree_node(A3Ui *ui, const char *id_str, const char *label, u32 icon, u32 flags, b32 *clicked) {
    A3UiId id = a3_ui_id(ui, id_str);
    UiState *st = ui_state(ui, id, (flags & A3_TREE_DEFAULT_OPEN) ? 1 : 0, 0);
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height - 2);
    Behavior b = behave(ui, id, r);
    b32 leaf = (flags & A3_TREE_LEAF) != 0;
    A3Rect arrow = a3_rect(r.x, r.y, 18, r.h);
    b32 arrow_hit = !leaf && a3_rect_contains(arrow, ui->in.mouse_pos);
    if (b.pressed && arrow_hit) st->i = !st->i;
    if (b.double_clicked && !leaf && !arrow_hit) st->i = !st->i;
    if (clicked) *clicked = b.pressed && !arrow_hit;
    if (flags & A3_TREE_SELECTED) a3_ui_rect(ui, a3_rect(r.x - 4, r.y, r.w + 4, r.h), COL(ui, A3_UIC_SELECTION), ui->theme.rounding);
    else if (b.hovered) a3_ui_rect(ui, a3_rect(r.x - 4, r.y, r.w + 4, r.h), a3_color_alpha(COL(ui, A3_UIC_WIDGET_HOVER), 0.7f), ui->theme.rounding);
    if (!leaf) {
        A3Vec2 c = a3_v2(r.x + 8, r.y + r.h * 0.5f);
        u32 ac = COL(ui, A3_UIC_TEXT_DIM);
        if (st->i) a3_ui_triangle(ui, a3_v2(c.x - 4, c.y - 2), a3_v2(c.x + 4, c.y - 2), a3_v2(c.x, c.y + 3), ac);
        else a3_ui_triangle(ui, a3_v2(c.x - 2, c.y - 4), a3_v2(c.x + 3, c.y), a3_v2(c.x - 2, c.y + 4), ac);
    }
    f32 x = r.x + 18;
    if (icon) {
        a3_ui_icon(ui, icon, a3_v2(x + 7, r.y + r.h * 0.5f), (flags & A3_TREE_DIM) ? COL(ui, A3_UIC_TEXT_DISABLED) : COL(ui, A3_UIC_ACCENT_HOVER), A3_FONT_UI);
        x += 20;
    }
    a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(x, r.y, r.x + r.w - x, r.h), A3_ALIGN_LEFT,
                       (flags & A3_TREE_DIM) ? COL(ui, A3_UIC_TEXT_DISABLED) : COL(ui, A3_UIC_TEXT), label);
    ui->last_id = id;
    ui->last_rect = r;
    ui->last_hovered = b.hovered;
    b32 open = st->i && !leaf;
    if (open) a3_ui_indent(ui, ui->theme.indent);
    return open;
}

void a3_ui_tree_pop(A3Ui *ui) { a3_ui_indent(ui, -ui->theme.indent); }

b32 a3_ui_collapsing_header(A3Ui *ui, const char *id_str, const char *label, u32 icon, b32 default_open) {
    A3UiId id = a3_ui_id(ui, id_str);
    UiState *st = ui_state(ui, id, default_open ? 1 : 0, 0);
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height + 4);
    Behavior b = behave(ui, id, r);
    if (b.clicked) st->i = !st->i;
    a3_ui_rect(ui, r, b.hovered ? COL(ui, A3_UIC_WIDGET_HOVER) : COL(ui, A3_UIC_PANEL_ALT), ui->theme.rounding);
    A3Vec2 c = a3_v2(r.x + 12, r.y + r.h * 0.5f);
    u32 ac = COL(ui, A3_UIC_TEXT_DIM);
    if (st->i) a3_ui_triangle(ui, a3_v2(c.x - 4, c.y - 2), a3_v2(c.x + 4, c.y - 2), a3_v2(c.x, c.y + 3), ac);
    else a3_ui_triangle(ui, a3_v2(c.x - 2, c.y - 4), a3_v2(c.x + 3, c.y), a3_v2(c.x - 2, c.y + 4), ac);
    f32 x = r.x + 24;
    if (icon) { a3_ui_icon(ui, icon, a3_v2(x + 7, c.y), COL(ui, A3_UIC_ACCENT_HOVER), A3_FONT_UI); x += 22; }
    a3_ui_text_in_rect(ui, A3_FONT_UI_BOLD, a3_rect(x, r.y, r.w - x + r.x - 60, r.h), A3_ALIGN_LEFT, COL(ui, A3_UIC_TEXT), label);
    ui->last_id = id;
    ui->last_rect = r;
    ui->last_hovered = b.hovered;
    return st->i;
}

void a3_ui_progress_bar(A3Ui *ui, f32 fraction, const char *overlay) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height - 6);
    a3_ui_rect(ui, r, COL(ui, A3_UIC_WIDGET), r.h * 0.5f);
    f32 f = a3_saturate(fraction);
    if (f > 0) a3_ui_rect(ui, a3_rect(r.x, r.y, a3_maxf(r.w * f, r.h), r.h), COL(ui, A3_UIC_ACCENT), r.h * 0.5f);
    if (overlay) a3_ui_text_in_rect(ui, A3_FONT_UI, r, A3_ALIGN_CENTER, COL(ui, A3_UIC_TEXT), overlay);
}

void a3_ui_plot_lines(A3Ui *ui, const char *label, const f32 *values, u32 count, u32 offset, f32 min, f32 max, f32 height, u32 color) {
    A3Rect r = a3_ui_next_rect(ui, 0, height);
    a3_ui_rect(ui, r, COL(ui, A3_UIC_BG), ui->theme.rounding);
    if (count >= 2 && max > min) {
        A3Vec2 pts[512];
        u32 n = count > 512 ? 512 : count;
        for (u32 i = 0; i < n; ++i) {
            f32 v = values[(offset + count - n + i) % count];
            f32 t = a3_saturate((v - min) / (max - min));
            pts[i] = a3_v2(r.x + r.w * (f32)i / (f32)(n - 1), r.y + r.h - 2 - (r.h - 4) * t);
        }
        a3_ui_polyline(ui, pts, n, color, 1.5f, 0);
    }
    if (label) a3_ui_text(ui, A3_FONT_UI, a3_v2(r.x + 6, r.y + 3), COL(ui, A3_UIC_TEXT_DIM), label);
}

b32 a3_ui_tab_bar_item(A3Ui *ui, const char *id_str, const char *label, b32 selected, b32 *close_clicked) {
    f32 w = a3_font_text_width(A3_FONT_UI, label, -1) + (close_clicked ? 44 : 24);
    A3Rect r = a3_ui_next_rect(ui, w, ui->theme.row_height + 2);
    A3UiId id = a3_ui_id(ui, id_str);
    Behavior b = behave(ui, id, r);
    u32 bg = selected ? COL(ui, A3_UIC_PANEL) : (b.hovered ? COL(ui, A3_UIC_WIDGET_HOVER) : 0);
    if (bg) a3_ui_rect_corners(ui, r, bg, 5, 1 | 2);
    if (selected) a3_ui_rect(ui, a3_rect(r.x + 4, r.y, r.w - 8, 2), COL(ui, A3_UIC_ACCENT), 1);
    a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(r.x + 12, r.y, r.w - 12, r.h), A3_ALIGN_LEFT, selected ? COL(ui, A3_UIC_TEXT) : COL(ui, A3_UIC_TEXT_DIM), label);
    if (close_clicked) {
        *close_clicked = 0;
        A3Rect cr = a3_rect(r.x + r.w - 22, r.y + (r.h - 16) * 0.5f, 16, 16);
        b32 over = a3_rect_contains(cr, ui->in.mouse_pos) && b.hovered;
        if (over) a3_ui_rect(ui, cr, COL(ui, A3_UIC_WIDGET_ACTIVE), 3);
        if (over || selected) a3_ui_icon(ui, A3_ICON_CROSS, a3_v2(cr.x + 8, cr.y + 8), COL(ui, A3_UIC_TEXT_DIM), A3_FONT_UI);
        if (over && b.clicked) { *close_clicked = 1; return 0; }
    }
    if (b.hovered && ui->in.mouse_pressed[A3_MOUSE_MIDDLE] && close_clicked) *close_clicked = 1;
    return b.pressed;
}

/* ======================================================================== */
/* Popups & menus                                                           */
/* ======================================================================== */

static UiPopup *find_popup(A3Ui *ui, A3UiId id) {
    for (u32 i = 0; i < ui->popup_count; ++i) if (ui->popups[i].id == id) return &ui->popups[i];
    return 0;
}

void a3_ui_open_popup_at(A3Ui *ui, const char *id_str, A3Vec2 pos) {
    A3UiId id = a3_ui_id(ui, id_str);
    /* opening from a parent popup keeps the chain; otherwise start fresh */
    u32 keep = ui->popup_depth;
    if (keep > ui->popup_count) keep = ui->popup_count;
    ui->popup_count = keep;
    if (ui->popup_count >= 8) return;
    UiPopup *p = &ui->popups[ui->popup_count++];
    a3_zero_struct(p);
    p->id = id;
    p->pos = pos;
    p->opened_frame = ui->frame;
    p->parent = ui->popup_depth ? ui->popup_stack[ui->popup_depth - 1] : 0;
}

void a3_ui_open_popup(A3Ui *ui, const char *id) { a3_ui_open_popup_at(ui, id, ui->in.mouse_pos); }
b32 a3_ui_popup_open(A3Ui *ui, const char *id) { return find_popup(ui, a3_ui_id(ui, id)) != 0; }

b32 a3_ui_begin_popup(A3Ui *ui, const char *id_str, f32 width) {
    A3UiId id = a3_ui_id(ui, id_str);
    UiPopup *p = find_popup(ui, id);
    if (!p) return 0;
    if (ui->popup_depth >= 8) return 0;
    ui->popup_stack[ui->popup_depth++] = id;
    a3_ui_set_layer(ui, A3_LAYER_POPUP);
    UiPanelState *ps = panel_state(ui, id);
    f32 h = a3_minf(ps->content_h + 2 * 6, (f32)ui->height - 20);
    if (h < 20) h = 20;
    A3Rect r = a3_rect(p->pos.x, p->pos.y, width > 0 ? width : 220, h);
    if (r.x + r.w > ui->width - 4) r.x = (f32)ui->width - r.w - 4;
    if (r.y + r.h > ui->height - 4) r.y = a3_maxf((f32)ui->height - r.h - 4, 4);
    if (r.x < 4) r.x = 4;
    p->rect = r;
    a3_ui_shadow(ui, r, 14, 7, COL(ui, A3_UIC_SHADOW));
    a3_ui_rect(ui, r, COL(ui, A3_UIC_POPUP), 7);
    a3_ui_rect_outline(ui, r, COL(ui, A3_UIC_BORDER), 7, 1);
    UiPanel *saved_panels = 0;
    A3_UNUSED(saved_panels);
    ui->theme.padding -= 2;
    a3_ui_begin_panel(ui, id_str, r, A3_PANEL_NO_BACKGROUND);
    ui->theme.padding += 2;
    return 1;
}

void a3_ui_end_popup(A3Ui *ui) {
    a3_ui_end_panel(ui);
    if (ui->popup_depth) ui->popup_depth--;
    if (ui->popup_close_requested) {
        ui->popup_close_requested = 0;
        ui->popup_count = 0;
        ui->menubar_open = 0;
    }
    a3_ui_set_layer(ui, ui->popup_depth ? A3_LAYER_POPUP : (ui->in_modal ? A3_LAYER_POPUP : A3_LAYER_MAIN));
}

void a3_ui_close_popup(A3Ui *ui) { ui->popup_close_requested = 1; }

b32 a3_ui_begin_context_menu(A3Ui *ui, const char *id) {
    if (ui->last_hovered && ui->in.mouse_pressed[A3_MOUSE_RIGHT]) a3_ui_open_popup(ui, id);
    return a3_ui_begin_popup(ui, id, 230);
}

static b32 menu_row(A3Ui *ui, const char *label, const char *shortcut, b32 enabled, b32 checked, b32 submenu) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    A3UiId id = a3_ui_id(ui, label);
    Behavior b = behave(ui, id, r);
    if (b.hovered && enabled) a3_ui_rect(ui, r, COL(ui, A3_UIC_ACCENT), 4);
    u32 tc = !enabled ? COL(ui, A3_UIC_TEXT_DISABLED) : (b.hovered ? COL(ui, A3_UIC_ACCENT_TEXT) : COL(ui, A3_UIC_TEXT));
    if (checked) a3_ui_icon(ui, A3_ICON_CHECK, a3_v2(r.x + 11, r.y + r.h * 0.5f), tc, A3_FONT_UI);
    char text[128];
    a3_str_to_buf(a3_str_n(label, (usize)label_len(label)), text, sizeof(text));
    a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(r.x + 24, r.y, r.w - 30, r.h), A3_ALIGN_LEFT, tc, text);
    if (shortcut) a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(r.x, r.y, r.w - 8, r.h), A3_ALIGN_RIGHT, b.hovered ? tc : COL(ui, A3_UIC_TEXT_DIM), shortcut);
    if (submenu) {
        A3Vec2 c = a3_v2(r.x + r.w - 12, r.y + r.h * 0.5f);
        a3_ui_triangle(ui, a3_v2(c.x - 2, c.y - 4), a3_v2(c.x + 3, c.y), a3_v2(c.x - 2, c.y + 4), tc);
        if (b.hovered && enabled) return 1;
        return 0;
    }
    if (b.clicked && enabled) { a3_ui_close_popup(ui); return 1; }
    return 0;
}

b32 a3_ui_menu_item(A3Ui *ui, const char *label, const char *shortcut, b32 enabled) { return menu_row(ui, label, shortcut, enabled, 0, 0); }
b32 a3_ui_menu_item_check(A3Ui *ui, const char *label, const char *shortcut, b32 checked) { return menu_row(ui, label, shortcut, 1, checked, 0); }

b32 a3_ui_begin_menu(A3Ui *ui, const char *label) {
    char pid[160];
    a3_snprintf(pid, sizeof(pid), "submenu_%s", label);
    b32 hover = menu_row(ui, label, 0, 1, 0, 1);
    A3Rect r = ui->last_rect;
    A3UiId sub_id = a3_ui_id(ui, pid);
    if (hover && !find_popup(ui, sub_id)) {
        /* close sibling submenus deeper in the chain, then open this one */
        u32 depth = 0;
        for (u32 i = 0; i < ui->popup_count; ++i) if (ui->popups[i].id == ui->popup_stack[ui->popup_depth - 1]) depth = i + 1;
        ui->popup_count = depth;
        if (ui->popup_count < 8) {
            UiPopup *p = &ui->popups[ui->popup_count++];
            a3_zero_struct(p);
            p->id = sub_id;
            p->pos = a3_v2(r.x + r.w + 6, r.y - 4);
            p->opened_frame = ui->frame;
            p->is_menu_child = 1;
        }
    }
    return a3_ui_begin_popup(ui, pid, 230);
}

void a3_ui_end_menu(A3Ui *ui) { a3_ui_end_popup(ui); }

void a3_ui_menu_separator(A3Ui *ui) {
    A3Rect r = a3_ui_next_rect(ui, 0, 5);
    a3_ui_rect(ui, a3_rect(r.x + 4, r.y + 2, r.w - 8, 1), COL(ui, A3_UIC_BORDER), 0);
}

b32 a3_ui_begin_menubar(A3Ui *ui, A3Rect r) {
    ui->menubar_rect = r;
    ui->in_menubar = 1;
    ui->menubar_cursor = a3_v2(r.x + 6, r.y);
    a3_ui_rect(ui, r, COL(ui, A3_UIC_HEADER), 0);
    return 1;
}

b32 a3_ui_menubar_menu(A3Ui *ui, const char *label) {
    A3Rect mr = ui->menubar_rect;
    f32 w = a3_font_text_width(A3_FONT_UI, label, -1) + 18;
    A3Rect r = a3_rect(ui->menubar_cursor.x, mr.y + 2, w, mr.h - 4);
    ui->menubar_cursor.x += w + 2;
    char pid[96];
    a3_snprintf(pid, sizeof(pid), "menubar_%s", label);
    A3UiId id = a3_ui_id(ui, pid);
    Behavior b = behave(ui, id ^ 0x7777u, r);
    b32 open = find_popup(ui, id) != 0;
    if (b.pressed) {
        if (open) { ui->popup_count = 0; ui->menubar_open = 0; open = 0; }
        else { ui->popup_depth = 0; a3_ui_open_popup_at(ui, pid, a3_v2(r.x, mr.y + mr.h)); ui->menubar_open = id; open = 1; }
    } else if (b.hovered && ui->menubar_open && ui->menubar_open != id) {
        ui->popup_depth = 0;
        a3_ui_open_popup_at(ui, pid, a3_v2(r.x, mr.y + mr.h));
        ui->menubar_open = id;
        open = 1;
    }
    if (open || b.hovered) a3_ui_rect(ui, r, open ? COL(ui, A3_UIC_WIDGET_ACTIVE) : COL(ui, A3_UIC_WIDGET_HOVER), 4);
    a3_ui_text_in_rect(ui, A3_FONT_UI, r, A3_ALIGN_CENTER, COL(ui, A3_UIC_TEXT), label);
    if (!open) return 0;
    return a3_ui_begin_popup(ui, pid, 260);
}

void a3_ui_end_menubar(A3Ui *ui) { ui->in_menubar = 0; }

void a3_ui_open_modal(A3Ui *ui, const char *id) { ui->modal_open = a3_ui_id(ui, id); ui->popup_count = 0; }
void a3_ui_close_modal(A3Ui *ui) { ui->modal_open = 0; }

b32 a3_ui_begin_modal(A3Ui *ui, const char *id_str, const char *title, f32 w, f32 h) {
    A3UiId id = a3_ui_id(ui, id_str);
    if (ui->modal_open != id) return 0;
    ui->in_modal = 1;
    ui->modal_current = id;
    a3_ui_set_layer(ui, A3_LAYER_POPUP);
    a3_ui_rect(ui, a3_rect(0, 0, (f32)ui->width, (f32)ui->height), a3_rgba(0, 0, 0, 120), 0);
    A3Rect r = a3_rect(((f32)ui->width - w) * 0.5f, ((f32)ui->height - h) * 0.5f, w, h);
    a3_ui_shadow(ui, r, 24, 10, COL(ui, A3_UIC_SHADOW));
    a3_ui_rect(ui, r, COL(ui, A3_UIC_PANEL), 10);
    a3_ui_rect_outline(ui, r, COL(ui, A3_UIC_BORDER), 10, 1);
    A3Rect tr = a3_rect(r.x, r.y, r.w, 40);
    a3_ui_text_in_rect(ui, A3_FONT_HEADING, a3_rect(tr.x + 16, tr.y, tr.w - 60, tr.h), A3_ALIGN_LEFT, COL(ui, A3_UIC_TEXT), title);
    A3Rect close = a3_rect(r.x + r.w - 34, r.y + 8, 24, 24);
    Behavior cb = behave(ui, id ^ 0xC105Eu, close);
    if (cb.hovered) a3_ui_rect(ui, close, COL(ui, A3_UIC_WIDGET_HOVER), 5);
    a3_ui_icon(ui, A3_ICON_CROSS, a3_v2(close.x + 12, close.y + 12), COL(ui, A3_UIC_TEXT_DIM), A3_FONT_UI);
    if (cb.clicked || ui->in.keys_pressed[A3_KEY_ESCAPE]) ui->modal_open = 0;
    a3_ui_rect(ui, a3_rect(r.x, r.y + 40, r.w, 1), COL(ui, A3_UIC_BORDER), 0);
    a3_ui_begin_panel(ui, id_str, a3_rect(r.x + 4, r.y + 44, r.w - 8, r.h - 48), A3_PANEL_NO_BACKGROUND);
    return 1;
}

void a3_ui_end_modal(A3Ui *ui) {
    a3_ui_end_panel(ui);
    ui->in_modal = 0;
    a3_ui_set_layer(ui, A3_LAYER_MAIN);
}

/* ======================================================================== */
/* Combo & color                                                            */
/* ======================================================================== */

b32 a3_ui_combo(A3Ui *ui, const char *id_str, i32 *current, const char *const *items, u32 count) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    A3UiId id = a3_ui_id(ui, id_str);
    Behavior b = behave(ui, id, r);
    char pid[96];
    a3_snprintf(pid, sizeof(pid), "combo_%s", id_str);
    if (b.clicked) a3_ui_open_popup_at(ui, pid, a3_v2(r.x, r.y + r.h + 2));
    a3_ui_rect(ui, r, b.hovered ? COL(ui, A3_UIC_WIDGET_HOVER) : COL(ui, A3_UIC_WIDGET), ui->theme.rounding);
    const char *cur = (*current >= 0 && (u32)*current < count) ? items[*current] : "(none)";
    a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(r.x + 8, r.y, r.w - 30, r.h), A3_ALIGN_LEFT, COL(ui, A3_UIC_TEXT), cur);
    A3Vec2 c = a3_v2(r.x + r.w - 13, r.y + r.h * 0.5f);
    a3_ui_triangle(ui, a3_v2(c.x - 4, c.y - 2), a3_v2(c.x + 4, c.y - 2), a3_v2(c.x, c.y + 3), COL(ui, A3_UIC_TEXT_DIM));
    b32 changed = 0;
    A3Rect saved = r;
    if (a3_ui_begin_popup(ui, pid, r.w)) {
        for (u32 i = 0; i < count; ++i) {
            a3_ui_push_id_int(ui, i);
            if (menu_row(ui, items[i], 0, 1, (i32)i == *current, 0)) { *current = (i32)i; changed = 1; }
            a3_ui_pop_id(ui);
        }
        a3_ui_end_popup(ui);
    }
    ui->last_rect = saved;
    return changed;
}

static void hsv_to_rgb(f32 h, f32 s, f32 v, f32 *r, f32 *g, f32 *b) {
    h = a3_fmodf(h, 1.0f) * 6.0f;
    if (h < 0) h += 6.0f;
    i32 i = (i32)h;
    f32 f = h - (f32)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

static void rgb_to_hsv(f32 r, f32 g, f32 b, f32 *h, f32 *s, f32 *v) {
    f32 mx = a3_maxf(r, a3_maxf(g, b)), mn = a3_minf(r, a3_minf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 1e-6f) { *h = 0; return; }
    if (mx == r) *h = (g - b) / d / 6.0f;
    else if (mx == g) *h = ((b - r) / d + 2.0f) / 6.0f;
    else *h = ((r - g) / d + 4.0f) / 6.0f;
    if (*h < 0) *h += 1.0f;
}

b32 a3_ui_color_edit(A3Ui *ui, const char *id_str, A3Vec4 *c, b32 alpha) {
    A3Rect r = a3_ui_next_rect(ui, 0, ui->theme.row_height);
    A3UiId id = a3_ui_id(ui, id_str);
    Behavior b = behave(ui, id, r);
    char pid[96];
    a3_snprintf(pid, sizeof(pid), "color_%s", id_str);
    if (b.clicked) a3_ui_open_popup_at(ui, pid, a3_v2(r.x, r.y + r.h + 2));
    /* swatch with checkerboard behind alpha */
    a3_ui_rect(ui, r, COL(ui, A3_UIC_WIDGET), ui->theme.rounding);
    A3Rect sw = a3_rect(r.x + 3, r.y + 3, 38, r.h - 6);
    a3_ui_rect(ui, sw, 0xFFFFFFFFu, 3);
    a3_ui_rect(ui, a3_rect(sw.x, sw.y, sw.w * 0.5f, sw.h * 0.5f), a3_rgb(190, 190, 190), 0);
    a3_ui_rect(ui, a3_rect(sw.x + sw.w * 0.5f, sw.y + sw.h * 0.5f, sw.w * 0.5f, sw.h * 0.5f), a3_rgb(190, 190, 190), 0);
    a3_ui_rect(ui, sw, a3_color_from_vec4(*c), 3);
    char hex[24];
    a3_snprintf(hex, sizeof(hex), "#%02X%02X%02X", (u32)(a3_saturate(c->x) * 255 + 0.5f), (u32)(a3_saturate(c->y) * 255 + 0.5f), (u32)(a3_saturate(c->z) * 255 + 0.5f));
    a3_ui_text_in_rect(ui, A3_FONT_MONO, a3_rect(sw.x + sw.w + 8, r.y, r.w - sw.w - 12, r.h), A3_ALIGN_LEFT, COL(ui, A3_UIC_TEXT_DIM), hex);
    b32 changed = 0;
    A3Rect saved = r;
    if (a3_ui_begin_popup(ui, pid, 240)) {
        UiState *st = ui_state(ui, id ^ 0x45u, 0, -1.0f);
        f32 h, s, v;
        rgb_to_hsv(c->x, c->y, c->z, &h, &s, &v);
        if (st->f >= 0 && s < 1e-4f) h = st->f; /* keep hue when saturation is 0 */
        A3Rect sq = a3_ui_next_rect(ui, 0, 150);
        A3Rect hue = a3_ui_next_rect(ui, 0, 14);
        f32 hr, hg, hb;
        hsv_to_rgb(h, 1, 1, &hr, &hg, &hb);
        a3_ui_rect_gradient_h(ui, sq, 0xFFFFFFFFu, a3_color_from_vec4(a3_v4(hr, hg, hb, 1)));
        a3_ui_rect_gradient(ui, sq, a3_rgba(0, 0, 0, 0), a3_rgba(0, 0, 0, 255));
        Behavior sb = behave(ui, id ^ 0x5Au, sq);
        if (sb.held) {
            s = a3_saturate((ui->in.mouse_pos.x - sq.x) / sq.w);
            v = 1.0f - a3_saturate((ui->in.mouse_pos.y - sq.y) / sq.h);
            changed = 1;
        }
        a3_ui_circle_outline(ui, a3_v2(sq.x + s * sq.w, sq.y + (1 - v) * sq.h), 5, 0xFFFFFFFFu, 2);
        for (int k = 0; k < 6; ++k) {
            f32 r0, g0, b0, r1, g1, b1;
            hsv_to_rgb((f32)k / 6.0f, 1, 1, &r0, &g0, &b0);
            hsv_to_rgb((f32)(k + 1) / 6.0f, 1, 1, &r1, &g1, &b1);
            a3_ui_rect_gradient_h(ui, a3_rect(hue.x + hue.w * (f32)k / 6.0f, hue.y, hue.w / 6.0f + 0.5f, hue.h),
                                  a3_color_from_vec4(a3_v4(r0, g0, b0, 1)), a3_color_from_vec4(a3_v4(r1, g1, b1, 1)));
        }
        Behavior hb2 = behave(ui, id ^ 0xB0u, hue);
        if (hb2.held) { h = a3_saturate((ui->in.mouse_pos.x - hue.x) / hue.w) * 0.9999f; changed = 1; }
        a3_ui_rect_outline(ui, a3_rect(hue.x + h * hue.w - 3, hue.y - 2, 6, hue.h + 4), 0xFFFFFFFFu, 2, 2);
        if (changed) {
            hsv_to_rgb(h, s, v, &c->x, &c->y, &c->z);
            st->f = h;
        }
        if (alpha) {
            a3_ui_property(ui, "Alpha", 0);
            if (a3_ui_slider_float(ui, "alpha", &c->w, 0, 1, "%.2f")) changed = 1;
        }
        a3_ui_property(ui, "RGB", 0);
        f32 rgb[3] = { c->x, c->y, c->z };
        if (a3_ui_drag_float_n(ui, "rgb", rgb, 3, 0.005f, 0, 1, "%.2f")) { c->x = rgb[0]; c->y = rgb[1]; c->z = rgb[2]; changed = 1; }
        a3_ui_end_popup(ui);
    }
    ui->last_rect = saved;
    return changed;
}

/* ======================================================================== */
/* Drag & drop                                                              */
/* ======================================================================== */

b32 a3_ui_drag_source(A3Ui *ui, const char *type, const void *payload, u32 size, const char *label) {
    if (ui->dragging) return ui->drag_source == ui->last_id;
    if (ui->active != ui->last_id || !ui->in.mouse[A3_MOUSE_LEFT]) return 0;
    A3Vec2 d = a3_v2_sub(ui->in.mouse_pos, ui->drag_start_mouse);
    if (a3_v2_dot(d, d) < 36.0f) return 0;
    ui->dragging = 1;
    ui->drag_source = ui->last_id;
    a3_strcpy(ui->drag_type, sizeof(ui->drag_type), type);
    ui->drag_size = size < sizeof(ui->drag_payload) ? size : (u32)sizeof(ui->drag_payload);
    if (payload && size) a3_memcpy(ui->drag_payload, payload, ui->drag_size);
    a3_strcpy(ui->drag_label, sizeof(ui->drag_label), label ? label : "");
    return 1;
}

b32 a3_ui_dragging(A3Ui *ui, const char *type) { return ui->dragging && (!type || a3_streq(ui->drag_type, type)); }

const void *a3_ui_drag_payload(A3Ui *ui, const char *type, u32 *size) {
    if (!a3_ui_dragging(ui, type)) return 0;
    if (size) *size = ui->drag_size;
    return ui->drag_payload;
}

const void *a3_ui_drop_target_rect(A3Ui *ui, A3Rect r, const char *type, u32 *size) {
    b32 active_drag = ui->dragging || ui->drag_released;
    if (!active_drag || !a3_streq(ui->drag_type, type)) return 0;
    if (!a3_rect_contains(r, ui->in.mouse_pos) || blocked_by_popup(ui)) return 0;
    a3_ui_rect_outline(ui, a3_rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2), COL(ui, A3_UIC_ACCENT), ui->theme.rounding, 2);
    if (ui->drag_released) {
        ui->drag_released = 0;
        if (size) *size = ui->drag_size;
        return ui->drag_payload;
    }
    return 0;
}

const void *a3_ui_drop_target(A3Ui *ui, const char *type, u32 *size) { return a3_ui_drop_target_rect(ui, ui->last_rect, type, size); }

/* ======================================================================== */
/* Splitter & notifications                                                 */
/* ======================================================================== */

b32 a3_ui_splitter(A3Ui *ui, const char *id, A3Rect r, b32 vertical, f32 *value) {
    Behavior b = behave(ui, a3_ui_id(ui, id), r);
    if (b.hovered || b.held) {
        ui->cursor = vertical ? A3_CURSOR_RESIZE_H : A3_CURSOR_RESIZE_V;
        a3_ui_rect(ui, r, a3_color_alpha(COL(ui, A3_UIC_ACCENT), b.held ? 0.8f : 0.4f), 0);
    }
    if (b.held) {
        *value += vertical ? ui->in.mouse_delta.x : ui->in.mouse_delta.y;
        return 1;
    }
    return 0;
}

void a3_ui_notify(A3Ui *ui, u32 color, const char *fmt, ...) {
    if (ui->toast_count >= 6) {
        a3_memmove(&ui->toasts[0], &ui->toasts[1], sizeof(UiToast) * 5);
        ui->toast_count = 5;
    }
    UiToast *t = &ui->toasts[ui->toast_count++];
    va_list a;
    va_start(a, fmt);
    a3_vsnprintf(t->text, sizeof(t->text), fmt, a);
    va_end(a);
    t->color = color ? color : COL(ui, A3_UIC_ACCENT);
    t->time = 4.0f;
}
