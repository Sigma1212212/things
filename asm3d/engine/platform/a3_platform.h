/*
 * ASM3D - a3_platform.h
 * The only interface between engine code and the operating system / browser.
 * Implementations: platform_posix.c (Linux, macOS), platform_web.c (wasm).
 * A Windows implementation is planned; see docs/STATUS.md.
 */
#ifndef A3_PLATFORM_H
#define A3_PLATFORM_H

#include "../core/a3_base.h"
#include "../core/a3_memory.h"
#include "../core/a3_log.h"

A3_EXTERN_C_BEGIN

/* ---- Lifetime ---- */
void a3_platform_init(void);
void a3_platform_shutdown(void);
const char *a3_platform_name(void);

/* ---- Time ---- */
u64  a3_time_ns(void);           /* monotonic */
f64  a3_time_seconds(void);      /* monotonic, seconds since platform init */
u64  a3_wall_clock_unix(void);   /* seconds since 1970 (0 if unavailable) */
void a3_sleep_ms(u32 ms);

/* ---- Console / process ---- */
void a3_console_write(A3LogLevel level, const char *text, usize len);
void a3_platform_abort(const char *reason);

/* ---- OS memory: page granular, zero-filled ---- */
void *a3_os_alloc(usize size);
void  a3_os_free(void *ptr, usize size);
usize a3_os_page_size(void);

/* ---- CPU ---- */
u32  a3_cpu_count(void);

/* ---- Files ----
 * Paths use '/' separators. On web, files live in a JS-side tree
 * (web/a3fs.js) where /user is kept in persistent browser
 * storage. */
typedef struct A3FileInfo {
    b32 exists;
    b32 is_dir;
    u64 size;
    u64 mtime_ns;
} A3FileInfo;

typedef struct A3FileData {
    u8 *data;      /* NUL terminated for convenience (data[size] == 0) */
    usize size;
} A3FileData;

b32      a3_file_info(const char *path, A3FileInfo *out);
b32      a3_file_exists(const char *path);
b32      a3_dir_exists(const char *path);
/* Reads a whole file into a3_malloc'd memory tagged `tag`. Free with a3_free(data). */
A3Result a3_file_read_all(const char *path, A3MemTag tag, A3FileData *out);
A3Result a3_file_write_all(const char *path, const void *data, usize size);
/* Writes to "<path>.tmp" then renames, so a crash never leaves a half file. */
A3Result a3_file_write_atomic(const char *path, const void *data, usize size);
A3Result a3_file_copy(const char *src, const char *dst);
A3Result a3_file_move(const char *src, const char *dst);
A3Result a3_file_delete(const char *path);
A3Result a3_dir_create(const char *path);      /* creates parents as needed */
A3Result a3_dir_delete_recursive(const char *path);

typedef struct A3DirEntry {
    char name[256];
    b32 is_dir;
    u64 size;
    u64 mtime_ns;
} A3DirEntry;
typedef b32 (*A3DirVisitFn)(const char *dir, const A3DirEntry *entry, void *user); /* return false to stop */
/* Lists the direct children of `path` (not recursive), sorted by name. */
A3Result a3_dir_list(const char *path, A3DirVisitFn fn, void *user);

/* File watching. Implemented by polling modification times from
 * a3_file_watch_poll(), which the editor calls a few times per second. */
typedef struct A3FileWatch A3FileWatch;
typedef void (*A3FileChangedFn)(const char *path, void *user);
A3FileWatch *a3_file_watch_create(const char *root_dir, A3FileChangedFn fn, void *user);
void         a3_file_watch_poll(A3FileWatch *w);
void         a3_file_watch_destroy(A3FileWatch *w);

b32  a3_get_cwd(char *out, usize cap);
b32  a3_get_exe_dir(char *out, usize cap);
b32  a3_get_user_data_dir(char *out, usize cap); /* e.g. ~/.local/share/asm3d */

/* ---- Threads (native only; web returns failure and callers fall back) ---- */
typedef struct A3Thread A3Thread;
typedef struct A3Mutex A3Mutex;
typedef struct A3Cond A3Cond;
typedef void (*A3ThreadFn)(void *user);

b32       a3_threads_supported(void);
A3Thread *a3_thread_create(A3ThreadFn fn, void *user, const char *name);
void      a3_thread_join(A3Thread *t);
u32       a3_thread_id(void);
A3Mutex  *a3_mutex_create(void);
void      a3_mutex_destroy(A3Mutex *m);
void      a3_mutex_lock(A3Mutex *m);
void      a3_mutex_unlock(A3Mutex *m);
A3Cond   *a3_cond_create(void);
void      a3_cond_destroy(A3Cond *c);
void      a3_cond_wait(A3Cond *c, A3Mutex *m);
void      a3_cond_signal(A3Cond *c);
void      a3_cond_broadcast(A3Cond *c);

/* ---- Processes (native editor/build tools only) ---- */
typedef void (*A3ProcessOutputFn)(const char *text, usize len, void *user);
/* Runs argv[0] with arguments, streams combined stdout/stderr to `out`,
 * returns the exit code or -1 when the process could not start. */
int  a3_process_run(const char *const *argv, const char *working_dir, A3ProcessOutputFn out, void *user);
/* Starts a detached process (e.g. opening a browser). */
b32  a3_process_spawn_detached(const char *const *argv);
b32  a3_find_executable(const char *name, char *out_path, usize cap);

A3_EXTERN_C_END

#endif
