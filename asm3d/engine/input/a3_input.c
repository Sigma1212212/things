/*
 * ASM3D - a3_input.c
 */
#include "a3_input.h"
#include "../core/a3_string.h"
#include "../core/a3_log.h"
#include "../core/a3_format.h"

void a3_input_begin_frame(A3InputState *in) {
    a3_zero(in->keys_pressed, sizeof(in->keys_pressed));
    a3_zero(in->keys_released, sizeof(in->keys_released));
    a3_zero(in->keys_repeat, sizeof(in->keys_repeat));
    a3_zero(in->mouse_pressed, sizeof(in->mouse_pressed));
    a3_zero(in->mouse_released, sizeof(in->mouse_released));
    a3_zero(in->mouse_double_click, sizeof(in->mouse_double_click));
    in->mouse_delta = a3_v2(0, 0);
    in->scroll = a3_v2(0, 0);
    in->text_count = 0;
}

A3Action *a3_input_map_find(A3InputMap *map, const char *name) {
    for (u32 i = 0; i < map->count; ++i) if (a3_streq(map->actions[i].name, name)) return &map->actions[i];
    return 0;
}

A3Action *a3_input_map_add(A3InputMap *map, const char *name, const char *desc) {
    A3Action *a = a3_input_map_find(map, name);
    if (a) return a;
    if (map->count >= A3_ACTION_MAX) { A3_ERROR("input", "too many input actions"); return 0; }
    a = &map->actions[map->count++];
    a3_zero_struct(a);
    a3_strcpy(a->name, sizeof(a->name), name);
    a3_strcpy(a->description, sizeof(a->description), desc ? desc : "");
    return a;
}

void a3_input_action_bind(A3Action *a, A3BindingKind kind, int code, int scale) {
    if (!a || a->binding_count >= A3_ACTION_BINDINGS) return;
    A3Binding b = { (u8)kind, (i16)code, (i8)(scale ? scale : 1) };
    a->bindings[a->binding_count++] = b;
}

void a3_input_map_defaults(A3InputMap *m) {
    a3_zero_struct(m);
    A3Action *a;
    a = a3_input_map_add(m, "move_x", "Strafe left/right");
    a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_D, 1); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_A, -1);
    a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_RIGHT, 1); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_LEFT, -1);
    a = a3_input_map_add(m, "move_y", "Move forward/back");
    a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_W, 1); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_S, -1);
    a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_UP, 1); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_DOWN, -1);
    a = a3_input_map_add(m, "jump", "Jump"); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_SPACE, 1);
    a = a3_input_map_add(m, "sprint", "Run faster"); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_LEFT_SHIFT, 1);
    a = a3_input_map_add(m, "crouch", "Crouch"); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_LEFT_CONTROL, 1); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_C, 1);
    a = a3_input_map_add(m, "interact", "Use / open"); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_E, 1);
    a = a3_input_map_add(m, "fire", "Shoot / attack"); a3_input_action_bind(a, A3_BIND_MOUSE, A3_MOUSE_LEFT, 1);
    a = a3_input_map_add(m, "aim", "Aim"); a3_input_action_bind(a, A3_BIND_MOUSE, A3_MOUSE_RIGHT, 1);
    a = a3_input_map_add(m, "reload", "Reload"); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_R, 1);
    a = a3_input_map_add(m, "pause", "Pause menu"); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_ESCAPE, 1);
    a = a3_input_map_add(m, "brake", "Handbrake (vehicles)"); a3_input_action_bind(a, A3_BIND_KEY, A3_KEY_SPACE, 1);
}

static f32 binding_value(const A3Binding *b, const A3InputState *in) {
    if (b->kind == A3_BIND_KEY && b->code >= 0 && b->code < A3_KEY_COUNT) return in->keys[b->code] ? (f32)b->scale : 0.0f;
    if (b->kind == A3_BIND_MOUSE && b->code >= 0 && b->code < A3_MOUSE_BUTTON_COUNT) return in->mouse[b->code] ? (f32)b->scale : 0.0f;
    return 0.0f;
}

