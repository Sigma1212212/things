/*
 * ASM3D - window_x11.c
 * X11 + GLX implementation of a3_window.h.
 * libGL is loaded at runtime with dlopen so the engine starts (and can
 * report a clear error) on machines without a working GL driver.
 */
#define _GNU_SOURCE
#include "a3_window.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "a3_platform.h"

#if A3_PLATFORM_LINUX

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <dlfcn.h>

/* ---- minimal GLX declarations (no GL headers needed) ---- */
typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef struct __GLXcontextRec *GLXContext;
typedef XID GLXDrawable;

#define GLX_DOUBLEBUFFER 5
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_STENCIL_SIZE 13
#define GLX_X_VISUAL_TYPE 0x22
#define GLX_TRUE_COLOR 0x8002
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_RENDER_TYPE 0x8011
#define GLX_X_RENDERABLE 0x8012
#define GLX_WINDOW_BIT 0x00000001
#define GLX_RGBA_BIT 0x00000001
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_FLAGS_ARB 0x2094
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define GLX_CONTEXT_DEBUG_BIT_ARB 0x00000001

typedef GLXFBConfig *(*PFN_glXChooseFBConfig)(Display *, int, const int *, int *);
typedef XVisualInfo *(*PFN_glXGetVisualFromFBConfig)(Display *, GLXFBConfig);
typedef Bool (*PFN_glXMakeCurrent)(Display *, GLXDrawable, GLXContext);
typedef void (*PFN_glXSwapBuffers)(Display *, GLXDrawable);
typedef void (*PFN_glXDestroyContext)(Display *, GLXContext);
typedef void *(*PFN_glXGetProcAddressARB)(const unsigned char *);
typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
typedef void (*PFN_glXSwapIntervalEXT)(Display *, GLXDrawable, int);
typedef int (*PFN_glXSwapIntervalMESA)(unsigned int);

static struct {
    void *lib;
    PFN_glXChooseFBConfig ChooseFBConfig;
    PFN_glXGetVisualFromFBConfig GetVisualFromFBConfig;
    PFN_glXMakeCurrent MakeCurrent;
    PFN_glXSwapBuffers SwapBuffers;
    PFN_glXDestroyContext DestroyContext;
    PFN_glXGetProcAddressARB GetProcAddress;
} glx;

struct A3Window {
    Display *dpy;
    Window win;
    Colormap cmap;
    GLXContext ctx;
    Atom wm_delete, clipboard, utf8, targets, a3_sel;
    XIM im;
    XIC ic;
    Cursor cursors[A3_CURSOR_COUNT];
    Cursor blank;
    A3Cursor current_cursor;
    i32 width, height;
    b32 close_requested;
    b32 captured;
    A3InputState input;
    char *clip_own;
    char *clip_recv;
    u64 last_click_ns[A3_MOUSE_BUTTON_COUNT];
    PFN_glXSwapIntervalEXT swap_interval_ext;
    PFN_glXSwapIntervalMESA swap_interval_mesa;
};

static b32 load_glx(void) {
    if (glx.lib) return 1;
    glx.lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!glx.lib) glx.lib = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!glx.lib) { A3_ERROR("window", "could not load libGL: %s", dlerror()); return 0; }
    glx.ChooseFBConfig = (PFN_glXChooseFBConfig)dlsym(glx.lib, "glXChooseFBConfig");
    glx.GetVisualFromFBConfig = (PFN_glXGetVisualFromFBConfig)dlsym(glx.lib, "glXGetVisualFromFBConfig");
    glx.MakeCurrent = (PFN_glXMakeCurrent)dlsym(glx.lib, "glXMakeCurrent");
    glx.SwapBuffers = (PFN_glXSwapBuffers)dlsym(glx.lib, "glXSwapBuffers");
    glx.DestroyContext = (PFN_glXDestroyContext)dlsym(glx.lib, "glXDestroyContext");
    glx.GetProcAddress = (PFN_glXGetProcAddressARB)dlsym(glx.lib, "glXGetProcAddressARB");
    if (!glx.ChooseFBConfig || !glx.GetVisualFromFBConfig || !glx.MakeCurrent || !glx.SwapBuffers || !glx.GetProcAddress) {
        A3_ERROR("window", "libGL is missing required GLX functions");
        return 0;
    }
    return 1;
}

