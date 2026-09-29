/*
 * ASM3D - a3_ui.h
 * Immediate-mode UI toolkit used by the editor (and available to games for
 * menus and HUDs). Anti-aliased vector drawing, embedded fonts, themes,
 * widgets, popups/menus, drag and drop, scroll regions and text editing.
 *
 * Usage each frame:
 *   a3_ui_begin_frame(ui, input, width, height, dt);
 *   if (a3_ui_begin_panel(ui, "Inspector", rect, 0)) { ... widgets ...; }
 *   a3_ui_end_panel(ui);
 *   a3_ui_end_frame(ui);
 *   a3_ui_render(ui);   // after the 3D scene, on the window target
 */
#ifndef A3_UI_H
#define A3_UI_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"
#include "../input/a3_input.h"
#include "../render/a3_rhi.h"
#include "a3_font.h"

A3_EXTERN_C_BEGIN

typedef struct A3Ui A3Ui;
typedef u32 A3UiId;
typedef struct A3Rect { f32 x, y, w, h; } A3Rect;

A3_INLINE A3Rect a3_rect(f32 x, f32 y, f32 w, f32 h) { A3Rect r = { x, y, w, h }; return r; }
A3_INLINE b32 a3_rect_contains(A3Rect r, A3Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }
A3_INLINE A3Rect a3_rect_shrink(A3Rect r, f32 d) { return a3_rect(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d); }
A3Rect a3_rect_intersect(A3Rect a, A3Rect b);

/* Colors are packed 0xAABBGGRR (little-endian RGBA bytes). */
A3_INLINE u32 a3_rgba(u8 r, u8 g, u8 b, u8 a) { return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24); }
A3_INLINE u32 a3_rgb(u8 r, u8 g, u8 b) { return a3_rgba(r, g, b, 255); }
u32 a3_color_from_vec4(A3Vec4 c);
A3Vec4 a3_color_to_vec4(u32 c);
u32 a3_color_alpha(u32 c, f32 alpha_mul);
u32 a3_color_lerp(u32 a, u32 b, f32 t);

typedef enum A3UiColor {
    A3_UIC_BG = 0,          /* window background */
    A3_UIC_PANEL,           /* panel body */
    A3_UIC_PANEL_ALT,       /* alternate rows / nested areas */
    A3_UIC_HEADER,          /* panel title bars, menubar */
    A3_UIC_BORDER,
    A3_UIC_TEXT,
    A3_UIC_TEXT_DIM,
    A3_UIC_TEXT_DISABLED,
    A3_UIC_WIDGET,          /* buttons, fields */
    A3_UIC_WIDGET_HOVER,
    A3_UIC_WIDGET_ACTIVE,
    A3_UIC_ACCENT,          /* selection, primary buttons, sliders */
    A3_UIC_ACCENT_HOVER,
    A3_UIC_ACCENT_TEXT,
    A3_UIC_SELECTION,       /* selected row / text selection */
    A3_UIC_POPUP,
    A3_UIC_SHADOW,
    A3_UIC_SCROLLBAR,
    A3_UIC_SUCCESS,
    A3_UIC_WARNING,
    A3_UIC_ERROR,
    A3_UIC_AXIS_X,
    A3_UIC_AXIS_Y,
    A3_UIC_AXIS_Z,
    A3_UIC_COUNT
} A3UiColor;

typedef struct A3UiTheme {
    u32 colors[A3_UIC_COUNT];
    f32 rounding;        /* widget corner radius */
    f32 panel_rounding;
    f32 padding;         /* inner padding of panels */
    f32 spacing;         /* between widgets */
    f32 row_height;      /* default widget height */
    f32 indent;
    f32 label_width;     /* inspector label column (fraction if <= 1) */
    f32 scrollbar_width;
} A3UiTheme;

void a3_ui_theme_dark(A3UiTheme *t);
void a3_ui_theme_light(A3UiTheme *t);

