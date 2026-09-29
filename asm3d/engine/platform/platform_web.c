/*
 * ASM3D - platform_web.c
 * WebAssembly implementation of a3_platform.h.
 *
 * The JavaScript side (web/asm3d.js, or tools/run_wasm_tests.mjs under Node)
 * provides a handful of imports in the "a3" and "fs" modules. Everything
 * else - memory management, formatting - runs inside wasm linear memory.
 */
#include "a3_platform.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_hash.h"

#if A3_PLATFORM_WEB

/* ---- JS imports (keep this list short: it is the whole platform bridge) ---- */
A3_WASM_IMPORT("a3", "log") void js_log(i32 level, const char *ptr, i32 len);
A3_WASM_IMPORT("a3", "now_ms") f64 js_now_ms(void);
A3_WASM_IMPORT("a3", "wall_clock") f64 js_wall_clock(void);
A3_WASM_IMPORT("a3", "abort") void js_abort(const char *ptr, i32 len);
/* file system: a JS-side tree of files (web/asm3d.js keeps it in IndexedDB) */
A3_WASM_IMPORT("fs", "stat") f64 js_fs_stat(const char *path, i32 len, f64 *mtime_ms);   /* size, -1 missing, -2 directory */
A3_WASM_IMPORT("fs", "read") i32 js_fs_read(const char *path, i32 len, void *dst, i32 cap);
A3_WASM_IMPORT("fs", "write") i32 js_fs_write(const char *path, i32 len, const void *data, i32 size);
A3_WASM_IMPORT("fs", "remove") i32 js_fs_remove(const char *path, i32 len, i32 recursive);
A3_WASM_IMPORT("fs", "rename") i32 js_fs_rename(const char *src, i32 slen, const char *dst, i32 dlen);
A3_WASM_IMPORT("fs", "mkdir") i32 js_fs_mkdir(const char *path, i32 len);
A3_WASM_IMPORT("fs", "list") i32 js_fs_list(const char *path, i32 len, char *dst, i32 cap);   /* "name\tsize\tis_dir\tmtime\n"..., returns bytes needed */

extern u8 __heap_base;

static f64 g_start_ms;

void a3_platform_init(void) { g_start_ms = js_now_ms(); }
void a3_platform_shutdown(void) {}
const char *a3_platform_name(void) { return "Web (WebAssembly)"; }

u64 a3_time_ns(void) { return (u64)(js_now_ms() * 1000000.0); }
f64 a3_time_seconds(void) { return (js_now_ms() - g_start_ms) * 0.001; }
u64 a3_wall_clock_unix(void) { return (u64)js_wall_clock(); }
void a3_sleep_ms(u32 ms) { A3_UNUSED(ms); /* cannot block the browser main thread */ }

void a3_console_write(A3LogLevel level, const char *text, usize len) {
    if (len && text[len - 1] == '\n') --len;
    js_log((i32)level, text, (i32)len);
}

void a3_platform_abort(const char *reason) {
    js_abort(reason, (i32)a3_strlen(reason));
    __builtin_trap();
}

/* ---- OS memory: region allocator on top of memory.grow ----
 * Freed regions are kept in an address-sorted list and coalesced so large
 * allocations can be recycled (wasm memory can never shrink). */
#define WEB_PAGE 4096u
#define WASM_PAGE 65536u
#define MAX_FREE_REGIONS 2048

typedef struct Region { uptr base; usize size; } Region;
static Region g_free[MAX_FREE_REGIONS];
static u32 g_free_count;
static uptr g_top;

static b32 ensure_memory(uptr end) {
    usize have = __builtin_wasm_memory_size(0) * (usize)WASM_PAGE;
    if (end <= have) return 1;
    usize need_pages = (end - have + WASM_PAGE - 1) / WASM_PAGE;
    if (__builtin_wasm_memory_grow(0, need_pages) == (usize)-1) return 0;
    return 1;
}