void *a3_window_gl_proc(const char *name) {
    if (!glx.GetProcAddress) return 0;
    void *p = glx.GetProcAddress((const unsigned char *)name);
    if (!p) p = dlsym(glx.lib, name);
    return p;
}

static int x_error_handler(Display *d, XErrorEvent *e) {
    char msg[256];
    XGetErrorText(d, e->error_code, msg, sizeof(msg));
    A3_WARN("window", "X11 error: %s (request %d)", msg, e->request_code);
    return 0;
}

A3Window *a3_window_create(const A3WindowDesc *desc) {
    if (!A3_VERIFY(desc)) return 0;
    XInitThreads();
    if (!load_glx()) return 0;
    Display *dpy = XOpenDisplay(0);
    if (!dpy) {
        a3_log_hint(A3_LOG_ERROR, "window", "ASM3D needs a desktop session. If you are on a server, run it under a virtual display (xvfb-run).",
                    "cannot open X display (DISPLAY is not set or the X server is unreachable)");
        return 0;
    }
    XSetErrorHandler(x_error_handler);
    int screen = DefaultScreen(dpy);
    static const int fb_attribs[] = {
        GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8, GLX_DOUBLEBUFFER, True, None
    };
    int count = 0;
    GLXFBConfig *configs = glx.ChooseFBConfig(dpy, screen, fb_attribs, &count);
    if (!configs || count == 0) {
        a3_log_hint(A3_LOG_ERROR, "window", "Your graphics driver does not offer a suitable OpenGL mode. Update your graphics drivers.",
                    "no matching GLX framebuffer config");
        XCloseDisplay(dpy);
        return 0;
    }
    GLXFBConfig fbc = configs[0];
    XFree(configs);
    XVisualInfo *vi = glx.GetVisualFromFBConfig(dpy, fbc);
    if (!vi) { A3_ERROR("window", "no X visual for GLX config"); XCloseDisplay(dpy); return 0; }

    A3Window *w = A3_NEW(A3Window, A3_MEM_CORE);
    if (!w) { XFree(vi); XCloseDisplay(dpy); return 0; }
    w->dpy = dpy;
    w->width = desc->width > 0 ? desc->width : 1280;
    w->height = desc->height > 0 ? desc->height : 720;
    Window root = RootWindow(dpy, vi->screen);
    w->cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    XSetWindowAttributes swa;
    a3_zero_struct(&swa);
    swa.colormap = w->cmap;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask;
    w->win = XCreateWindow(dpy, root, 0, 0, (unsigned)w->width, (unsigned)w->height, 0, vi->depth, InputOutput, vi->visual,
                           CWBorderPixel | CWColormap | CWEventMask | CWBackPixel, &swa);
    XFree(vi);
    if (!w->win) { A3_ERROR("window", "XCreateWindow failed"); XCloseDisplay(dpy); a3_free(w); return 0; }
    XStoreName(dpy, w->win, desc->title ? desc->title : "ASM3D");
    XClassHint *ch = XAllocClassHint();
    if (ch) { ch->res_name = (char *)"asm3d"; ch->res_class = (char *)"ASM3D"; XSetClassHint(dpy, w->win, ch); XFree(ch); }
    if (!desc->resizable) {
        XSizeHints *sh = XAllocSizeHints();
        if (sh) {
            sh->flags = PMinSize | PMaxSize;
            sh->min_width = sh->max_width = w->width;
            sh->min_height = sh->max_height = w->height;
            XSetWMNormalHints(dpy, w->win, sh);
            XFree(sh);
        }
    }
    w->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, w->win, &w->wm_delete, 1);
    w->clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    w->utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    w->targets = XInternAtom(dpy, "TARGETS", False);
    w->a3_sel = XInternAtom(dpy, "A3_SELECTION", False);
    Bool supported = False;
    XkbSetDetectableAutoRepeat(dpy, True, &supported);

    /* input method for UTF-8 text entry */
    w->im = XOpenIM(dpy, 0, 0, 0);
    if (w->im) w->ic = XCreateIC(w->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, w->win, XNFocusWindow, w->win, (void *)0);

    /* cursors */
    w->cursors[A3_CURSOR_ARROW] = XCreateFontCursor(dpy, 68);
    w->cursors[A3_CURSOR_IBEAM] = XCreateFontCursor(dpy, 152);
    w->cursors[A3_CURSOR_HAND] = XCreateFontCursor(dpy, 60);
    w->cursors[A3_CURSOR_RESIZE_H] = XCreateFontCursor(dpy, 108);
    w->cursors[A3_CURSOR_RESIZE_V] = XCreateFontCursor(dpy, 116);
    w->cursors[A3_CURSOR_MOVE] = XCreateFontCursor(dpy, 52);
    {
        char zero[8] = { 0 };
        Pixmap pm = XCreateBitmapFromData(dpy, w->win, zero, 8, 8);
        XColor black;
        a3_zero_struct(&black);
        w->blank = XCreatePixmapCursor(dpy, pm, pm, &black, &black, 0, 0);
        XFreePixmap(dpy, pm);
    }

    /* OpenGL 3.3 core context */
    PFN_glXCreateContextAttribsARB create_ctx = (PFN_glXCreateContextAttribsARB)a3_window_gl_proc("glXCreateContextAttribsARB");
    if (!create_ctx) {
        a3_log_hint(A3_LOG_ERROR, "window", "Your graphics driver is too old for ASM3D (OpenGL 3.3 required). Update your graphics drivers.",
                    "glXCreateContextAttribsARB unavailable");
        a3_window_destroy(w);
        return 0;
    }
    int ctx_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        GLX_CONTEXT_FLAGS_ARB, desc->gl_debug ? GLX_CONTEXT_DEBUG_BIT_ARB : 0,
        None
    };
    w->ctx = create_ctx(dpy, fbc, 0, True, ctx_attribs);
    XSync(dpy, False);
    if (!w->ctx) {
        a3_log_hint(A3_LOG_ERROR, "window", "Your graphics driver does not support OpenGL 3.3. Update your graphics drivers.",
                    "failed to create an OpenGL 3.3 core context");
        a3_window_destroy(w);
        return 0;
    }
    if (!desc->hidden) XMapWindow(dpy, w->win);
    glx.MakeCurrent(dpy, w->win, w->ctx);
    w->swap_interval_ext = (PFN_glXSwapIntervalEXT)a3_window_gl_proc("glXSwapIntervalEXT");
    w->swap_interval_mesa = (PFN_glXSwapIntervalMESA)a3_window_gl_proc("glXSwapIntervalMESA");
    a3_window_set_vsync(w, desc->vsync);
    XFlush(dpy);
    w->input.focused = 1;
    return w;
}