typedef enum A3UiLayer { A3_LAYER_BACK = 0, A3_LAYER_MAIN, A3_LAYER_POPUP, A3_LAYER_OVERLAY, A3_LAYER_COUNT } A3UiLayer;

/* ---- Context ---- */
typedef void (*A3UiSetClipboard)(void *user, const char *text);
typedef const char *(*A3UiGetClipboard)(void *user);

A3Ui *a3_ui_create(void);                     /* requires an initialized RHI for rendering */
void  a3_ui_destroy(A3Ui *ui);
A3UiTheme *a3_ui_theme(A3Ui *ui);
void  a3_ui_set_clipboard_fns(A3Ui *ui, A3UiSetClipboard set, A3UiGetClipboard get, void *user);
void  a3_ui_begin_frame(A3Ui *ui, const A3InputState *input, i32 width, i32 height, f32 dt);
void  a3_ui_end_frame(A3Ui *ui);
void  a3_ui_render(A3Ui *ui);                 /* draws to the bound render target */
A3Cursor a3_ui_cursor(A3Ui *ui);
b32   a3_ui_wants_mouse(A3Ui *ui);            /* pointer is over UI or dragging UI */
b32   a3_ui_wants_keyboard(A3Ui *ui);         /* a text field has focus */
/* Custom text widgets (code editor, node graph search) take keyboard focus
 * with claim; it is released automatically when another widget is clicked. */
void  a3_ui_claim_keyboard(A3Ui *ui, A3UiId id);
b32   a3_ui_has_keyboard(A3Ui *ui, A3UiId id);
void  a3_ui_release_keyboard(A3Ui *ui, A3UiId id);
void  a3_ui_set_clipboard(A3Ui *ui, const char *text);
const char *a3_ui_get_clipboard(A3Ui *ui);
f64   a3_ui_time(A3Ui *ui);
A3Vec2 a3_ui_mouse(A3Ui *ui);
const A3InputState *a3_ui_input(A3Ui *ui);
u32   a3_ui_vertex_count(A3Ui *ui);

/* ---- IDs ---- */
A3UiId a3_ui_id(A3Ui *ui, const char *str);
void  a3_ui_push_id(A3Ui *ui, const char *str);
void  a3_ui_push_id_int(A3Ui *ui, i64 v);
void  a3_ui_pop_id(A3Ui *ui);

