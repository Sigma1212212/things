/*
 * ASM3D - platform_win32.c
 * Windows implementation of a3_platform.h.
 *
 * Engine paths are UTF-8 with '/' separators; they are converted to UTF-16
 * for the wide ("W") Win32 APIs, and paths returned by Windows are converted
 * back with '\' turned into '/'.
 */
#include "a3_platform.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_atomic.h"

#if A3_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <stdlib.h>

#define WPATH 1024

static LARGE_INTEGER g_freq;
static u64 g_start_ns;

/* ---- string conversion ---- */

static b32 to_wide(const char *utf8, wchar_t *out, int cap) {
    if (!utf8) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap);
    if (n <= 0) { out[0] = 0; return 0; }
    for (wchar_t *p = out; *p; ++p) if (*p == L'/') *p = L'\\';
    return 1;
}

static void to_utf8(const wchar_t *w, char *out, usize cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, 0, 0);
    if (n <= 0) { if (cap) out[0] = 0; return; }
    a3_path_normalize(out);
}

static u64 filetime_ns(FILETIME ft) {
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    return t * 100ull;
}

/* ---- lifetime & time ---- */

static u64 now_ns(void) {
    LARGE_INTEGER c;
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&c);
    /* split to avoid overflow: seconds and remainder */
    u64 sec = (u64)(c.QuadPart / g_freq.QuadPart), rem = (u64)(c.QuadPart % g_freq.QuadPart);
    return sec * 1000000000ull + rem * 1000000000ull / (u64)g_freq.QuadPart;
}

void a3_platform_init(void) {
    if (!g_start_ns) g_start_ns = now_ns();
    /* GUI programs have no console; show logs when started from one */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) AttachConsole(ATTACH_PARENT_PROCESS);
    timeBeginPeriod(1); /* 1 ms sleep granularity for frame pacing */
    SetConsoleOutputCP(CP_UTF8);
}

void a3_platform_shutdown(void) { timeEndPeriod(1); }
const char *a3_platform_name(void) { return "Windows"; }

u64 a3_time_ns(void) { return now_ns(); }
f64 a3_time_seconds(void) {
    if (!g_start_ns) g_start_ns = now_ns();
    return (f64)(now_ns() - g_start_ns) * 1e-9;
}
u64 a3_wall_clock_unix(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000000ull;
}
void a3_sleep_ms(u32 ms) { Sleep(ms); }

/* ---- console ---- */