void a3_window_destroy(A3Window *w) {
    if (!w) return;
    if (w->dpy) {
        if (w->ctx) { glx.MakeCurrent(w->dpy, None, 0); if (glx.DestroyContext) glx.DestroyContext(w->dpy, w->ctx); }
        if (w->ic) XDestroyIC(w->ic);
        if (w->im) XCloseIM(w->im);
        for (int i = 0; i < A3_CURSOR_COUNT; ++i) if (w->cursors[i]) XFreeCursor(w->dpy, w->cursors[i]);
        if (w->blank) XFreeCursor(w->dpy, w->blank);
        if (w->win) XDestroyWindow(w->dpy, w->win);
        if (w->cmap) XFreeColormap(w->dpy, w->cmap);
        XCloseDisplay(w->dpy);
    }
    a3_free(w->clip_own);
    a3_free(w->clip_recv);
    a3_free(w);
}

static int map_keysym(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) return (int)(ks - XK_a) + A3_KEY_A;
    if (ks >= XK_A && ks <= XK_Z) return (int)(ks - XK_A) + A3_KEY_A;
    if (ks >= XK_0 && ks <= XK_9) return (int)(ks - XK_0) + A3_KEY_0;
    if (ks >= XK_F1 && ks <= XK_F12) return (int)(ks - XK_F1) + A3_KEY_F1;
    switch (ks) {
    case XK_space: return A3_KEY_SPACE; case XK_apostrophe: return A3_KEY_APOSTROPHE; case XK_comma: return A3_KEY_COMMA;
    case XK_minus: return A3_KEY_MINUS; case XK_period: return A3_KEY_PERIOD; case XK_slash: return A3_KEY_SLASH;
    case XK_semicolon: return A3_KEY_SEMICOLON; case XK_equal: return A3_KEY_EQUAL; case XK_bracketleft: return A3_KEY_LEFT_BRACKET;
    case XK_backslash: return A3_KEY_BACKSLASH; case XK_bracketright: return A3_KEY_RIGHT_BRACKET; case XK_grave: return A3_KEY_GRAVE;
    case XK_Escape: return A3_KEY_ESCAPE; case XK_Return: case XK_KP_Enter: return A3_KEY_ENTER; case XK_Tab: case XK_ISO_Left_Tab: return A3_KEY_TAB;
    case XK_BackSpace: return A3_KEY_BACKSPACE; case XK_Insert: return A3_KEY_INSERT; case XK_Delete: return A3_KEY_DELETE;
    case XK_Right: return A3_KEY_RIGHT; case XK_Left: return A3_KEY_LEFT; case XK_Down: return A3_KEY_DOWN; case XK_Up: return A3_KEY_UP;
    case XK_Prior: return A3_KEY_PAGE_UP; case XK_Next: return A3_KEY_PAGE_DOWN; case XK_Home: return A3_KEY_HOME; case XK_End: return A3_KEY_END;
    case XK_Caps_Lock: return A3_KEY_CAPS_LOCK;
    case XK_Shift_L: return A3_KEY_LEFT_SHIFT; case XK_Shift_R: return A3_KEY_RIGHT_SHIFT;
    case XK_Control_L: return A3_KEY_LEFT_CONTROL; case XK_Control_R: return A3_KEY_RIGHT_CONTROL;
    case XK_Alt_L: return A3_KEY_LEFT_ALT; case XK_Alt_R: return A3_KEY_RIGHT_ALT;
    case XK_Super_L: return A3_KEY_LEFT_SUPER; case XK_Super_R: return A3_KEY_RIGHT_SUPER;
    default: return A3_KEY_UNKNOWN;
    }
}

