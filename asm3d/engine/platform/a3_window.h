/*
 * ASM3D - a3_window.h
 * Desktop window + OpenGL context + input events.
 * Implementations: window_x11.c (Linux/X11). Win32 and Cocoa are planned
 * behind this same interface.
 */
#ifndef A3_WINDOW_H
#define A3_WINDOW_H

#include "../core/a3_base.h"
#include "../input/a3_input.h"

A3_EXTERN_C_BEGIN

typedef struct A3Window A3Window;

typedef struct A3WindowDesc {
    const char *title;
    i32 width, height;
    b32 resizable;
    b32 vsync;
    b32 hidden;          /* create without showing (offscreen tests) */
    b32 gl_debug;
} A3WindowDesc;

A3Window *a3_window_create(const A3WindowDesc *desc);
void      a3_window_destroy(A3Window *w);
/* Pumps OS events into the input state. Returns false once the user asked
 * to close the window. */
b32       a3_window_poll(A3Window *w);
void      a3_window_swap(A3Window *w);
void      a3_window_size(A3Window *w, i32 *width, i32 *height);
void      a3_window_set_title(A3Window *w, const char *title);
void      a3_window_set_size(A3Window *w, i32 width, i32 height);
A3InputState *a3_window_input(A3Window *w);
void      a3_window_set_cursor(A3Window *w, A3Cursor cursor);
/* Relative mouse mode: hides the cursor and reports raw deltas (FPS look). */
void      a3_window_capture_mouse(A3Window *w, b32 capture);
void      a3_window_set_clipboard(A3Window *w, const char *utf8);
/* Returns clipboard text (valid until the next call) or "". */
const char *a3_window_get_clipboard(A3Window *w);
void     *a3_window_gl_proc(const char *name);
void      a3_window_set_vsync(A3Window *w, b32 on);
b32       a3_window_close_requested(A3Window *w);
void      a3_window_cancel_close(A3Window *w);

A3_EXTERN_C_END

#endif
