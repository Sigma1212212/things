/*
 * ASM3D - window_win32.c
 * Windows window + OpenGL 3.3 core context (WGL) + input.
 *
 * Mouse look uses raw input (WM_INPUT) so deltas are unaffected by pointer
 * acceleration or the window edge. The process is not marked DPI aware, so on
 * high-DPI screens Windows scales the editor to the same visual size as on a
 * 100% screen (the UI is designed in pixels; native UI scaling is planned).
 */
#include "a3_window.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "a3_platform.h"

#if A3_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x0001
typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);

struct A3Window {
    HWND hwnd;
    HDC hdc;
    HGLRC ctx;
    i32 width, height;
    b32 close_requested;
    b32 captured;
    b32 cursor_hidden;
    A3Cursor current_cursor;
    HCURSOR cursors[A3_CURSOR_COUNT];
    A3InputState input;
    char *clip_recv;
    u64 last_click_ns[A3_MOUSE_BUTTON_COUNT];
    u32 buttons_down;
    wchar_t high_surrogate;
    PFN_wglSwapIntervalEXT swap_interval;
};

static HMODULE g_opengl32;

void *a3_window_gl_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if (p == 0 || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        if (!g_opengl32) g_opengl32 = LoadLibraryA("opengl32.dll");
        p = g_opengl32 ? (void *)GetProcAddress(g_opengl32, name) : 0;
    }
    return p;
}

static int map_vk(WPARAM vk, LPARAM lp) {
    b32 extended = (lp >> 24) & 1;
    if (vk >= 'A' && vk <= 'Z') return (int)(vk - 'A') + A3_KEY_A;
    if (vk >= '0' && vk <= '9') return (int)(vk - '0') + A3_KEY_0;
    if (vk >= VK_F1 && vk <= VK_F12) return (int)(vk - VK_F1) + A3_KEY_F1;
    switch (vk) {
    case VK_SPACE: return A3_KEY_SPACE; case VK_OEM_7: return A3_KEY_APOSTROPHE; case VK_OEM_COMMA: return A3_KEY_COMMA;
    case VK_OEM_MINUS: return A3_KEY_MINUS; case VK_OEM_PERIOD: return A3_KEY_PERIOD; case VK_OEM_2: return A3_KEY_SLASH;
    case VK_OEM_1: return A3_KEY_SEMICOLON; case VK_OEM_PLUS: return A3_KEY_EQUAL; case VK_OEM_4: return A3_KEY_LEFT_BRACKET;
    case VK_OEM_5: return A3_KEY_BACKSLASH; case VK_OEM_6: return A3_KEY_RIGHT_BRACKET; case VK_OEM_3: return A3_KEY_GRAVE;
    case VK_ESCAPE: return A3_KEY_ESCAPE; case VK_RETURN: return A3_KEY_ENTER; case VK_TAB: return A3_KEY_TAB;
    case VK_BACK: return A3_KEY_BACKSPACE; case VK_INSERT: return A3_KEY_INSERT; case VK_DELETE: return A3_KEY_DELETE;
    case VK_RIGHT: return A3_KEY_RIGHT; case VK_LEFT: return A3_KEY_LEFT; case VK_DOWN: return A3_KEY_DOWN; case VK_UP: return A3_KEY_UP;
    case VK_PRIOR: return A3_KEY_PAGE_UP; case VK_NEXT: return A3_KEY_PAGE_DOWN; case VK_HOME: return A3_KEY_HOME; case VK_END: return A3_KEY_END;
    case VK_CAPITAL: return A3_KEY_CAPS_LOCK;
    case VK_SHIFT: return ((lp >> 16) & 0xFF) == 0x36 ? A3_KEY_RIGHT_SHIFT : A3_KEY_LEFT_SHIFT;
    case VK_CONTROL: return extended ? A3_KEY_RIGHT_CONTROL : A3_KEY_LEFT_CONTROL;
    case VK_MENU: return extended ? A3_KEY_RIGHT_ALT : A3_KEY_LEFT_ALT;
    case VK_LWIN: return A3_KEY_LEFT_SUPER; case VK_RWIN: return A3_KEY_RIGHT_SUPER;
    default: return A3_KEY_UNKNOWN;
    }
}

static u32 current_mods(void) {
    u32 m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= A3_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= A3_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= A3_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= A3_MOD_SUPER;
    return m;
}