static u32 map_mods(unsigned state) {
    u32 m = 0;
    if (state & ShiftMask) m |= A3_MOD_SHIFT;
    if (state & ControlMask) m |= A3_MOD_CTRL;
    if (state & Mod1Mask) m |= A3_MOD_ALT;
    if (state & Mod4Mask) m |= A3_MOD_SUPER;
    return m;
}

static void push_utf8_text(A3InputState *in, const char *s, int n) {
    for (int i = 0; i < n;) {
        u8 c = (u8)s[i];
        u32 cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < n; ++k) cp = (cp << 6) | ((u8)s[i + k] & 0x3F);
        i += len;
        if (cp >= 32 && cp != 127 && in->text_count < A3_TEXT_INPUT_MAX) in->text[in->text_count++] = cp;
    }
}

static void handle_selection_request(A3Window *w, XSelectionRequestEvent *req) {
    XSelectionEvent ev;
    a3_zero_struct(&ev);
    ev.type = SelectionNotify;
    ev.display = req->display;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target = req->target;
    ev.time = req->time;
    ev.property = None;
    if (w->clip_own) {
        if (req->target == w->targets) {
            Atom supported[3] = { w->targets, w->utf8, XA_STRING };
            XChangeProperty(w->dpy, req->requestor, req->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)supported, 3);
            ev.property = req->property;
        } else if (req->target == w->utf8 || req->target == XA_STRING) {
            XChangeProperty(w->dpy, req->requestor, req->property, req->target, 8, PropModeReplace,
                            (unsigned char *)w->clip_own, (int)a3_strlen(w->clip_own));
            ev.property = req->property;
        }
    }
    XSendEvent(w->dpy, req->requestor, False, 0, (XEvent *)&ev);
}

