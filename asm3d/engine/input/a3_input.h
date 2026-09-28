/*
 * ASM3D - a3_input.h
 * Platform-independent input state and action mapping.
 * The window layer fills A3InputState; gameplay reads named actions
 * ("jump", "move_x") so controls can be rebound without code changes.
 */
#ifndef A3_INPUT_H
#define A3_INPUT_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

typedef enum A3Key {
    A3_KEY_UNKNOWN = 0,
    A3_KEY_SPACE = 32, A3_KEY_APOSTROPHE = 39, A3_KEY_COMMA = 44, A3_KEY_MINUS, A3_KEY_PERIOD, A3_KEY_SLASH,
    A3_KEY_0 = 48, A3_KEY_1, A3_KEY_2, A3_KEY_3, A3_KEY_4, A3_KEY_5, A3_KEY_6, A3_KEY_7, A3_KEY_8, A3_KEY_9,
    A3_KEY_SEMICOLON = 59, A3_KEY_EQUAL = 61,
    A3_KEY_A = 65, A3_KEY_B, A3_KEY_C, A3_KEY_D, A3_KEY_E, A3_KEY_F, A3_KEY_G, A3_KEY_H, A3_KEY_I, A3_KEY_J,
    A3_KEY_K, A3_KEY_L, A3_KEY_M, A3_KEY_N, A3_KEY_O, A3_KEY_P, A3_KEY_Q, A3_KEY_R, A3_KEY_S, A3_KEY_T,
    A3_KEY_U, A3_KEY_V, A3_KEY_W, A3_KEY_X, A3_KEY_Y, A3_KEY_Z,
    A3_KEY_LEFT_BRACKET = 91, A3_KEY_BACKSLASH, A3_KEY_RIGHT_BRACKET, A3_KEY_GRAVE = 96,
    A3_KEY_ESCAPE = 256, A3_KEY_ENTER, A3_KEY_TAB, A3_KEY_BACKSPACE, A3_KEY_INSERT, A3_KEY_DELETE,
    A3_KEY_RIGHT, A3_KEY_LEFT, A3_KEY_DOWN, A3_KEY_UP, A3_KEY_PAGE_UP, A3_KEY_PAGE_DOWN, A3_KEY_HOME, A3_KEY_END,
    A3_KEY_CAPS_LOCK = 280,
    A3_KEY_F1 = 290, A3_KEY_F2, A3_KEY_F3, A3_KEY_F4, A3_KEY_F5, A3_KEY_F6, A3_KEY_F7, A3_KEY_F8, A3_KEY_F9,
    A3_KEY_F10, A3_KEY_F11, A3_KEY_F12,
    A3_KEY_LEFT_SHIFT = 340, A3_KEY_LEFT_CONTROL, A3_KEY_LEFT_ALT, A3_KEY_LEFT_SUPER,
    A3_KEY_RIGHT_SHIFT, A3_KEY_RIGHT_CONTROL, A3_KEY_RIGHT_ALT, A3_KEY_RIGHT_SUPER,
    A3_KEY_COUNT = 360
} A3Key;

typedef enum A3MouseButton { A3_MOUSE_LEFT = 0, A3_MOUSE_RIGHT, A3_MOUSE_MIDDLE, A3_MOUSE_BUTTON_COUNT } A3MouseButton;

typedef enum A3Mods { A3_MOD_SHIFT = 1, A3_MOD_CTRL = 2, A3_MOD_ALT = 4, A3_MOD_SUPER = 8 } A3Mods;

typedef enum A3Cursor { A3_CURSOR_ARROW = 0, A3_CURSOR_IBEAM, A3_CURSOR_HAND, A3_CURSOR_RESIZE_H, A3_CURSOR_RESIZE_V, A3_CURSOR_MOVE, A3_CURSOR_COUNT } A3Cursor;

#define A3_TEXT_INPUT_MAX 64

typedef struct A3InputState {
    u8 keys[A3_KEY_COUNT];            /* 1 while held */
    u8 keys_pressed[A3_KEY_COUNT];    /* went down this frame (includes key repeat) */
    u8 keys_released[A3_KEY_COUNT];
    u8 keys_repeat[A3_KEY_COUNT];     /* pressed or auto-repeated this frame (text editing) */
    u8 mouse[A3_MOUSE_BUTTON_COUNT];
    u8 mouse_pressed[A3_MOUSE_BUTTON_COUNT];
    u8 mouse_released[A3_MOUSE_BUTTON_COUNT];
    u8 mouse_double_click[A3_MOUSE_BUTTON_COUNT];
    A3Vec2 mouse_pos;                 /* pixels, top-left origin */
    A3Vec2 mouse_delta;               /* movement since last frame (works while captured) */
    A3Vec2 scroll;                    /* wheel ticks this frame */
    u32 mods;                         /* A3Mods */
    u32 text[A3_TEXT_INPUT_MAX];      /* UTF-32 codepoints typed this frame */
    u32 text_count;
    b32 focused;
    b32 mouse_captured;               /* relative mouse mode (FPS look) */
} A3InputState;

/* Called by the window layer at the start of each frame. */
void a3_input_begin_frame(A3InputState *in);

/* ---- Action mapping ---- */
#define A3_ACTION_MAX 64
#define A3_ACTION_BINDINGS 4

typedef enum A3BindingKind { A3_BIND_NONE = 0, A3_BIND_KEY, A3_BIND_MOUSE } A3BindingKind;
typedef struct A3Binding { u8 kind; i16 code; i8 scale; /* +1/-1 for axes */ } A3Binding;
typedef struct A3Action {
    char name[32];
    A3Binding bindings[A3_ACTION_BINDINGS];
    u32 binding_count;
    char description[96];
} A3Action;

typedef struct A3InputMap {
    A3Action actions[A3_ACTION_MAX];
    u32 count;
} A3InputMap;

void a3_input_map_defaults(A3InputMap *map);  /* move_x, move_y, jump, sprint, crouch, interact, fire, aim, pause */
A3Action *a3_input_map_add(A3InputMap *map, const char *name, const char *description);
void a3_input_action_bind(A3Action *a, A3BindingKind kind, int code, int scale);
A3Action *a3_input_map_find(A3InputMap *map, const char *name);
/* Queries (return 0 when the action does not exist). */
f32 a3_action_value(const A3InputMap *map, const A3InputState *in, const char *name);   /* axis in [-1,1] or 0/1 */
b32 a3_action_down(const A3InputMap *map, const A3InputState *in, const char *name);
b32 a3_action_pressed(const A3InputMap *map, const A3InputState *in, const char *name);
b32 a3_action_released(const A3InputMap *map, const A3InputState *in, const char *name);

const char *a3_key_name(int key);
int a3_key_from_name(const char *name);

A3_EXTERN_C_END

#endif