void *a3_os_alloc(usize size) {
    size = A3_ALIGN_UP(size, WEB_PAGE);
    for (u32 i = 0; i < g_free_count; ++i) {
        if (g_free[i].size >= size) {
            uptr p = g_free[i].base;
            g_free[i].base += size;
            g_free[i].size -= size;
            if (g_free[i].size == 0) {
                a3_memmove(&g_free[i], &g_free[i + 1], (g_free_count - i - 1) * sizeof(Region));
                g_free_count--;
            }
            a3_memset((void *)p, 0, size);
            return (void *)p;
        }
    }
    if (!g_top) g_top = A3_ALIGN_UP((uptr)&__heap_base, WEB_PAGE);
    uptr p = g_top;
    if (!ensure_memory(p + size)) return 0;
    g_top += size;
    return (void *)p; /* fresh wasm memory is zeroed */
}

void a3_os_free(void *ptr, usize size) {
    if (!ptr) return;
    size = A3_ALIGN_UP(size, WEB_PAGE);
    uptr base = (uptr)ptr;
    /* find insertion index */
    u32 i = 0;
    while (i < g_free_count && g_free[i].base < base) ++i;
    /* merge with previous / next */
    b32 merged = 0;
    if (i > 0 && g_free[i - 1].base + g_free[i - 1].size == base) {
        g_free[i - 1].size += size;
        merged = 1;
        if (i < g_free_count && g_free[i - 1].base + g_free[i - 1].size == g_free[i].base) {
            g_free[i - 1].size += g_free[i].size;
            a3_memmove(&g_free[i], &g_free[i + 1], (g_free_count - i - 1) * sizeof(Region));
            g_free_count--;
        }
    } else if (i < g_free_count && base + size == g_free[i].base) {
        g_free[i].base = base;
        g_free[i].size += size;
        merged = 1;
    }
    if (!merged) {
        if (g_free_count >= MAX_FREE_REGIONS) return; /* leak rather than corrupt */
        a3_memmove(&g_free[i + 1], &g_free[i], (g_free_count - i) * sizeof(Region));
        g_free[i].base = base;
        g_free[i].size = size;
        g_free_count++;
    }
}

usize a3_os_page_size(void) { return WEB_PAGE; }
u32 a3_cpu_count(void) { return 1; }

/* ---- File system ----
 * Paths are absolute ("/user/projects/MyGame/project.a3proj"); relative
 * paths are resolved against "/". Files live in a JS map; web/asm3d.js
 * persists /user to IndexedDB and can mount project folders and zips. */

#define WEB_PATH_MAX 1024
static i32 plen(const char *p) { return (i32)a3_strlen(p); }

static const char *norm(const char *path, char *buf, usize cap) {
    while (path[0] == '.' && path[1] == '/') path += 2;
    if (path[0] == '/') return path;
    if (path[0] == '.' && path[1] == 0) return "/";
    a3_snprintf(buf, cap, "/%s", path);
    return buf;
}

A3_WASM_EXPORT("a3_web_alloc") void *a3_web_alloc(u32 size) { return a3_malloc(size ? size : 1, A3_MEM_RESOURCE); }
A3_WASM_EXPORT("a3_web_free") void a3_web_free(void *p) { a3_free(p); }

b32 a3_file_info(const char *path, A3FileInfo *out) {
    a3_zero_struct(out);
    if (!path || !*path) return 0;
    char b[WEB_PATH_MAX];
    path = norm(path, b, sizeof(b));
    f64 mt = 0;
    f64 n = js_fs_stat(path, plen(path), &mt);
    if (n == -1.0) return 0;
    out->exists = 1;
    out->is_dir = n == -2.0;
    out->size = n >= 0 ? (u64)n : 0;
    out->mtime_ns = (u64)(mt * 1000000.0);
    return 1;
}

