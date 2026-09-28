/*
 * ASM3D - platform_posix.c
 * Linux / macOS implementation of a3_platform.h.
 */
#define _GNU_SOURCE
#include "a3_platform.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_atomic.h"

#if A3_PLATFORM_POSIX

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if A3_PLATFORM_MACOS
#include <mach-o/dyld.h>
#endif

static u64 g_start_ns;

static u64 posix_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

void a3_platform_init(void) {
    if (!g_start_ns) g_start_ns = posix_now_ns();
    signal(SIGPIPE, SIG_IGN); /* local web server must not die on closed sockets */
}

void a3_platform_shutdown(void) {}

const char *a3_platform_name(void) {
#if A3_PLATFORM_MACOS
    return "macOS";
#else
    return "Linux";
#endif
}

u64 a3_time_ns(void) { return posix_now_ns(); }
f64 a3_time_seconds(void) {
    if (!g_start_ns) g_start_ns = posix_now_ns();
    return (f64)(posix_now_ns() - g_start_ns) * 1e-9;
}
u64 a3_wall_clock_unix(void) { return (u64)time(0); }

void a3_sleep_ms(u32 ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void a3_console_write(A3LogLevel level, const char *text, usize len) {
    int fd = level >= A3_LOG_WARN ? 2 : 1;
    static int use_color = -1;
    if (use_color < 0) use_color = isatty(fd) ? 1 : 0;
    if (use_color) {
        const char *c = level >= A3_LOG_ERROR ? "\x1b[31m" : level == A3_LOG_WARN ? "\x1b[33m" : level <= A3_LOG_DEBUG ? "\x1b[90m" : "";
        if (*c) { ssize_t r = write(fd, c, strlen(c)); (void)r; }
    }
    while (len) {
        ssize_t w = write(fd, text, len);
        if (w <= 0) break;
        text += w; len -= (usize)w;
    }
    if (use_color) { ssize_t r = write(fd, "\x1b[0m", 4); (void)r; }
}

void a3_platform_abort(const char *reason) {
    fprintf(stderr, "ASM3D abort: %s\n", reason ? reason : "");
    fflush(stderr);
    abort();
}

void *a3_os_alloc(usize size) {
    void *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}

void a3_os_free(void *ptr, usize size) {
    if (ptr) munmap(ptr, size);
}

usize a3_os_page_size(void) {
    static usize ps;
    if (!ps) ps = (usize)sysconf(_SC_PAGESIZE);
    return ps;
}

u32 a3_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (u32)n : 1;
}

/* ---- Files ---- */

b32 a3_file_info(const char *path, A3FileInfo *out) {
    struct stat st;
    a3_zero_struct(out);
    if (!path || stat(path, &st) != 0) return 0;
    out->exists = 1;
    out->is_dir = S_ISDIR(st.st_mode);
    out->size = (u64)st.st_size;
#if A3_PLATFORM_MACOS
    out->mtime_ns = (u64)st.st_mtimespec.tv_sec * 1000000000ull + (u64)st.st_mtimespec.tv_nsec;
#else
    out->mtime_ns = (u64)st.st_mtim.tv_sec * 1000000000ull + (u64)st.st_mtim.tv_nsec;
#endif
    return 1;
}

b32 a3_file_exists(const char *path) { A3FileInfo fi; return a3_file_info(path, &fi) && !fi.is_dir; }
b32 a3_dir_exists(const char *path) { A3FileInfo fi; return a3_file_info(path, &fi) && fi.is_dir; }

A3Result a3_file_read_all(const char *path, A3MemTag tag, A3FileData *out) {
    a3_zero_struct(out);
    if (!path) return A3_ERR_INVALID_ARG;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? A3_ERR_NOT_FOUND : A3_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) { close(fd); return A3_ERR_IO; }
    usize size = (usize)st.st_size;
    u8 *data = (u8 *)a3_malloc(size + 1, tag);
    if (!data) { close(fd); return A3_ERR_OUT_OF_MEMORY; }
    usize got = 0;
    while (got < size) {
        ssize_t r = read(fd, data + got, size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (usize)r;
    }
    close(fd);
    if (got != size) { a3_free(data); return A3_ERR_IO; }
    data[size] = 0;
    out->data = data;
    out->size = size;
    return A3_OK;
}

