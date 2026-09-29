/*
 * ASM3D - a3_ui_internal.h
 */
#ifndef A3_UI_INTERNAL_H
#define A3_UI_INTERNAL_H

#include "a3_ui.h"
#include "../core/a3_memory.h"
#include "../core/a3_hash.h"

typedef struct UiVertex { f32 x, y, u, v; u32 col; } UiVertex;
typedef struct UiCmd { u32 first, count; A3Rect clip; u32 tex; u32 mode; } UiCmd;

typedef struct UiDrawList {
    A3_ARRAY_TYPE(UiVertex) v;
    A3_ARRAY_TYPE(u32) i;
    A3_ARRAY_TYPE(UiCmd) cmds;
    A3Rect clip_stack[64];
    u32 clip_depth;
} UiDrawList;

typedef struct UiPanel {
    A3UiId id;
    A3Rect rect;           /* outer */
    A3Rect inner;          /* content area (minus padding / scrollbar) */
    A3Vec2 cursor;         /* next item position */
    f32 line_h;            /* height of the current line */
    b32 same_line;
    b32 cursor_x_set;      /* set_cursor_pos: next item starts at cursor.x instead of the left edge */
    f32 indent;
    f32 content_bottom;    /* lowest item bottom (for scrolling) */
    f32 content_right;
    u32 flags;
    f32 scroll_y, scroll_x;
    b32 property;          /* next item goes into the property value column */
    A3Rect property_rect;
    A3Vec2 last_line_pos;  /* where the previous item started (for same_line) */
    f32 last_item_right;
    f32 last_item_h;
} UiPanel;

typedef struct UiPanelState { A3UiId id; f32 scroll_y, scroll_x, content_h, content_w, view_h, view_w; u32 frame; } UiPanelState;
typedef struct UiState { A3UiId id; i32 i; f32 f; u32 frame; } UiState;

typedef struct UiPopup {
    A3UiId id;
    A3Vec2 pos;
    A3Rect rect;           /* rect last frame (for hover blocking) */
    u32 opened_frame;
    b32 is_menu_child;
    A3UiId parent;
} UiPopup;

typedef struct UiToast { char text[160]; u32 color; f32 time; } UiToast;

struct A3Ui {
    A3UiTheme theme;
    UiDrawList lists[A3_LAYER_COUNT];
    A3UiLayer layer;
    /* GPU */
    A3RhiTexture atlas;
    A3RhiShader shader;
    A3RhiBuffer vb, ib;
    A3RhiMesh mesh;
    b32 gpu_ok;
    f32 white_u, white_v;
    /* frame */
    A3InputState in;
    b32 have_input;
    i32 width, height;
    f32 dt;
    f64 time;
    u32 frame;
    /* interaction */
    A3UiId hot, hot_next, active, focus, focus_next;
    A3UiId last_id;
    A3Rect last_rect;
    b32 last_hovered;
    b32 active_released;   /* active item was released this frame */
    A3UiId released_id;
    A3Vec2 drag_start_mouse;
    f32 drag_start_value[4];
    b32 mouse_consumed;
    b32 wheel_consumed;
    b32 wants_mouse, wants_keyboard;
    b32 any_hovered;
    A3Cursor cursor;
    /* ids */
    A3UiId id_stack[64];
    u32 id_depth;
    /* panels */
    UiPanel panels[32];
    u32 panel_depth;
    A3_ARRAY_TYPE(UiPanelState) panel_states;
    A3_ARRAY_TYPE(UiState) states;
    A3HashMap state_map;   /* id -> index + 1 in states */
    /* text editing */
    A3UiId edit_id;
    char edit_buf[8192];
    i32 edit_len, edit_cursor, edit_anchor;
    f32 edit_scroll;
    f64 edit_blink;
    b32 edit_select_all_pending;
    /* popups / modals */
    UiPopup popups[8];
    u32 popup_count;
    A3UiId popup_stack[8];
    u32 popup_depth;
    b32 popup_close_requested;
    A3Rect popup_rects_prev[8];
    u32 popup_rects_prev_count;
    A3UiId modal_open;
    A3UiId modal_current;
    b32 in_modal;
    A3UiId menubar_open;   /* menubar menu currently open */
    b32 in_menubar;
    A3Vec2 menubar_cursor;
    A3Rect menubar_rect;
    /* drag and drop */
    char drag_type[32];
    u8 drag_payload[512];
    u32 drag_size;
    char drag_label[96];
    b32 dragging;
    A3UiId drag_source;
    b32 drag_released;     /* released this frame: drop targets may accept */
    /* tooltip */
    char tooltip[512];
    b32 tooltip_set;
    A3UiId hover_id;
    f32 hover_time;
    /* toasts */
    UiToast toasts[6];
    u32 toast_count;
    /* clipboard */
    A3UiSetClipboard set_clip;
    A3UiGetClipboard get_clip;
    void *clip_user;
    char tree_ids_open[1];
};

/* draw helpers (a3_ui_draw.c) */
void ui_draw_init(A3Ui *ui);
void ui_draw_shutdown(A3Ui *ui);
void ui_draw_reset(A3Ui *ui);
UiDrawList *ui_list(A3Ui *ui);
void ui_prim_reserve_cmd(A3Ui *ui, u32 tex, u32 mode);

/* state helpers (a3_ui.c) */
UiState *ui_state(A3Ui *ui, A3UiId id, i32 default_i, f32 default_f);

#endif