b32 a3_file_exists(const char *path) { A3FileInfo fi; return a3_file_info(path, &fi) && !fi.is_dir; }
b32 a3_dir_exists(const char *path) { A3FileInfo fi; return a3_file_info(path, &fi) && fi.is_dir; }

A3Result a3_file_read_all(const char *path, A3MemTag tag, A3FileData *out) {
    a3_zero_struct(out);
    if (!path) return A3_ERR_INVALID_ARG;
    char b[WEB_PATH_MAX];
    path = norm(path, b, sizeof(b));
    f64 mt = 0;
    f64 n = js_fs_stat(path, plen(path), &mt);
    if (n < 0) return A3_ERR_NOT_FOUND;
    u8 *d = (u8 *)a3_malloc((usize)n + 1, tag);
    if (!d) return A3_ERR_OUT_OF_MEMORY;
    if (js_fs_read(path, plen(path), d, (i32)n) != (i32)n) { a3_free(d); return A3_ERR_IO; }
    d[(usize)n] = 0;
    out->data = d;
    out->size = (usize)n;
    return A3_OK;
}

A3Result a3_file_write_all(const char *path, const void *data, usize size) {
    if (!path) return A3_ERR_INVALID_ARG;
    char b[WEB_PATH_MAX];
    path = norm(path, b, sizeof(b));
    return js_fs_write(path, plen(path), data, (i32)size) ? A3_OK : A3_ERR_IO;
}

A3Result a3_file_write_atomic(const char *path, const void *data, usize size) { return a3_file_write_all(path, data, size); }

A3Result a3_file_copy(const char *src, const char *dst) {
    A3FileData fd;
    A3Result r = a3_file_read_all(src, A3_MEM_TEMP, &fd);
    if (r != A3_OK) return r;
    r = a3_file_write_all(dst, fd.data, fd.size);
    a3_free(fd.data);
    return r;
}

A3Result a3_file_move(const char *src, const char *dst) {
    char b1[WEB_PATH_MAX], b2[WEB_PATH_MAX];
    src = norm(src, b1, sizeof(b1));
    dst = norm(dst, b2, sizeof(b2));
    return js_fs_rename(src, plen(src), dst, plen(dst)) ? A3_OK : A3_ERR_NOT_FOUND;
}

A3Result a3_file_delete(const char *path) {
    char b[WEB_PATH_MAX];
    path = norm(path, b, sizeof(b));
    return js_fs_remove(path, plen(path), 0) ? A3_OK : A3_ERR_NOT_FOUND;
}

A3Result a3_dir_create(const char *path) {
    char b[WEB_PATH_MAX];
    path = norm(path, b, sizeof(b));
    return js_fs_mkdir(path, plen(path)) ? A3_OK : A3_ERR_IO;
}

A3Result a3_dir_delete_recursive(const char *path) {
    char b[WEB_PATH_MAX];
    path = norm(path, b, sizeof(b));
    return js_fs_remove(path, plen(path), 1) ? A3_OK : A3_ERR_NOT_FOUND;
}