/* ---- Drawing ---- */
void  a3_ui_set_layer(A3Ui *ui, A3UiLayer layer);
A3UiLayer a3_ui_layer(A3Ui *ui);
void  a3_ui_push_clip(A3Ui *ui, A3Rect r);
void  a3_ui_pop_clip(A3Ui *ui);
A3Rect a3_ui_clip(A3Ui *ui);
void  a3_ui_rect(A3Ui *ui, A3Rect r, u32 color, f32 rounding);
void  a3_ui_rect_corners(A3Ui *ui, A3Rect r, u32 color, f32 rounding, u32 corner_mask); /* 1 TL, 2 TR, 4 BR, 8 BL */
void  a3_ui_rect_outline(A3Ui *ui, A3Rect r, u32 color, f32 rounding, f32 thickness);
void  a3_ui_rect_gradient(A3Ui *ui, A3Rect r, u32 top, u32 bottom);
void  a3_ui_rect_gradient_h(A3Ui *ui, A3Rect r, u32 left, u32 right);
void  a3_ui_shadow(A3Ui *ui, A3Rect r, f32 size, f32 rounding, u32 color);
void  a3_ui_line(A3Ui *ui, A3Vec2 a, A3Vec2 b, u32 color, f32 thickness);
void  a3_ui_polyline(A3Ui *ui, const A3Vec2 *pts, u32 count, u32 color, f32 thickness, b32 closed);
void  a3_ui_triangle(A3Ui *ui, A3Vec2 a, A3Vec2 b, A3Vec2 c, u32 color);
void  a3_ui_circle(A3Ui *ui, A3Vec2 c, f32 radius, u32 color);
void  a3_ui_circle_outline(A3Ui *ui, A3Vec2 c, f32 radius, u32 color, f32 thickness);
void  a3_ui_bezier(A3Ui *ui, A3Vec2 p0, A3Vec2 p1, A3Vec2 p2, A3Vec2 p3, u32 color, f32 thickness);
f32   a3_ui_text(A3Ui *ui, A3FontId font, A3Vec2 pos, u32 color, const char *text);            /* pos = top-left */
f32   a3_ui_text_n(A3Ui *ui, A3FontId font, A3Vec2 pos, u32 color, const char *text, i32 len);
typedef enum A3Align { A3_ALIGN_LEFT = 0, A3_ALIGN_CENTER, A3_ALIGN_RIGHT } A3Align;
void  a3_ui_text_in_rect(A3Ui *ui, A3FontId font, A3Rect r, A3Align align, u32 color, const char *text);
/* Word-wrapped text; returns height used. */
f32   a3_ui_text_wrapped(A3Ui *ui, A3FontId font, A3Rect r, u32 color, const char *text);
f32   a3_ui_text_wrapped_height(A3FontId font, f32 width, const char *text);
void  a3_ui_icon(A3Ui *ui, u32 icon, A3Vec2 center, u32 color, A3FontId font);
void  a3_ui_image(A3Ui *ui, A3RhiTexture tex, A3Rect r, A3Vec2 uv0, A3Vec2 uv1, u32 tint);
/* Procedural icons that fonts lack. */
void  a3_ui_draw_icon_search(A3Ui *ui, A3Vec2 c, f32 size, u32 color);
void  a3_ui_draw_icon_pause(A3Ui *ui, A3Vec2 c, f32 size, u32 color);
void  a3_ui_draw_icon_step(A3Ui *ui, A3Vec2 c, f32 size, u32 color);
void  a3_ui_draw_icon_folder(A3Ui *ui, A3Vec2 c, f32 size, u32 color);
void  a3_ui_draw_icon_file(A3Ui *ui, A3Vec2 c, f32 size, u32 color);

/* ---- Panels (scrollable regions) and layout ---- */
typedef enum A3PanelFlags {
    A3_PANEL_NONE = 0,
    A3_PANEL_NO_SCROLL = 1 << 0,
    A3_PANEL_NO_BACKGROUND = 1 << 1,
    A3_PANEL_NO_PADDING = 1 << 2,
    A3_PANEL_BORDER = 1 << 3,
    A3_PANEL_HORIZONTAL_SCROLL = 1 << 4,
} A3PanelFlags;

b32   a3_ui_begin_panel(A3Ui *ui, const char *id, A3Rect r, u32 flags);
void  a3_ui_end_panel(A3Ui *ui);
A3Rect a3_ui_panel_rect(A3Ui *ui);               /* inner rect of current panel */
f32   a3_ui_content_width(A3Ui *ui);             /* remaining width on the current line */
A3Rect a3_ui_next_rect(A3Ui *ui, f32 w, f32 h);  /* w <= 0: fill (minus -w) */
void  a3_ui_same_line(A3Ui *ui);
void  a3_ui_set_next_width(A3Ui *ui, f32 w);     /* width of the next widget that would fill the line */
void  a3_ui_spacing(A3Ui *ui, f32 h);
void  a3_ui_separator(A3Ui *ui);
void  a3_ui_indent(A3Ui *ui, f32 w);
void  a3_ui_scroll_to_top(A3Ui *ui);
void  a3_ui_scroll_here(A3Ui *ui);               /* make the next item visible */
A3Vec2 a3_ui_cursor_pos(A3Ui *ui);
void  a3_ui_set_cursor_pos(A3Ui *ui, A3Vec2 p);

/* Inspector style two-column rows: label on the left, widget on the right. */
void  a3_ui_property(A3Ui *ui, const char *label, const char *tooltip);