b32 a3_window_poll(A3Window *w) {
    if (!w) return 0;
    A3InputState *in = &w->input;
    a3_input_begin_frame(in);
    while (XPending(w->dpy)) {
        XEvent ev;
        XNextEvent(w->dpy, &ev);
        if (XFilterEvent(&ev, None)) continue;
        switch (ev.type) {
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == w->wm_delete) w->close_requested = 1;
            break;
        case ConfigureNotify:
            w->width = ev.xconfigure.width;
            w->height = ev.xconfigure.height;
            break;
        case FocusIn: in->focused = 1; break;
        case FocusOut:
            in->focused = 0;
            a3_zero(in->keys, sizeof(in->keys)); /* avoid stuck keys after alt-tab */
            a3_zero(in->mouse, sizeof(in->mouse));
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int k = map_keysym(ks);
            in->mods = map_mods(ev.xkey.state);
            if (k > 0 && k < A3_KEY_COUNT) {
                if (!in->keys[k]) in->keys_pressed[k] = 1;
                in->keys[k] = 1;
                in->keys_repeat[k] = 1;
                if (k == A3_KEY_LEFT_SHIFT || k == A3_KEY_RIGHT_SHIFT) in->mods |= A3_MOD_SHIFT;
                if (k == A3_KEY_LEFT_CONTROL || k == A3_KEY_RIGHT_CONTROL) in->mods |= A3_MOD_CTRL;
                if (k == A3_KEY_LEFT_ALT || k == A3_KEY_RIGHT_ALT) in->mods |= A3_MOD_ALT;
            }
            if (!(in->mods & (A3_MOD_CTRL | A3_MOD_SUPER))) {
                char buf[64];
                int n;
                if (w->ic) {
                    Status st;
                    n = Xutf8LookupString(w->ic, &ev.xkey, buf, sizeof(buf) - 1, 0, &st);
                    if (st != XLookupChars && st != XLookupBoth) n = 0;
                } else {
                    n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, 0, 0);
                }
                if (n > 0) push_utf8_text(in, buf, n);
            }
        } break;
        case KeyRelease: {
            int k = map_keysym(XLookupKeysym(&ev.xkey, 0));
            in->mods = map_mods(ev.xkey.state);
            if (k > 0 && k < A3_KEY_COUNT) {
                in->keys[k] = 0;
                in->keys_released[k] = 1;
                if (k == A3_KEY_LEFT_SHIFT || k == A3_KEY_RIGHT_SHIFT) in->mods &= ~(u32)A3_MOD_SHIFT;
                if (k == A3_KEY_LEFT_CONTROL || k == A3_KEY_RIGHT_CONTROL) in->mods &= ~(u32)A3_MOD_CTRL;
                if (k == A3_KEY_LEFT_ALT || k == A3_KEY_RIGHT_ALT) in->mods &= ~(u32)A3_MOD_ALT;
            }
        } break;
        case ButtonPress: {
            unsigned b = ev.xbutton.button;
            in->mods = map_mods(ev.xbutton.state);
            if (b == 4) in->scroll.y += 1; else if (b == 5) in->scroll.y -= 1;
            else if (b == 6) in->scroll.x -= 1; else if (b == 7) in->scroll.x += 1;
            else {
                int mb = b == 1 ? A3_MOUSE_LEFT : b == 3 ? A3_MOUSE_RIGHT : b == 2 ? A3_MOUSE_MIDDLE : -1;
                if (mb >= 0) {
                    in->mouse[mb] = 1;
                    in->mouse_pressed[mb] = 1;
                    u64 now = a3_time_ns();
                    if (now - w->last_click_ns[mb] < 400000000ull) { in->mouse_double_click[mb] = 1; w->last_click_ns[mb] = 0; }
                    else w->last_click_ns[mb] = now;
                }
            }
        } break;
        case ButtonRelease: {
            unsigned b = ev.xbutton.button;
            int mb = b == 1 ? A3_MOUSE_LEFT : b == 3 ? A3_MOUSE_RIGHT : b == 2 ? A3_MOUSE_MIDDLE : -1;
            if (mb >= 0) { in->mouse[mb] = 0; in->mouse_released[mb] = 1; }
        } break;
        case MotionNotify: {
            A3Vec2 p = a3_v2((f32)ev.xmotion.x, (f32)ev.xmotion.y);
            if (w->captured) {
                f32 cx = (f32)(w->width / 2), cy = (f32)(w->height / 2);
                if (p.x != cx || p.y != cy) {
                    in->mouse_delta = a3_v2_add(in->mouse_delta, a3_v2(p.x - cx, p.y - cy));
                    XWarpPointer(w->dpy, None, w->win, 0, 0, 0, 0, w->width / 2, w->height / 2);
                }
            } else {
                in->mouse_delta = a3_v2_add(in->mouse_delta, a3_v2_sub(p, in->mouse_pos));
                in->mouse_pos = p;
            }
        } break;
        case SelectionRequest: handle_selection_request(w, &ev.xselectionrequest); break;
        case SelectionClear: a3_free(w->clip_own); w->clip_own = 0; break;
        default: break;
        }
    }
    return !w->close_requested;
}