A3Result a3_file_write_all(const char *path, const void *data, usize size) {
    if (!path) return A3_ERR_INVALID_ARG;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return A3_ERR_IO;
    const u8 *p = (const u8 *)data;
    usize left = size;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); return A3_ERR_IO; }
        p += w; left -= (usize)w;
    }
    if (close(fd) != 0) return A3_ERR_IO;
    return A3_OK;
}

A3Result a3_file_write_atomic(const char *path, const void *data, usize size) {
    char tmp[1024];
    a3_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    A3Result r = a3_file_write_all(tmp, data, size);
    if (r != A3_OK) return r;
    if (rename(tmp, path) != 0) { unlink(tmp); return A3_ERR_IO; }
    return A3_OK;
}

A3Result a3_file_copy(const char *src, const char *dst) {
    A3FileData fd;
    A3Result r = a3_file_read_all(src, A3_MEM_TEMP, &fd);
    if (r != A3_OK) return r;
    r = a3_file_write_all(dst, fd.data, fd.size);
    a3_free(fd.data);
    return r;
}

A3Result a3_file_move(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return A3_OK;
    if (errno == EXDEV) {
        A3Result r = a3_file_copy(src, dst);
        if (r == A3_OK) unlink(src);
        return r;
    }
    return A3_ERR_IO;
}

A3Result a3_file_delete(const char *path) {
    return unlink(path) == 0 ? A3_OK : (errno == ENOENT ? A3_ERR_NOT_FOUND : A3_ERR_IO);
}

A3Result a3_dir_create(const char *path) {
    char buf[1024];
    a3_strcpy(buf, sizeof(buf), path);
    a3_path_normalize(buf);
    usize n = a3_strlen(buf);
    for (usize i = 1; i <= n; ++i) {
        if (buf[i] == '/' || buf[i] == 0) {
            char save = buf[i];
            buf[i] = 0;
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return A3_ERR_IO;
            buf[i] = save;
        }
    }
    return a3_dir_exists(path) ? A3_OK : A3_ERR_IO;
}

A3Result a3_dir_delete_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) return errno == ENOENT ? A3_OK : A3_ERR_IO;
    struct dirent *e;
    A3Result res = A3_OK;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1024];
        a3_path_join(child, sizeof(child), path, e->d_name);
        if (a3_dir_exists(child)) { if (a3_dir_delete_recursive(child) != A3_OK) res = A3_ERR_IO; }
        else if (unlink(child) != 0) res = A3_ERR_IO;
    }
    closedir(d);
    if (rmdir(path) != 0) res = A3_ERR_IO;
    return res;
}

static int dir_entry_cmp(const void *a, const void *b) {
    const A3DirEntry *x = (const A3DirEntry *)a, *y = (const A3DirEntry *)b;
    return strcmp(x->name, y->name);
}

A3Result a3_dir_list(const char *path, A3DirVisitFn fn, void *user) {
    DIR *d = opendir(path);
    if (!d) return A3_ERR_NOT_FOUND;
    A3_ARRAY_TYPE(A3DirEntry) entries = { 0 };
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        A3DirEntry de;
        a3_zero_struct(&de);
        a3_strcpy(de.name, sizeof(de.name), e->d_name);
        char child[1024];
        a3_path_join(child, sizeof(child), path, e->d_name);
        A3FileInfo fi;
        if (a3_file_info(child, &fi)) { de.is_dir = fi.is_dir; de.size = fi.size; de.mtime_ns = fi.mtime_ns; }
        a3_array_push(entries, de, A3_MEM_TEMP);
    }
    closedir(d);
    if (entries.count) qsort(entries.data, entries.count, sizeof(A3DirEntry), dir_entry_cmp);
    for (u32 i = 0; i < entries.count; ++i) if (!fn(path, &entries.data[i], user)) break;
    a3_array_free(entries);
    return A3_OK;
}