static void mouse_button(A3Window *w, int mb, b32 down) {
    A3InputState *in = &w->input;
    if (down) {
        in->mouse[mb] = 1;
        in->mouse_pressed[mb] = 1;
        u64 now = a3_time_ns();
        if (now - w->last_click_ns[mb] < (u64)GetDoubleClickTime() * 1000000ull) { in->mouse_double_click[mb] = 1; w->last_click_ns[mb] = 0; }
        else w->last_click_ns[mb] = now;
        if (!w->buttons_down) SetCapture(w->hwnd); /* keep receiving the drag outside the window */
        w->buttons_down |= 1u << mb;
    } else {
        in->mouse[mb] = 0;
        in->mouse_released[mb] = 1;
        w->buttons_down &= ~(1u << mb);
        if (!w->buttons_down) ReleaseCapture();
    }
    in->mods = current_mods();
}

static void clip_to_window(A3Window *w) {
    RECT r;
    GetClientRect(w->hwnd, &r);
    POINT a = { r.left, r.top }, b = { r.right, r.bottom };
    ClientToScreen(w->hwnd, &a);
    ClientToScreen(w->hwnd, &b);
    RECT s = { a.x, a.y, b.x, b.y };
    ClipCursor(&s);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    A3Window *w = (A3Window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    A3InputState *in = &w->input;
    switch (msg) {
    case WM_CLOSE: w->close_requested = 1; return 0;
    case WM_SIZE:
        w->width = LOWORD(lp);
        w->height = HIWORD(lp);
        if (w->captured) clip_to_window(w);
        return 0;
    case WM_SETFOCUS: in->focused = 1; if (w->captured) clip_to_window(w); return 0;
    case WM_KILLFOCUS:
        in->focused = 0;
        a3_zero(in->keys, sizeof(in->keys)); /* avoid stuck keys after alt-tab */
        a3_zero(in->mouse, sizeof(in->mouse));
        w->buttons_down = 0;
        ClipCursor(0);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0; /* Alt must not open the (non-existent) menu */
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int k = map_vk(wp, lp);
        in->mods = current_mods();
        if (k > 0 && k < A3_KEY_COUNT) {
            if (!in->keys[k]) in->keys_pressed[k] = 1;
            in->keys[k] = 1;
            in->keys_repeat[k] = 1;
        }
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) { w->close_requested = 1; return 0; }
        return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        int k = map_vk(wp, lp);
        in->mods = current_mods();
        if (k > 0 && k < A3_KEY_COUNT) { in->keys[k] = 0; in->keys_released[k] = 1; }
        return 0;
    }
    case WM_CHAR: {
        u32 cp = (u32)wp;
        if (cp >= 0xD800 && cp <= 0xDBFF) { w->high_surrogate = (wchar_t)cp; return 0; }
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
            if (!w->high_surrogate) return 0;
            cp = 0x10000 + (((u32)w->high_surrogate - 0xD800) << 10) + (cp - 0xDC00);
            w->high_surrogate = 0;
        }
        if (in->mods & (A3_MOD_CTRL | A3_MOD_SUPER)) return 0; /* shortcuts are not text */
        if (cp >= 32 && cp != 127 && in->text_count < A3_TEXT_INPUT_MAX) in->text[in->text_count++] = cp;
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: mouse_button(w, A3_MOUSE_LEFT, 1); return 0;
    case WM_LBUTTONUP: mouse_button(w, A3_MOUSE_LEFT, 0); return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: mouse_button(w, A3_MOUSE_RIGHT, 1); return 0;
    case WM_RBUTTONUP: mouse_button(w, A3_MOUSE_RIGHT, 0); return 0;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: mouse_button(w, A3_MOUSE_MIDDLE, 1); return 0;
    case WM_MBUTTONUP: mouse_button(w, A3_MOUSE_MIDDLE, 0); return 0;
    case WM_MOUSEWHEEL: in->scroll.y += (f32)GET_WHEEL_DELTA_WPARAM(wp) / (f32)WHEEL_DELTA; return 0;
    case WM_MOUSEHWHEEL: in->scroll.x += (f32)GET_WHEEL_DELTA_WPARAM(wp) / (f32)WHEEL_DELTA; return 0;
    case WM_MOUSEMOVE:
        if (!w->captured) {
            A3Vec2 p = a3_v2((f32)(i16)LOWORD(lp), (f32)(i16)HIWORD(lp));
            in->mouse_delta = a3_v2_add(in->mouse_delta, a3_v2_sub(p, in->mouse_pos));
            in->mouse_pos = p;
        }
        return 0;
    case WM_INPUT: {
        if (!w->captured) break;
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            in->mouse_delta = a3_v2_add(in->mouse_delta, a3_v2((f32)ri.data.mouse.lLastX, (f32)ri.data.mouse.lLastY));
        break;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(w->captured ? 0 : w->cursors[w->current_cursor]); return TRUE; }
        break;
    case WM_ERASEBKGND: return 1;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

A3Window *a3_window_create(const A3WindowDesc *desc) {
    if (!A3_VERIFY(desc)) return 0;
    HINSTANCE inst = GetModuleHandleW(0);
    static b32 registered;
    if (!registered) {
        WNDCLASSEXW wc;
        a3_zero_struct(&wc);
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
        wc.hIcon = LoadIconW(inst, (LPCWSTR)MAKEINTRESOURCEW(1));
        if (!wc.hIcon) wc.hIcon = LoadIconW(0, (LPCWSTR)IDI_APPLICATION);
        wc.lpszClassName = L"ASM3DWindow";
        if (!RegisterClassExW(&wc)) { A3_ERROR("window", "RegisterClassEx failed (%lu)", GetLastError()); return 0; }
        registered = 1;
    }
    A3Window *w = A3_NEW(A3Window, A3_MEM_CORE);
    if (!w) return 0;
    w->width = desc->width > 0 ? desc->width : 1280;
    w->height = desc->height > 0 ? desc->height : 720;
    DWORD style = desc->resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    RECT r = { 0, 0, w->width, w->height };
    AdjustWindowRect(&r, style, FALSE);
    wchar_t title[256];
    MultiByteToWideChar(CP_UTF8, 0, desc->title ? desc->title : "ASM3D", -1, title, 256);
    w->hwnd = CreateWindowExW(0, L"ASM3DWindow", title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    if (!w->hwnd) { A3_ERROR("window", "CreateWindowEx failed (%lu)", GetLastError()); a3_free(w); return 0; }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    w->hdc = GetDC(w->hwnd);
    PIXELFORMATDESCRIPTOR pfd;
    a3_zero_struct(&pfd);
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(w->hdc, &pfd);
    if (!pf || !SetPixelFormat(w->hdc, pf, &pfd)) {
        a3_log_hint(A3_LOG_ERROR, "window", "Your graphics driver does not offer a suitable OpenGL mode. Update your graphics drivers.",
                    "no suitable pixel format (%lu)", GetLastError());
        a3_window_destroy(w);
        return 0;
    }
    /* a legacy context is needed to load wglCreateContextAttribsARB */
    HGLRC legacy = wglCreateContext(w->hdc);
    if (!legacy || !wglMakeCurrent(w->hdc, legacy)) {
        a3_log_hint(A3_LOG_ERROR, "window", "OpenGL is not available. Install the graphics driver from your GPU vendor (NVIDIA, AMD or Intel).",
                    "wglCreateContext failed (%lu)", GetLastError());
        if (legacy) wglDeleteContext(legacy);
        a3_window_destroy(w);
        return 0;
    }
    PFN_wglCreateContextAttribsARB create_ctx = (PFN_wglCreateContextAttribsARB)a3_window_gl_proc("wglCreateContextAttribsARB");
    if (create_ctx) {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            WGL_CONTEXT_FLAGS_ARB, desc->gl_debug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0,
            0
        };
        w->ctx = create_ctx(w->hdc, 0, attribs);
    }
    wglMakeCurrent(0, 0);
    wglDeleteContext(legacy);
    if (!w->ctx || !wglMakeCurrent(w->hdc, w->ctx)) {
        a3_log_hint(A3_LOG_ERROR, "window", "ASM3D needs OpenGL 3.3. Update your graphics drivers from NVIDIA, AMD or Intel (the Windows default driver is not enough).",
                    "failed to create an OpenGL 3.3 core context");
        a3_window_destroy(w);
        return 0;
    }
    w->swap_interval = (PFN_wglSwapIntervalEXT)a3_window_gl_proc("wglSwapIntervalEXT");
    a3_window_set_vsync(w, desc->vsync);
    /* raw mouse input for captured (FPS) look */
    RAWINPUTDEVICE rid = { 0x01, 0x02, 0, w->hwnd };
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    w->cursors[A3_CURSOR_ARROW] = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
    w->cursors[A3_CURSOR_IBEAM] = LoadCursorW(0, (LPCWSTR)IDC_IBEAM);
    w->cursors[A3_CURSOR_HAND] = LoadCursorW(0, (LPCWSTR)IDC_HAND);
    w->cursors[A3_CURSOR_RESIZE_H] = LoadCursorW(0, (LPCWSTR)IDC_SIZEWE);
    w->cursors[A3_CURSOR_RESIZE_V] = LoadCursorW(0, (LPCWSTR)IDC_SIZENS);
    w->cursors[A3_CURSOR_MOVE] = LoadCursorW(0, (LPCWSTR)IDC_SIZEALL);
    if (!desc->hidden) { ShowWindow(w->hwnd, SW_SHOW); UpdateWindow(w->hwnd); }
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    if (cr.right > 0) { w->width = cr.right; w->height = cr.bottom; }
    w->input.focused = 1;
    return w;
}

void a3_window_destroy(A3Window *w) {
    if (!w) return;
    if (w->captured) a3_window_capture_mouse(w, 0);
    if (w->ctx) { wglMakeCurrent(0, 0); wglDeleteContext(w->ctx); }
    if (w->hdc) ReleaseDC(w->hwnd, w->hdc);
    if (w->hwnd) { SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0); DestroyWindow(w->hwnd); }
    a3_free(w->clip_recv);
    a3_free(w);
}

