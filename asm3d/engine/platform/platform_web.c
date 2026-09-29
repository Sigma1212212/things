/*
 * ASM3D - platform_web.c
 * WebAssembly implementation of a3_platform.h.
 *
 * The JavaScript side (runtime/web/asm3d.js) provides a handful of imports in
 * the "a3" module. Everything else - memory management, the package virtual
 * file system, formatting - runs inside wasm linear memory.
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
A3_WASM_IMPORT("a3", "storage_write") i32 js_storage_write(const char *key, i32 key_len, const void *data, i32 len);
A3_WASM_IMPORT("a3", "storage_size") i32 js_storage_size(const char *key, i32 key_len);
A3_WASM_IMPORT("a3", "storage_read") i32 js_storage_read(const char *key, i32 key_len, void *dst, i32 cap);
A3_WASM_IMPORT("a3", "storage_delete") void js_storage_delete(const char *key, i32 key_len);

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

/* ---- Virtual file system ----
 * The loader mounts package files into wasm memory (a3_web_mount_file).
 * Paths starting with "save:/" go to browser storage via the JS bridge. */
typedef struct VfsFile {
    char path[192];
    u64 hash;
    u8 *data;
    usize size;
} VfsFile;

static A3_ARRAY_TYPE(VfsFile) g_vfs;

static VfsFile *vfs_find(const char *path) {
    while (path[0] == '.' && path[1] == '/') path += 2;
    u64 h = a3_hash_str(path);
    for (u32 i = 0; i < g_vfs.count; ++i) if (g_vfs.data[i].hash == h && a3_streq(g_vfs.data[i].path, path)) return &g_vfs.data[i];
    return 0;
}

A3_WASM_EXPORT("a3_web_alloc") void *a3_web_alloc(u32 size) { return a3_malloc(size ? size : 1, A3_MEM_RESOURCE); }
A3_WASM_EXPORT("a3_web_free") void a3_web_free(void *p) { a3_free(p); }

/* Takes ownership of `data` (allocated with a3_web_alloc). */
A3_WASM_EXPORT("a3_web_mount_file") i32 a3_web_mount_file(const char *path, u8 *data, u32 size) {
    VfsFile *existing = vfs_find(path);
    if (existing) { a3_free(existing->data); existing->data = data; existing->size = size; return 1; }
    VfsFile f;
    a3_zero_struct(&f);
    a3_strcpy(f.path, sizeof(f.path), path);
    f.hash = a3_hash_str(f.path);
    f.data = data;
    f.size = size;
    return a3_array_push(g_vfs, f, A3_MEM_RESOURCE) ? 1 : 0;
}

static b32 is_save_path(const char *p) { return a3_str_starts_with(p, "save:/"); }

b32 a3_file_info(const char *path, A3FileInfo *out) {
    a3_zero_struct(out);
    if (!path) return 0;
    if (is_save_path(path)) {
        i32 n = js_storage_size(path, (i32)a3_strlen(path));
        if (n < 0) return 0;
        out->exists = 1; out->size = (u64)n;
        return 1;
    }
    VfsFile *f = vfs_find(path);
    if (f) { out->exists = 1; out->size = f->size; return 1; }
    /* directories: any mounted file with this prefix */
    usize l = a3_strlen(path);
    for (u32 i = 0; i < g_vfs.count; ++i) {
        if (a3_strncmp(g_vfs.data[i].path, path, l) == 0 && g_vfs.data[i].path[l] == '/') { out->exists = 1; out->is_dir = 1; return 1; }
    }
    return 0;
}

b32 a3_file_exists(const char *path) { A3FileInfo fi; return a3_file_info(path, &fi) && !fi.is_dir; }
b32 a3_dir_exists(const char *path) { A3FileInfo fi; return a3_file_info(path, &fi) && fi.is_dir; }

A3Result a3_file_read_all(const char *path, A3MemTag tag, A3FileData *out) {
    a3_zero_struct(out);
    if (!path) return A3_ERR_INVALID_ARG;
    if (is_save_path(path)) {
        i32 kl = (i32)a3_strlen(path);
        i32 n = js_storage_size(path, kl);
        if (n < 0) return A3_ERR_NOT_FOUND;
        u8 *d = (u8 *)a3_malloc((usize)n + 1, tag);
        if (!d) return A3_ERR_OUT_OF_MEMORY;
        if (js_storage_read(path, kl, d, n) != n) { a3_free(d); return A3_ERR_IO; }
        d[n] = 0;
        out->data = d; out->size = (usize)n;
        return A3_OK;
    }
    VfsFile *f = vfs_find(path);
    if (!f) return A3_ERR_NOT_FOUND;
    u8 *d = (u8 *)a3_malloc(f->size + 1, tag);
    if (!d) return A3_ERR_OUT_OF_MEMORY;
    a3_memcpy(d, f->data, f->size);
    d[f->size] = 0;
    out->data = d; out->size = f->size;
    return A3_OK;
}

A3Result a3_file_write_all(const char *path, const void *data, usize size) {
    if (!path) return A3_ERR_INVALID_ARG;
    if (!is_save_path(path)) return A3_ERR_UNSUPPORTED; /* package is read only */
    return js_storage_write(path, (i32)a3_strlen(path), data, (i32)size) ? A3_OK : A3_ERR_IO;
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
    A3Result r = a3_file_copy(src, dst);
    if (r == A3_OK) a3_file_delete(src);
    return r;
}

A3Result a3_file_delete(const char *path) {
    if (!is_save_path(path)) return A3_ERR_UNSUPPORTED;
    js_storage_delete(path, (i32)a3_strlen(path));
    return A3_OK;
}

A3Result a3_dir_create(const char *path) { A3_UNUSED(path); return A3_OK; }
A3Result a3_dir_delete_recursive(const char *path) { A3_UNUSED(path); return A3_ERR_UNSUPPORTED; }

A3Result a3_dir_list(const char *path, A3DirVisitFn fn, void *user) {
    usize l = a3_strlen(path);
    b32 root = l == 0 || (l == 1 && path[0] == '.');
    for (u32 i = 0; i < g_vfs.count; ++i) {
        const char *p = g_vfs.data[i].path;
        const char *rest;
        if (root) rest = p;
        else if (a3_strncmp(p, path, l) == 0 && p[l] == '/') rest = p + l + 1;
        else continue;
        if (a3_strchr(rest, '/')) continue; /* not a direct child (dirs not synthesized) */
        A3DirEntry e;
        a3_zero_struct(&e);
        a3_strcpy(e.name, sizeof(e.name), rest);
        e.size = g_vfs.data[i].size;
        if (!fn(path, &e, user)) break;
    }
    return A3_OK;
}

A3FileWatch *a3_file_watch_create(const char *root, A3FileChangedFn fn, void *user) { A3_UNUSED(root); A3_UNUSED(fn); A3_UNUSED(user); return 0; }
void a3_file_watch_poll(A3FileWatch *w) { A3_UNUSED(w); }
void a3_file_watch_destroy(A3FileWatch *w) { A3_UNUSED(w); }

b32 a3_get_cwd(char *out, usize cap) { a3_strcpy(out, cap, "."); return 1; }
b32 a3_get_exe_dir(char *out, usize cap) { a3_strcpy(out, cap, "."); return 1; }
b32 a3_get_user_data_dir(char *out, usize cap) { a3_strcpy(out, cap, "save:"); return 1; }

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