/* ---- Item state (applies to the last widget) ---- */
A3Rect a3_ui_last_rect(A3Ui *ui);
A3UiId a3_ui_last_id(A3Ui *ui);
b32   a3_ui_item_hovered(A3Ui *ui);
b32   a3_ui_item_clicked(A3Ui *ui, A3MouseButton b);
b32   a3_ui_item_double_clicked(A3Ui *ui);
b32   a3_ui_item_active(A3Ui *ui);
b32   a3_ui_item_released(A3Ui *ui);             /* active item released this frame (end of an edit) */
void  a3_ui_tooltip(A3Ui *ui, const char *text); /* shown after hovering the last item */
void  a3_ui_tooltip_now(A3Ui *ui, const char *text);

/* ---- Widgets ---- */
typedef enum A3ButtonFlags {
    A3_BUTTON_NONE = 0,
    A3_BUTTON_PRIMARY = 1 << 0,
    A3_BUTTON_DANGER = 1 << 1,
    A3_BUTTON_FLAT = 1 << 2,
    A3_BUTTON_TOGGLED = 1 << 3,
    A3_BUTTON_DISABLED = 1 << 4,
    A3_BUTTON_SMALL = 1 << 5,
} A3ButtonFlags;

b32  a3_ui_button(A3Ui *ui, const char *label);
b32  a3_ui_button_ex(A3Ui *ui, const char *label, f32 width, u32 flags);
b32  a3_ui_button_rect(A3Ui *ui, const char *id_label, A3Rect r, u32 flags);
b32  a3_ui_icon_button(A3Ui *ui, u32 icon, const char *tooltip, u32 flags);
b32  a3_ui_invisible_button(A3Ui *ui, const char *id, A3Rect r);
void a3_ui_label(A3Ui *ui, const char *fmt, ...) A3_PRINTF_LIKE(2, 3);
void a3_ui_label_colored(A3Ui *ui, u32 color, const char *fmt, ...) A3_PRINTF_LIKE(3, 4);
void a3_ui_label_font(A3Ui *ui, A3FontId font, u32 color, const char *text);
void a3_ui_label_wrapped(A3Ui *ui, const char *text);
void a3_ui_heading(A3Ui *ui, const char *text);
b32  a3_ui_checkbox(A3Ui *ui, const char *label, b32 *v);
b32  a3_ui_toggle(A3Ui *ui, const char *id, b32 *v);          /* switch style */
b32  a3_ui_slider_float(A3Ui *ui, const char *id, f32 *v, f32 min, f32 max, const char *fmt);
b32  a3_ui_slider_int(A3Ui *ui, const char *id, i32 *v, i32 min, i32 max);
b32  a3_ui_drag_float(A3Ui *ui, const char *id, f32 *v, f32 speed, f32 min, f32 max, const char *fmt);
b32  a3_ui_drag_float_n(A3Ui *ui, const char *id, f32 *v, u32 n, f32 speed, f32 min, f32 max, const char *fmt);
b32  a3_ui_drag_int(A3Ui *ui, const char *id, i32 *v, f32 speed, i32 min, i32 max);
typedef enum A3InputFlags {
    A3_INPUT_NONE = 0,
    A3_INPUT_ENTER_RETURNS = 1 << 0,   /* return true only on Enter / focus loss */
    A3_INPUT_SELECT_ALL = 1 << 1,
    A3_INPUT_NUMERIC = 1 << 2,
    A3_INPUT_FOCUS = 1 << 3,           /* grab keyboard focus this frame */
    A3_INPUT_SEARCH = 1 << 4,          /* search icon + placeholder */
} A3InputFlags;
b32  a3_ui_input_text(A3Ui *ui, const char *id, char *buf, usize cap, u32 flags, const char *placeholder);
b32  a3_ui_combo(A3Ui *ui, const char *id, i32 *current, const char *const *items, u32 count);
b32  a3_ui_color_edit(A3Ui *ui, const char *id, A3Vec4 *rgba, b32 alpha);
b32  a3_ui_selectable(A3Ui *ui, const char *label, b32 selected);
b32  a3_ui_selectable_ex(A3Ui *ui, const char *id, const char *label, b32 selected, u32 icon, u32 icon_color);
typedef enum A3TreeFlags { A3_TREE_NONE = 0, A3_TREE_SELECTED = 1, A3_TREE_LEAF = 2, A3_TREE_DEFAULT_OPEN = 4, A3_TREE_DIM = 8 } A3TreeFlags;
/* Returns open state. *clicked set when the label (not the arrow) was clicked. */
b32  a3_ui_tree_node(A3Ui *ui, const char *id, const char *label, u32 icon, u32 flags, b32 *clicked);
void a3_ui_tree_pop(A3Ui *ui);
b32  a3_ui_collapsing_header(A3Ui *ui, const char *id, const char *label, u32 icon, b32 default_open);
void a3_ui_progress_bar(A3Ui *ui, f32 fraction, const char *overlay);
/* Value history graph (profiler). */
void a3_ui_plot_lines(A3Ui *ui, const char *label, const f32 *values, u32 count, u32 offset, f32 min, f32 max, f32 height, u32 color);
b32  a3_ui_tab_bar_item(A3Ui *ui, const char *id, const char *label, b32 selected, b32 *close_clicked);