b32 a3_window_poll(A3Window *w) {
    if (!w) return 0;
    a3_input_begin_frame(&w->input);
    MSG msg;
    while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) w->close_requested = 1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (w->captured && w->input.focused) {
        /* keep the (hidden) pointer centered so clicks never leave the window */
        POINT c = { w->width / 2, w->height / 2 };
        ClientToScreen(w->hwnd, &c);
        SetCursorPos(c.x, c.y);
    }
    return !w->close_requested;
}

void a3_window_swap(A3Window *w) { if (w) SwapBuffers(w->hdc); }
void a3_window_size(A3Window *w, i32 *width, i32 *height) { if (width) *width = w ? w->width : 0; if (height) *height = w ? w->height : 0; }

void a3_window_set_title(A3Window *w, const char *title) {
    if (!w) return;
    wchar_t t[512];
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, t, 512);
    SetWindowTextW(w->hwnd, t);
}

void a3_window_set_size(A3Window *w, i32 width, i32 height) {
    if (!w) return;
    RECT r = { 0, 0, width, height };
    AdjustWindowRect(&r, (DWORD)GetWindowLongW(w->hwnd, GWL_STYLE), FALSE);
    SetWindowPos(w->hwnd, 0, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

A3InputState *a3_window_input(A3Window *w) { return w ? &w->input : 0; }
b32 a3_window_close_requested(A3Window *w) { return w && w->close_requested; }
void a3_window_cancel_close(A3Window *w) { if (w) w->close_requested = 0; }

void a3_window_set_cursor(A3Window *w, A3Cursor c) {
    if (!w || c >= A3_CURSOR_COUNT || c == w->current_cursor) return;
    w->current_cursor = c;
    if (!w->captured) {
        POINT p;
        GetCursorPos(&p);
        if (WindowFromPoint(p) == w->hwnd) SetCursor(w->cursors[c]);
    }
}

void a3_window_capture_mouse(A3Window *w, b32 capture) {
    if (!w || capture == w->captured) return;
    w->captured = capture;
    w->input.mouse_captured = capture;
    if (capture) {
        if (!w->cursor_hidden) { ShowCursor(FALSE); w->cursor_hidden = 1; }
        clip_to_window(w);
        SetCursor(0);
    } else {
        ClipCursor(0);
        if (w->cursor_hidden) { ShowCursor(TRUE); w->cursor_hidden = 0; }
        SetCursor(w->cursors[w->current_cursor]);
    }
}

void a3_window_set_clipboard(A3Window *w, const char *utf8) {
    if (!w || !OpenClipboard(w->hwnd)) return;
    EmptyClipboard();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, 0, 0);
    HGLOBAL mem = n > 0 ? GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t)) : 0;
    if (mem) {
        wchar_t *dst = (wchar_t *)GlobalLock(mem);
        MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, dst, n);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

const char *a3_window_get_clipboard(A3Window *w) {
    if (!w || !OpenClipboard(w->hwnd)) return "";
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *src = h ? (const wchar_t *)GlobalLock(h) : 0;
    a3_free(w->clip_recv);
    w->clip_recv = 0;
    if (src) {
        int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, 0, 0, 0, 0);
        if (n > 0 && (w->clip_recv = (char *)a3_malloc((usize)n, A3_MEM_CORE)) != 0) WideCharToMultiByte(CP_UTF8, 0, src, -1, w->clip_recv, n, 0, 0);
        GlobalUnlock(h);
    }
    CloseClipboard();
    return w->clip_recv ? w->clip_recv : "";
}

void a3_window_set_vsync(A3Window *w, b32 on) {
    if (w && w->swap_interval) w->swap_interval(on ? 1 : 0);
}

#endif /* A3_PLATFORM_WINDOWS */