A3Result a3_dir_list(const char *path, A3DirVisitFn fn, void *user) {
    char b[WEB_PATH_MAX];
    const char *p = norm(path, b, sizeof(b));
    i32 cap = 16384;
    char *buf = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        buf = (char *)a3_malloc((usize)cap + 1, A3_MEM_TEMP);
        if (!buf) return A3_ERR_OUT_OF_MEMORY;
        i32 need = js_fs_list(p, plen(p), buf, cap);
        if (need < 0) { a3_free(buf); return A3_ERR_NOT_FOUND; }
        if (need <= cap) { buf[need] = 0; break; }
        a3_free(buf);
        buf = 0;
        cap = need + 1024;
    }
    if (!buf) return A3_ERR_IO;
    for (char *line = buf; *line;) {
        char *eol = (char *)a3_strchr(line, '\n');
        if (eol) *eol = 0;
        char *f[4] = { line, 0, 0, 0 };
        for (int k = 1; k < 4; ++k) { char *tab = f[k - 1] ? (char *)a3_strchr(f[k - 1], '\t') : 0; if (tab) { *tab = 0; f[k] = tab + 1; } }
        if (f[0][0]) {
            A3DirEntry e;
            a3_zero_struct(&e);
            a3_strcpy(e.name, sizeof(e.name), f[0]);
            f64 v = 0;
            if (f[1]) { a3_parse_f64(f[1], a3_strlen(f[1]), &v); e.size = (u64)v; }
            e.is_dir = f[2] && f[2][0] == '1';
            if (f[3]) { a3_parse_f64(f[3], a3_strlen(f[3]), &v); e.mtime_ns = (u64)(v * 1000000.0); }
            if (!fn(path, &e, user)) break;
        }
        if (!eol) break;
        line = eol + 1;
    }
    a3_free(buf);
    return A3_OK;
}

A3FileWatch *a3_file_watch_create(const char *root, A3FileChangedFn fn, void *user) { A3_UNUSED(root); A3_UNUSED(fn); A3_UNUSED(user); return 0; }
void a3_file_watch_poll(A3FileWatch *w) { A3_UNUSED(w); }
void a3_file_watch_destroy(A3FileWatch *w) { A3_UNUSED(w); }

b32 a3_get_cwd(char *out, usize cap) { a3_strcpy(out, cap, "/"); return 1; }
b32 a3_get_exe_dir(char *out, usize cap) { a3_strcpy(out, cap, "/app"); return 1; }
b32 a3_get_user_data_dir(char *out, usize cap) { a3_strcpy(out, cap, "/user"); return 1; }

/* ---- Threads: single-threaded web runtime ----
 * Wasm threads need SharedArrayBuffer + cross-origin isolation; the job
 * system detects a3_threads_supported() == 0 and runs jobs inline. Mutexes
 * and condition variables are no-ops so shared code runs unchanged. */
struct A3Mutex { int unused; };
struct A3Cond { int unused; };
static struct A3Mutex g_dummy_mutex;
static struct A3Cond g_dummy_cond;

b32 a3_threads_supported(void) { return 0; }
A3Thread *a3_thread_create(A3ThreadFn fn, void *user, const char *name) { A3_UNUSED(fn); A3_UNUSED(user); A3_UNUSED(name); return 0; }
void a3_thread_join(A3Thread *t) { A3_UNUSED(t); }
u32 a3_thread_id(void) { return 1; }
A3Mutex *a3_mutex_create(void) { return &g_dummy_mutex; }
void a3_mutex_destroy(A3Mutex *m) { A3_UNUSED(m); }
void a3_mutex_lock(A3Mutex *m) { A3_UNUSED(m); }
void a3_mutex_unlock(A3Mutex *m) { A3_UNUSED(m); }
A3Cond *a3_cond_create(void) { return &g_dummy_cond; }
void a3_cond_destroy(A3Cond *c) { A3_UNUSED(c); }
void a3_cond_wait(A3Cond *c, A3Mutex *m) { A3_UNUSED(c); A3_UNUSED(m); }
void a3_cond_signal(A3Cond *c) { A3_UNUSED(c); }
void a3_cond_broadcast(A3Cond *c) { A3_UNUSED(c); }

int a3_process_run(const char *const *argv, const char *wd, A3ProcessOutputFn out, void *user) {
    A3_UNUSED(argv); A3_UNUSED(wd); A3_UNUSED(out); A3_UNUSED(user);
    return -1;
}
b32 a3_process_spawn_detached(const char *const *argv) { A3_UNUSED(argv); return 0; }
b32 a3_find_executable(const char *name, char *out, usize cap) { A3_UNUSED(name); A3_UNUSED(out); A3_UNUSED(cap); return 0; }

#endif /* A3_PLATFORM_WEB */