/* ---- Popups, menus, modals ---- */
void a3_ui_open_popup(A3Ui *ui, const char *id);
void a3_ui_open_popup_at(A3Ui *ui, const char *id, A3Vec2 pos);
b32  a3_ui_popup_open(A3Ui *ui, const char *id);
b32  a3_ui_begin_popup(A3Ui *ui, const char *id, f32 width);
void a3_ui_end_popup(A3Ui *ui);
void a3_ui_close_popup(A3Ui *ui);
/* Opens on right-click over the last item (or the rect when given). */
b32  a3_ui_begin_context_menu(A3Ui *ui, const char *id);
b32  a3_ui_menu_item(A3Ui *ui, const char *label, const char *shortcut, b32 enabled);
b32  a3_ui_menu_item_check(A3Ui *ui, const char *label, const char *shortcut, b32 checked);
b32  a3_ui_begin_menu(A3Ui *ui, const char *label);       /* submenu inside a popup */
void a3_ui_end_menu(A3Ui *ui);
void a3_ui_menu_separator(A3Ui *ui);
b32  a3_ui_begin_menubar(A3Ui *ui, A3Rect r);
b32  a3_ui_menubar_menu(A3Ui *ui, const char *label);     /* follow with a3_ui_end_popup when true */
void a3_ui_end_menubar(A3Ui *ui);
b32  a3_ui_begin_modal(A3Ui *ui, const char *id, const char *title, f32 w, f32 h);
void a3_ui_end_modal(A3Ui *ui);
void a3_ui_open_modal(A3Ui *ui, const char *id);
void a3_ui_close_modal(A3Ui *ui);

/* ---- Drag and drop ---- */
b32  a3_ui_drag_source(A3Ui *ui, const char *type, const void *payload, u32 size, const char *label);
const void *a3_ui_drop_target(A3Ui *ui, const char *type, u32 *size);     /* over the last item */
const void *a3_ui_drop_target_rect(A3Ui *ui, A3Rect r, const char *type, u32 *size);
b32  a3_ui_dragging(A3Ui *ui, const char *type);
const void *a3_ui_drag_payload(A3Ui *ui, const char *type, u32 *size);

/* ---- Splitters ---- */
b32  a3_ui_splitter(A3Ui *ui, const char *id, A3Rect r, b32 vertical, f32 *value);

/* ---- Notifications (toasts) ---- */
void a3_ui_notify(A3Ui *ui, u32 color, const char *fmt, ...) A3_PRINTF_LIKE(3, 4);

A3_EXTERN_C_END

#endif