void a3_console_write(A3LogLevel level, const char *text, usize len) {
    HANDLE h = GetStdHandle(level >= A3_LOG_WARN ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD mode;
        b32 console = GetConsoleMode(h, &mode);
        WORD attr = 0;
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (console && GetConsoleScreenBufferInfo(h, &info)) {
            attr = info.wAttributes;
            WORD c = level >= A3_LOG_ERROR ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                   : level == A3_LOG_WARN ? (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
                   : level <= A3_LOG_DEBUG ? FOREGROUND_INTENSITY : attr;
            SetConsoleTextAttribute(h, c);
        }
        while (len) {
            DWORD w = 0;
            if (!WriteFile(h, text, (DWORD)(len > 0x10000 ? 0x10000 : len), &w, 0) || !w) break;
            text += w;
            len -= w;
        }
        if (console && attr) SetConsoleTextAttribute(h, attr);
    }
    /* also visible in debuggers when there is no console (GUI builds) */
    char buf[1024];
    usize n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    A3_UNUSED(n);
    A3_UNUSED(buf);
}

void a3_platform_abort(const char *reason) {
    char msg[512];
    a3_snprintf(msg, sizeof(msg), "ASM3D stopped: %s", reason ? reason : "unknown error");
    a3_console_write(A3_LOG_FATAL, msg, a3_strlen(msg));
    MessageBoxA(0, msg, "ASM3D", MB_OK | MB_ICONERROR);
    abort();
}

/* ---- memory & CPU ---- */

void *a3_os_alloc(usize size) { return VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void  a3_os_free(void *ptr, usize size) { A3_UNUSED(size); if (ptr) VirtualFree(ptr, 0, MEM_RELEASE); }
usize a3_os_page_size(void) {
    static usize ps;
    if (!ps) { SYSTEM_INFO si; GetSystemInfo(&si); ps = si.dwPageSize; }
    return ps;
}
u32 a3_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
}

/* ---- files ---- */

b32 a3_file_info(const char *path, A3FileInfo *out) {
    a3_zero_struct(out);
    wchar_t w[WPATH];
    if (!path || !to_wide(path, w, WPATH)) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fa)) return 0;
    out->exists = 1;
    out->is_dir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->size = ((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    out->mtime_ns = filetime_ns(fa.ftLastWriteTime);
    return 1;
}

b32 a3_file_exists(const char *path) { A3FileInfo fi; return a3_file_info(path, &fi) && !fi.is_dir; }
b32 a3_dir_exists(const char *path) { A3FileInfo fi; return a3_file_info(path, &fi) && fi.is_dir; }

A3Result a3_file_read_all(const char *path, A3MemTag tag, A3FileData *out) {
    a3_zero_struct(out);
    wchar_t w[WPATH];
    if (!path || !to_wide(path, w, WPATH)) return A3_ERR_INVALID_ARG;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? A3_ERR_NOT_FOUND : A3_ERR_IO;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0x7FFFFFFF) { CloseHandle(h); return A3_ERR_IO; }
    usize size = (usize)sz.QuadPart;
    u8 *data = (u8 *)a3_malloc(size + 1, tag);
    if (!data) { CloseHandle(h); return A3_ERR_OUT_OF_MEMORY; }
    usize got = 0;
    while (got < size) {
        DWORD r = 0;
        if (!ReadFile(h, data + got, (DWORD)(size - got), &r, 0) || !r) break;
        got += r;
    }
    CloseHandle(h);
    if (got != size) { a3_free(data); return A3_ERR_IO; }
    data[size] = 0;
    out->data = data;
    out->size = size;
    return A3_OK;
}

A3Result a3_file_write_all(const char *path, const void *data, usize size) {
    wchar_t w[WPATH];
    if (!path || !to_wide(path, w, WPATH)) return A3_ERR_INVALID_ARG;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return A3_ERR_IO;
    const u8 *p = (const u8 *)data;
    usize left = size;
    while (left) {
        DWORD wr = 0;
        if (!WriteFile(h, p, (DWORD)(left > 0x40000000 ? 0x40000000 : left), &wr, 0) || !wr) { CloseHandle(h); return A3_ERR_IO; }
        p += wr;
        left -= wr;
    }
    return CloseHandle(h) ? A3_OK : A3_ERR_IO;
}

A3Result a3_file_write_atomic(const char *path, const void *data, usize size) {
    char tmp[WPATH];
    a3_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    A3Result r = a3_file_write_all(tmp, data, size);
    if (r != A3_OK) return r;
    wchar_t wt[WPATH], wp[WPATH];
    to_wide(tmp, wt, WPATH);
    to_wide(path, wp, WPATH);
    if (!MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(wt); return A3_ERR_IO; }
    return A3_OK;
}

A3Result a3_file_copy(const char *src, const char *dst) {
    wchar_t ws[WPATH], wd[WPATH];
    if (!to_wide(src, ws, WPATH) || !to_wide(dst, wd, WPATH)) return A3_ERR_INVALID_ARG;
    if (CopyFileW(ws, wd, FALSE)) return A3_OK;
    DWORD e = GetLastError();
    return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? A3_ERR_NOT_FOUND : A3_ERR_IO;
}

A3Result a3_file_move(const char *src, const char *dst) {
    wchar_t ws[WPATH], wd[WPATH];
    if (!to_wide(src, ws, WPATH) || !to_wide(dst, wd, WPATH)) return A3_ERR_INVALID_ARG;
    return MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? A3_OK : A3_ERR_IO;
}

A3Result a3_file_delete(const char *path) {
    wchar_t w[WPATH];
    if (!to_wide(path, w, WPATH)) return A3_ERR_INVALID_ARG;
    if (DeleteFileW(w)) return A3_OK;
    return GetLastError() == ERROR_FILE_NOT_FOUND ? A3_ERR_NOT_FOUND : A3_ERR_IO;
}