/* ---- File watch (mtime polling) ---- */
typedef struct WatchEntry { u64 path_hash; u64 mtime; } WatchEntry;
struct A3FileWatch {
    char root[1024];
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
    if (e->name[0] == '.') return 1; /* skip hidden (.git, .recovery) */
    char full[1024];
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

b32 a3_get_cwd(char *out, usize cap) { return getcwd(out, cap) != 0; }

b32 a3_get_exe_dir(char *out, usize cap) {
    char buf[1024];
#if A3_PLATFORM_MACOS
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
#endif
    a3_path_dirname(buf, out, cap);
    return 1;
}

b32 a3_get_user_data_dir(char *out, usize cap) {
    const char *home = getenv("HOME");
    if (!home) return 0;
#if A3_PLATFORM_MACOS
    a3_snprintf(out, cap, "%s/Library/Application Support/ASM3D", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) a3_snprintf(out, cap, "%s/asm3d", xdg);
    else a3_snprintf(out, cap, "%s/.local/share/asm3d", home);
#endif
    a3_dir_create(out);
    return 1;
}

/* ---- Threads ---- */
struct A3Thread { pthread_t handle; A3ThreadFn fn; void *user; char name[16]; };
struct A3Mutex { pthread_mutex_t m; };
struct A3Cond { pthread_cond_t c; };

b32 a3_threads_supported(void) { return 1; }

static void *thread_trampoline(void *p) {
    A3Thread *t = (A3Thread *)p;
#if A3_PLATFORM_LINUX
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
#endif
    t->fn(t->user);
    return 0;
}

A3Thread *a3_thread_create(A3ThreadFn fn, void *user, const char *name) {
    A3Thread *t = A3_NEW(A3Thread, A3_MEM_JOBS);
    if (!t) return 0;
    t->fn = fn;
    t->user = user;
    a3_strcpy(t->name, sizeof(t->name), name ? name : "");
    if (pthread_create(&t->handle, 0, thread_trampoline, t) != 0) { a3_free(t); return 0; }
    return t;
}

void a3_thread_join(A3Thread *t) {
    if (!t) return;
    pthread_join(t->handle, 0);
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
    if (m) pthread_mutex_init(&m->m, 0);
    return m;
}
void a3_mutex_destroy(A3Mutex *m) { if (m) { pthread_mutex_destroy(&m->m); a3_free(m); } }
void a3_mutex_lock(A3Mutex *m) { pthread_mutex_lock(&m->m); }
void a3_mutex_unlock(A3Mutex *m) { pthread_mutex_unlock(&m->m); }

A3Cond *a3_cond_create(void) {
    A3Cond *c = A3_NEW(A3Cond, A3_MEM_JOBS);
    if (c) pthread_cond_init(&c->c, 0);
    return c;
}
void a3_cond_destroy(A3Cond *c) { if (c) { pthread_cond_destroy(&c->c); a3_free(c); } }
void a3_cond_wait(A3Cond *c, A3Mutex *m) { pthread_cond_wait(&c->c, &m->m); }
void a3_cond_signal(A3Cond *c) { pthread_cond_signal(&c->c); }
void a3_cond_broadcast(A3Cond *c) { pthread_cond_broadcast(&c->c); }

/* ---- Processes ---- */
int a3_process_run(const char *const *argv, const char *working_dir, A3ProcessOutputFn out, void *user) {
    if (!argv || !argv[0]) return -1;
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        if (working_dir && chdir(working_dir) != 0) _exit(126);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (out) out(buf, (usize)n, user);
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        return code == 127 ? -1 : code;
    }
    return -1;
}

b32 a3_process_spawn_detached(const char *const *argv) {
    if (!argv || !argv[0]) return 0;
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    return 1;
}

b32 a3_find_executable(const char *name, char *out_path, usize cap) {
    if (!name || !*name) return 0;
    if (a3_strchr(name, '/')) {
        if (access(name, X_OK) == 0) { a3_strcpy(out_path, cap, name); return 1; }
        return 0;
    }
    const char *path = getenv("PATH");
    if (!path) return 0;
    A3Str rest = a3_str(path);
    while (rest.len) {
        A3Str dir = a3_str_split_next(&rest, ':');
        char d[512], full[1024];
        a3_str_to_buf(dir, d, sizeof(d));
        a3_path_join(full, sizeof(full), d, name);
        if (access(full, X_OK) == 0) { a3_strcpy(out_path, cap, full); return 1; }
    }
    return 0;
}

#endif /* A3_PLATFORM_POSIX */