static b32 binding_edge(const A3Binding *b, const A3InputState *in, b32 pressed) {
    if (b->kind == A3_BIND_KEY && b->code >= 0 && b->code < A3_KEY_COUNT) return pressed ? in->keys_pressed[b->code] : in->keys_released[b->code];
    if (b->kind == A3_BIND_MOUSE && b->code >= 0 && b->code < A3_MOUSE_BUTTON_COUNT) return pressed ? in->mouse_pressed[b->code] : in->mouse_released[b->code];
    return 0;
}

f32 a3_action_value(const A3InputMap *map, const A3InputState *in, const char *name) {
    const A3Action *a = a3_input_map_find((A3InputMap *)map, name);
    if (!a || !in) return 0.0f;
    f32 v = 0.0f;
    for (u32 i = 0; i < a->binding_count; ++i) v += binding_value(&a->bindings[i], in);
    return a3_clampf(v, -1.0f, 1.0f);
}

b32 a3_action_down(const A3InputMap *map, const A3InputState *in, const char *name) { return a3_action_value(map, in, name) != 0.0f; }

b32 a3_action_pressed(const A3InputMap *map, const A3InputState *in, const char *name) {
    const A3Action *a = a3_input_map_find((A3InputMap *)map, name);
    if (!a || !in) return 0;
    for (u32 i = 0; i < a->binding_count; ++i) if (binding_edge(&a->bindings[i], in, 1)) return 1;
    return 0;
}

b32 a3_action_released(const A3InputMap *map, const A3InputState *in, const char *name) {
    const A3Action *a = a3_input_map_find((A3InputMap *)map, name);
    if (!a || !in) return 0;
    for (u32 i = 0; i < a->binding_count; ++i) if (binding_edge(&a->bindings[i], in, 0)) return 1;
    return 0;
}

const char *a3_key_name(int key) {
    static char buf[8];
    if ((key >= A3_KEY_A && key <= A3_KEY_Z) || (key >= A3_KEY_0 && key <= A3_KEY_9)) { buf[0] = (char)key; buf[1] = 0; return buf; }
    if (key >= A3_KEY_F1 && key <= A3_KEY_F12) { a3_snprintf(buf, sizeof(buf), "F%d", key - A3_KEY_F1 + 1); return buf; }
    switch (key) {
    case A3_KEY_SPACE: return "Space"; case A3_KEY_ESCAPE: return "Escape"; case A3_KEY_ENTER: return "Enter";
    case A3_KEY_TAB: return "Tab"; case A3_KEY_BACKSPACE: return "Backspace"; case A3_KEY_DELETE: return "Delete";
    case A3_KEY_LEFT: return "Left"; case A3_KEY_RIGHT: return "Right"; case A3_KEY_UP: return "Up"; case A3_KEY_DOWN: return "Down";
    case A3_KEY_LEFT_SHIFT: return "LeftShift"; case A3_KEY_RIGHT_SHIFT: return "RightShift";
    case A3_KEY_LEFT_CONTROL: return "LeftCtrl"; case A3_KEY_RIGHT_CONTROL: return "RightCtrl";
    case A3_KEY_LEFT_ALT: return "LeftAlt"; case A3_KEY_RIGHT_ALT: return "RightAlt";
    case A3_KEY_HOME: return "Home"; case A3_KEY_END: return "End"; case A3_KEY_PAGE_UP: return "PageUp"; case A3_KEY_PAGE_DOWN: return "PageDown";
    default: return "?";
    }
}

int a3_key_from_name(const char *name) {
    if (!name || !*name) return 0;
    if (!name[1]) {
        int c = a3_to_upper(name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    for (int k = 1; k < A3_KEY_COUNT; ++k) {
        const char *n = a3_key_name(k);
        if (n[0] != '?' && a3_streq(n, name)) return k;
    }
    return 0;
}
