/*
 * ASM3D - window_web.c
 * a3_window.h for the browser: a <canvas> with a WebGL2 context.
 *
 * web/asm3d.js owns the canvas. Browser events are queued with the exported
 * a3_web_event() and applied at the start of the next frame (a3_window_poll),
 * so the input state has the same per-frame semantics as on the desktop.
 * OpenGL calls go to WebGL2 through the "gl" imports declared from the same
 * function list the desktop loader uses (a3_gl.h), so rhi_gl.c runs as is.
 */
#include "a3_window.h"
#include "a3_platform.h"
#include "../render/a3_gl.h"
#include "../core/a3_string.h"

#if A3_PLATFORM_WEB

A3_WASM_IMPORT("win", "canvas_width") i32 js_canvas_width(void);
A3_WASM_IMPORT("win", "canvas_height") i32 js_canvas_height(void);
A3_WASM_IMPORT("win", "set_title") void js_set_title(const char *s, i32 len);
A3_WASM_IMPORT("win", "set_cursor") void js_set_cursor(i32 cursor);
A3_WASM_IMPORT("win", "capture_mouse") void js_capture_mouse(i32 on);
A3_WASM_IMPORT("win", "set_clipboard") void js_set_clipboard(const char *s, i32 len);
A3_WASM_IMPORT("win", "get_clipboard") i32 js_get_clipboard(char *dst, i32 cap);
A3_WASM_IMPORT("win", "gl_init") i32 js_gl_init(void);

/* ---- GL imports: one per function of the desktop loader ---- */
#define A3_WEB_GL_IMPORT(ret, name, params) A3_WASM_IMPORT("gl", #name) ret imp_##name params;
A3_GL_FUNCTIONS(A3_WEB_GL_IMPORT)
#undef A3_WEB_GL_IMPORT

typedef struct GlEntry { const char *name; void *fn; } GlEntry;
#define A3_WEB_GL_ENTRY(ret, name, params) { #name, (void *)imp_##name },
static const GlEntry k_gl[] = { A3_GL_FUNCTIONS(A3_WEB_GL_ENTRY) };
#undef A3_WEB_GL_ENTRY

void *a3_window_gl_proc(const char *name) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(k_gl); ++i) if (a3_streq(k_gl[i].name, name)) return k_gl[i].fn;
    return 0;
}

/* ---- events ---- */

enum { EV_KEY_DOWN = 1, EV_KEY_UP, EV_TEXT, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_WHEEL, EV_FOCUS, EV_BLUR, EV_MODS, EV_MOUSE_DELTA, EV_DOUBLE };

typedef struct WebEvent { i32 type; i32 a; f32 x, y; } WebEvent;
#define EVENT_CAP 1024

struct A3Window {
    A3InputState input;
    i32 width, height;
    WebEvent events[EVENT_CAP];
    u32 event_count;
    b32 captured;
    char clip[4096];
};

static A3Window g_window;
static b32 g_window_alive;

A3_WASM_EXPORT("a3_web_event") void a3_web_event(i32 type, i32 a, f32 x, f32 y) {
    if (!g_window_alive || g_window.event_count >= EVENT_CAP) return;
    WebEvent *e = &g_window.events[g_window.event_count++];
    e->type = type; e->a = a; e->x = x; e->y = y;
}

A3Window *a3_window_create(const A3WindowDesc *desc) {
    A3_UNUSED(desc);
    if (g_window_alive) return 0;
    if (!js_gl_init()) return 0;
    a3_zero_struct(&g_window);
    g_window.width = js_canvas_width();
    g_window.height = js_canvas_height();
    g_window.input.focused = 1;
    g_window_alive = 1;
    if (desc && desc->title) a3_window_set_title(&g_window, desc->title);
    return &g_window;
}

void a3_window_destroy(A3Window *w) { if (w) g_window_alive = 0; }