void a3_window_swap(A3Window *w) { if (w) glx.SwapBuffers(w->dpy, w->win); }
void a3_window_size(A3Window *w, i32 *width, i32 *height) { if (width) *width = w ? w->width : 0; if (height) *height = w ? w->height : 0; }
void a3_window_set_title(A3Window *w, const char *title) { if (w) { XStoreName(w->dpy, w->win, title); XFlush(w->dpy); } }
void a3_window_set_size(A3Window *w, i32 width, i32 height) { if (w) { XResizeWindow(w->dpy, w->win, (unsigned)width, (unsigned)height); w->width = width; w->height = height; } }
A3InputState *a3_window_input(A3Window *w) { return w ? &w->input : 0; }
b32 a3_window_close_requested(A3Window *w) { return w && w->close_requested; }
void a3_window_cancel_close(A3Window *w) { if (w) w->close_requested = 0; }

void a3_window_set_cursor(A3Window *w, A3Cursor c) {
    if (!w || w->captured || c == w->current_cursor || c >= A3_CURSOR_COUNT) return;
    w->current_cursor = c;
    XDefineCursor(w->dpy, w->win, w->cursors[c]);
}

void a3_window_capture_mouse(A3Window *w, b32 capture) {
    if (!w || capture == w->captured) return;
    w->captured = capture;
    w->input.mouse_captured = capture;
    if (capture) {
        XDefineCursor(w->dpy, w->win, w->blank);
        XGrabPointer(w->dpy, w->win, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, w->win, None, CurrentTime);
        XWarpPointer(w->dpy, None, w->win, 0, 0, 0, 0, w->width / 2, w->height / 2);
    } else {
        XUngrabPointer(w->dpy, CurrentTime);
        XDefineCursor(w->dpy, w->win, w->cursors[w->current_cursor]);
    }
    XFlush(w->dpy);
}

void a3_window_set_clipboard(A3Window *w, const char *text) {
    if (!w) return;
    a3_free(w->clip_own);
    w->clip_own = a3_strdup(text ? text : "", A3_MEM_CORE);
    XSetSelectionOwner(w->dpy, w->clipboard, w->win, CurrentTime);
}

const char *a3_window_get_clipboard(A3Window *w) {
    if (!w) return "";
    if (w->clip_own && XGetSelectionOwner(w->dpy, w->clipboard) == w->win) return w->clip_own;
    if (XGetSelectionOwner(w->dpy, w->clipboard) == None) return "";
    XConvertSelection(w->dpy, w->clipboard, w->utf8, w->a3_sel, w->win, CurrentTime);
    XFlush(w->dpy);
    u64 deadline = a3_time_ns() + 200000000ull;
    while (a3_time_ns() < deadline) {
        XEvent ev;
        if (XCheckTypedWindowEvent(w->dpy, w->win, SelectionNotify, &ev)) {
            if (ev.xselection.property == None) return "";
            Atom type;
            int format;
            unsigned long nitems, after;
            unsigned char *data = 0;
            XGetWindowProperty(w->dpy, w->win, w->a3_sel, 0, 1 << 20, True, AnyPropertyType, &type, &format, &nitems, &after, &data);
            a3_free(w->clip_recv);
            w->clip_recv = 0;
            if (data) {
                w->clip_recv = (char *)a3_malloc(nitems + 1, A3_MEM_CORE);
                if (w->clip_recv) { a3_memcpy(w->clip_recv, data, nitems); w->clip_recv[nitems] = 0; }
                XFree(data);
            }
            return w->clip_recv ? w->clip_recv : "";
        }
        a3_sleep_ms(1);
    }
    return "";
}

void a3_window_set_vsync(A3Window *w, b32 on) {
    if (!w) return;
    if (w->swap_interval_ext) w->swap_interval_ext(w->dpy, w->win, on ? 1 : 0);
    else if (w->swap_interval_mesa) w->swap_interval_mesa(on ? 1 : 0);
}

#endif /* A3_PLATFORM_LINUX */