A3Result a3_dir_create(const char *path) {
    char buf[WPATH];
    a3_strcpy(buf, sizeof(buf), path);
    a3_path_normalize(buf);
    usize n = a3_strlen(buf);
    for (usize i = 1; i <= n; ++i) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        if (i == 2 && buf[1] == ':') continue; /* "C:" */
        char save = buf[i];
        buf[i] = 0;
        wchar_t w[WPATH];
        to_wide(buf, w, WPATH);
        if (!CreateDirectoryW(w, 0) && GetLastError() != ERROR_ALREADY_EXISTS && !a3_dir_exists(buf)) { buf[i] = save; return A3_ERR_IO; }
        buf[i] = save;
    }
    return a3_dir_exists(path) ? A3_OK : A3_ERR_IO;
}

A3Result a3_dir_delete_recursive(const char *path) {
    char pattern[WPATH];
    a3_snprintf(pattern, sizeof(pattern), "%s/*", path);
    wchar_t w[WPATH];
    to_wide(pattern, w, WPATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return a3_dir_exists(path) ? A3_ERR_IO : A3_OK;
    A3Result res = A3_OK;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name[512], child[WPATH];
        to_utf8(fd.cFileName, name, sizeof(name));
        a3_path_join(child, sizeof(child), path, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (a3_dir_delete_recursive(child) != A3_OK) res = A3_ERR_IO; }
        else {
            wchar_t wc[WPATH];
            to_wide(child, wc, WPATH);
            SetFileAttributesW(wc, FILE_ATTRIBUTE_NORMAL); /* clear read-only */
            if (!DeleteFileW(wc)) res = A3_ERR_IO;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    wchar_t wp[WPATH];
    to_wide(path, wp, WPATH);
    if (!RemoveDirectoryW(wp)) res = A3_ERR_IO;
    return res;
}

static int dir_entry_cmp(const void *a, const void *b) { return a3_strcmp(((const A3DirEntry *)a)->name, ((const A3DirEntry *)b)->name); }

A3Result a3_dir_list(const char *path, A3DirVisitFn fn, void *user) {
    char pattern[WPATH];
    a3_snprintf(pattern, sizeof(pattern), "%s/*", path);
    wchar_t w[WPATH];
    to_wide(pattern, w, WPATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return A3_ERR_NOT_FOUND;
    A3_ARRAY_TYPE(A3DirEntry) entries = { 0 };
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        A3DirEntry de;
        a3_zero_struct(&de);
        to_utf8(fd.cFileName, de.name, sizeof(de.name));
        de.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        de.size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        de.mtime_ns = filetime_ns(fd.ftLastWriteTime);
        a3_array_push(entries, de, A3_MEM_TEMP);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (entries.count) qsort(entries.data, entries.count, sizeof(A3DirEntry), dir_entry_cmp);
    for (u32 i = 0; i < entries.count; ++i) if (!fn(path, &entries.data[i], user)) break;
    a3_array_free(entries);
    return A3_OK;
}

/* ---- file watch (mtime polling, same as POSIX) ---- */
typedef struct WatchEntry { u64 path_hash; u64 mtime; } WatchEntry;
struct A3FileWatch {
    char root[WPATH];
    A3FileChangedFn fn;
    void *user;
    A3_ARRAY_TYPE(WatchEntry) entries;
    b32 primed;
};

static u64 watch_hash(const char *s) {
    u64 h = 0xcbf29ce484222325ull;
    while (*s) { h ^= (u8)*s++; h *= 0x100000001b3ull; }
    return h;
}

typedef struct WatchScan { A3FileWatch *w; int depth; } WatchScan;

static b32 watch_visit(const char *dir, const A3DirEntry *e, void *user) {
    WatchScan *ws = (WatchScan *)user;
    A3FileWatch *w = ws->w;
    if (e->name[0] == '.') return 1;
    char full[WPATH];
    a3_path_join(full, sizeof(full), dir, e->name);
    if (e->is_dir) {
        if (ws->depth < 16) { WatchScan sub = { w, ws->depth + 1 }; a3_dir_list(full, watch_visit, &sub); }
        return 1;
    }
    u64 h = watch_hash(full);
    for (u32 i = 0; i < w->entries.count; ++i) {
        if (w->entries.data[i].path_hash == h) {
            if (w->entries.data[i].mtime != e->mtime_ns) {
                w->entries.data[i].mtime = e->mtime_ns;
                if (w->primed && w->fn) w->fn(full, w->user);
            }
            return 1;
        }
    }
    WatchEntry we = { h, e->mtime_ns };
    a3_array_push(w->entries, we, A3_MEM_EDITOR);
    if (w->primed && w->fn) w->fn(full, w->user);
    return 1;
}

A3FileWatch *a3_file_watch_create(const char *root, A3FileChangedFn fn, void *user) {
    A3FileWatch *w = A3_NEW(A3FileWatch, A3_MEM_EDITOR);
    if (!w) return 0;
    a3_strcpy(w->root, sizeof(w->root), root);
    w->fn = fn;
    w->user = user;
    a3_file_watch_poll(w);
    w->primed = 1;
    return w;
}

void a3_file_watch_poll(A3FileWatch *w) {
    if (!w) return;
    WatchScan ws = { w, 0 };
    a3_dir_list(w->root, watch_visit, &ws);
}

void a3_file_watch_destroy(A3FileWatch *w) {
    if (!w) return;
    a3_array_free(w->entries);
    a3_free(w);
}

/* ---- well-known folders ---- */

b32 a3_get_cwd(char *out, usize cap) {
    wchar_t w[WPATH];
    if (!GetCurrentDirectoryW(WPATH, w)) return 0;
    to_utf8(w, out, cap);
    return 1;
}

b32 a3_get_exe_dir(char *out, usize cap) {
    wchar_t w[WPATH];
    DWORD n = GetModuleFileNameW(0, w, WPATH);
    if (!n || n >= WPATH) return 0;
    char buf[WPATH];
    to_utf8(w, buf, sizeof(buf));
    a3_path_dirname(buf, out, cap);
    return 1;
}

b32 a3_get_user_data_dir(char *out, usize cap) {
    wchar_t w[MAX_PATH];
    if (SHGetFolderPathW(0, CSIDL_APPDATA | CSIDL_FLAG_CREATE, 0, 0, w) != S_OK) return 0;
    char base[WPATH];
    to_utf8(w, base, sizeof(base));
    a3_snprintf(out, cap, "%s/ASM3D", base);
    a3_dir_create(out);
    return 1;
}

/* ---- threads ---- */
struct A3Thread { HANDLE handle; A3ThreadFn fn; void *user; };
struct A3Mutex { SRWLOCK lock; };
struct A3Cond { CONDITION_VARIABLE cv; };

b32 a3_threads_supported(void) { return 1; }

static DWORD WINAPI thread_trampoline(LPVOID p) {
    A3Thread *t = (A3Thread *)p;
    t->fn(t->user);
    return 0;
}

A3Thread *a3_thread_create(A3ThreadFn fn, void *user, const char *name) {
    A3_UNUSED(name);
    A3Thread *t = A3_NEW(A3Thread, A3_MEM_JOBS);
    if (!t) return 0;
    t->fn = fn;
    t->user = user;
    t->handle = CreateThread(0, 0, thread_trampoline, t, 0, 0);
    if (!t->handle) { a3_free(t); return 0; }
    return t;
}

void a3_thread_join(A3Thread *t) {
    if (!t) return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    a3_free(t);
}

u32 a3_thread_id(void) {
    static A3AtomicU32 next_id;
    static _Thread_local u32 id;
    if (!id) id = a3_atomic_add_u32(&next_id, 1);
    return id;
}

A3Mutex *a3_mutex_create(void) {
    A3Mutex *m = A3_NEW(A3Mutex, A3_MEM_JOBS);
    if (m) InitializeSRWLock(&m->lock);
    return m;
}
void a3_mutex_destroy(A3Mutex *m) { a3_free(m); }
void a3_mutex_lock(A3Mutex *m) { AcquireSRWLockExclusive(&m->lock); }
void a3_mutex_unlock(A3Mutex *m) { ReleaseSRWLockExclusive(&m->lock); }

A3Cond *a3_cond_create(void) {
    A3Cond *c = A3_NEW(A3Cond, A3_MEM_JOBS);
    if (c) InitializeConditionVariable(&c->cv);
    return c;
}
void a3_cond_destroy(A3Cond *c) { a3_free(c); }
void a3_cond_wait(A3Cond *c, A3Mutex *m) { SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0); }
void a3_cond_signal(A3Cond *c) { WakeConditionVariable(&c->cv); }
void a3_cond_broadcast(A3Cond *c) { WakeAllConditionVariable(&c->cv); }

/* ---- processes ---- */

/* Quotes arguments per the MSVC runtime rules (backslashes before quotes). */
static void build_cmdline(const char *const *argv, char *out, usize cap) {
    usize w = 0;
    out[0] = 0;
    for (u32 i = 0; argv[i]; ++i) {
        const char *a = argv[i];
        b32 quote = !*a || a3_strchr(a, ' ') || a3_strchr(a, '\t') || a3_strchr(a, '"');
        if (i && w + 1 < cap) out[w++] = ' ';
        if (quote && w + 1 < cap) out[w++] = '"';
        for (const char *p = a; *p && w + 4 < cap; ++p) {
            usize bs = 0;
            while (*p == '\\') { ++bs; ++p; }
            if (!*p) { for (usize k = 0; k < (quote ? bs * 2 : bs) && w + 2 < cap; ++k) out[w++] = '\\'; break; }
            if (*p == '"') { for (usize k = 0; k < bs * 2 + 1 && w + 2 < cap; ++k) out[w++] = '\\'; }
            else for (usize k = 0; k < bs && w + 2 < cap; ++k) out[w++] = '\\';
            out[w++] = *p;
        }
        if (quote && w + 1 < cap) out[w++] = '"';
    }
    out[w] = 0;
}

int a3_process_run(const char *const *argv, const char *working_dir, A3ProcessOutputFn out, void *user) {
    if (!argv || !argv[0]) return -1;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    a3_zero_struct(&si);
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    char cmd[4096];
    build_cmdline(argv, cmd, sizeof(cmd));
    static wchar_t wcmd[4096];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 4096);
    wchar_t wdir[WPATH];
    b32 has_dir = working_dir && to_wide(working_dir, wdir, WPATH);
    b32 ok = CreateProcessW(0, wcmd, 0, 0, TRUE, CREATE_NO_WINDOW, 0, has_dir ? wdir : 0, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(rd, buf, sizeof(buf), &n, 0) || !n) break;
        if (out) out(buf, n, user);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

b32 a3_process_spawn_detached(const char *const *argv) {
    if (!argv || !argv[0]) return 0;
    char cmd[4096];
    build_cmdline(argv, cmd, sizeof(cmd));
    static wchar_t wcmd[4096];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 4096);
    /* run from the program's folder so games find their data */
    char dir[WPATH];
    a3_path_dirname(argv[0], dir, sizeof(dir));
    wchar_t wdir[WPATH];
    b32 has_dir = dir[0] && to_wide(dir, wdir, WPATH);
    STARTUPINFOW si;
    a3_zero_struct(&si);
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(0, wcmd, 0, 0, FALSE, DETACHED_PROCESS, 0, has_dir ? wdir : 0, &si, &pi)) return 0;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 1;
}

b32 a3_find_executable(const char *name, char *out_path, usize cap) {
    if (!name || !*name) return 0;
    char with_ext[512];
    a3_strcpy(with_ext, sizeof(with_ext), name);
    if (!a3_str_ends_with(with_ext, ".exe")) a3_strcat(with_ext, sizeof(with_ext), ".exe");
    if (a3_strchr(name, '/') || a3_strchr(name, '\\')) {
        if (a3_file_exists(with_ext)) { a3_strcpy(out_path, cap, with_ext); return 1; }
        return 0;
    }
    wchar_t wn[512], found[WPATH];
    to_wide(with_ext, wn, 512);
    if (!SearchPathW(0, wn, 0, WPATH, found, 0)) return 0;
    to_utf8(found, out_path, cap);
    return 1;
}

#endif /* A3_PLATFORM_WINDOWS */