b32 a3_window_poll(A3Window *w) {
    if (!w) return 0;
    A3InputState *in = &w->input;
    a3_input_begin_frame(in);
    w->width = js_canvas_width();
    w->height = js_canvas_height();
    for (u32 i = 0; i < w->event_count; ++i) {
        WebEvent *e = &w->events[i];
        switch (e->type) {
        case EV_KEY_DOWN:
            if (e->a > 0 && e->a < A3_KEY_COUNT) {
                if (!in->keys[e->a]) in->keys_pressed[e->a] = 1;
                in->keys[e->a] = 1;
                in->keys_repeat[e->a] = 1;
            }
            break;
        case EV_KEY_UP:
            if (e->a > 0 && e->a < A3_KEY_COUNT) { in->keys[e->a] = 0; in->keys_released[e->a] = 1; }
            break;
        case EV_TEXT:
            if (e->a >= 32 && e->a != 127 && in->text_count < A3_TEXT_INPUT_MAX) in->text[in->text_count++] = (u32)e->a;
            break;
        case EV_MOUSE_MOVE: {
            A3Vec2 p = a3_v2(e->x, e->y);
            if (!w->captured) in->mouse_delta = a3_v2_add(in->mouse_delta, a3_v2_sub(p, in->mouse_pos));
            in->mouse_pos = p;
        } break;
        case EV_MOUSE_DELTA:
            if (w->captured) in->mouse_delta = a3_v2_add(in->mouse_delta, a3_v2(e->x, e->y));
            break;
        case EV_MOUSE_DOWN:
            if (e->a >= 0 && e->a < A3_MOUSE_BUTTON_COUNT) { in->mouse[e->a] = 1; in->mouse_pressed[e->a] = 1; }
            break;
        case EV_MOUSE_UP:
            if (e->a >= 0 && e->a < A3_MOUSE_BUTTON_COUNT) { in->mouse[e->a] = 0; in->mouse_released[e->a] = 1; }
            break;
        case EV_DOUBLE:
            if (e->a >= 0 && e->a < A3_MOUSE_BUTTON_COUNT) in->mouse_double_click[e->a] = 1;
            break;
        case EV_WHEEL:
            in->scroll = a3_v2_add(in->scroll, a3_v2(e->x, e->y));
            break;
        case EV_MODS:
            in->mods = (u32)e->a;
            break;
        case EV_FOCUS:
            in->focused = 1;
            break;
        case EV_BLUR:
            in->focused = 0;
            a3_memset(in->keys, 0, sizeof(in->keys));
            a3_memset(in->mouse, 0, sizeof(in->mouse));
            break;
        default: break;
        }
    }
    w->event_count = 0;
    in->mouse_captured = w->captured;
    return 1;   /* a browser tab is closed by the browser, not by the engine */
}

void a3_window_swap(A3Window *w) { A3_UNUSED(w); /* the browser presents after each animation frame */ }
void a3_window_size(A3Window *w, i32 *width, i32 *height) { if (width) *width = w ? w->width : 0; if (height) *height = w ? w->height : 0; }
void a3_window_set_title(A3Window *w, const char *title) { A3_UNUSED(w); if (title) js_set_title(title, (i32)a3_strlen(title)); }
void a3_window_set_size(A3Window *w, i32 width, i32 height) { A3_UNUSED(w); A3_UNUSED(width); A3_UNUSED(height); /* the page layout decides */ }
A3InputState *a3_window_input(A3Window *w) { return w ? &w->input : 0; }
void a3_window_set_cursor(A3Window *w, A3Cursor cursor) { A3_UNUSED(w); js_set_cursor((i32)cursor); }
void a3_window_capture_mouse(A3Window *w, b32 capture) {
    if (!w) return;
    w->captured = capture ? 1 : 0;
    w->input.mouse_captured = w->captured;
    js_capture_mouse(w->captured);
}
A3_WASM_EXPORT("a3_web_capture_lost") void a3_web_capture_lost(void) { g_window.captured = 0; g_window.input.mouse_captured = 0; }
void a3_window_set_clipboard(A3Window *w, const char *utf8) { A3_UNUSED(w); if (utf8) js_set_clipboard(utf8, (i32)a3_strlen(utf8)); }
const char *a3_window_get_clipboard(A3Window *w) {
    if (!w) return "";
    i32 n = js_get_clipboard(w->clip, (i32)sizeof(w->clip) - 1);
    w->clip[n > 0 ? n : 0] = 0;
    return w->clip;
}
void a3_window_set_vsync(A3Window *w, b32 on) { A3_UNUSED(w); A3_UNUSED(on); }
b32 a3_window_close_requested(A3Window *w) { A3_UNUSED(w); return 0; }
void a3_window_cancel_close(A3Window *w) { A3_UNUSED(w); }

#endif
