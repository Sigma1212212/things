/*
 * ASM3D - a3_jobs.c
 */
#include "a3_jobs.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../platform/a3_platform.h"

#define JOB_QUEUE_SIZE 4096
#define MAX_WORKERS 64

typedef struct Job {
    A3JobFn fn;
    void *user;
    u32 index;
    A3JobCounter *counter;
} Job;

static struct {
    Job queue[JOB_QUEUE_SIZE];
    u32 head, tail, count;
    A3Mutex *mutex;
    A3Cond *cond;
    A3Thread *workers[MAX_WORKERS];
    u32 worker_count;
    volatile b32 running;
    b32 initialized;
} g_jobs;

static void run_job(Job *j) {
    j->fn(j->user, j->index);
    if (j->counter) a3_atomic_sub_u32(&j->counter->pending, 1);
}

/* Pops a job if available; caller holds no lock. */
static b32 try_pop(Job *out) {
    b32 got = 0;
    a3_mutex_lock(g_jobs.mutex);
    if (g_jobs.count) {
        *out = g_jobs.queue[g_jobs.head];
        g_jobs.head = (g_jobs.head + 1) % JOB_QUEUE_SIZE;
        g_jobs.count--;
        got = 1;
    }
    a3_mutex_unlock(g_jobs.mutex);
    return got;
}

static void worker_main(void *arg) {
    A3_UNUSED(arg);
    for (;;) {
        a3_mutex_lock(g_jobs.mutex);
        while (g_jobs.running && g_jobs.count == 0) a3_cond_wait(g_jobs.cond, g_jobs.mutex);
        if (!g_jobs.running && g_jobs.count == 0) { a3_mutex_unlock(g_jobs.mutex); return; }
        Job j = g_jobs.queue[g_jobs.head];
        g_jobs.head = (g_jobs.head + 1) % JOB_QUEUE_SIZE;
        g_jobs.count--;
        a3_mutex_unlock(g_jobs.mutex);
        run_job(&j);
    }
}

u32 a3_jobs_init(u32 worker_count) {
    if (g_jobs.initialized) return g_jobs.worker_count;
    g_jobs.initialized = 1;
    g_jobs.mutex = a3_mutex_create();
    g_jobs.cond = a3_cond_create();
    if (!a3_threads_supported()) {
        A3_INFO("jobs", "threads unavailable on this platform: jobs run inline");
        return 0;
    }
    if (worker_count == 0) {
        u32 cpus = a3_cpu_count();
        worker_count = cpus > 1 ? cpus - 1 : 1;
    }
    if (worker_count > MAX_WORKERS) worker_count = MAX_WORKERS;
    g_jobs.running = 1;
    for (u32 i = 0; i < worker_count; ++i) {
        g_jobs.workers[i] = a3_thread_create(worker_main, 0, "a3-worker");
        if (!g_jobs.workers[i]) { A3_WARN("jobs", "failed to start worker %u", i); break; }
        g_jobs.worker_count++;
    }
    A3_INFO("jobs", "job system started with %u worker threads", g_jobs.worker_count);
    return g_jobs.worker_count;
}

void a3_jobs_shutdown(void) {
    if (!g_jobs.initialized) return;
    a3_mutex_lock(g_jobs.mutex);
    g_jobs.running = 0;
    a3_cond_broadcast(g_jobs.cond);
    a3_mutex_unlock(g_jobs.mutex);
    for (u32 i = 0; i < g_jobs.worker_count; ++i) a3_thread_join(g_jobs.workers[i]);
    g_jobs.worker_count = 0;
    a3_cond_destroy(g_jobs.cond);
    a3_mutex_destroy(g_jobs.mutex);
    g_jobs.initialized = 0;
}

u32 a3_jobs_worker_count(void) { return g_jobs.worker_count; }

void a3_jobs_submit(A3JobFn fn, void *user, u32 count, A3JobCounter *counter) {
    if (!A3_VERIFY(fn)) return;
    if (counter) a3_atomic_add_u32(&counter->pending, count);
    if (!g_jobs.initialized) a3_jobs_init(0);
    if (g_jobs.worker_count == 0) {
        for (u32 i = 0; i < count; ++i) { Job j = { fn, user, i, counter }; run_job(&j); }
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        Job j = { fn, user, i, counter };
        a3_mutex_lock(g_jobs.mutex);
        if (g_jobs.count == JOB_QUEUE_SIZE) {
            /* queue full: run inline instead of blocking */
            a3_mutex_unlock(g_jobs.mutex);
            run_job(&j);
            continue;
        }
        g_jobs.queue[g_jobs.tail] = j;
        g_jobs.tail = (g_jobs.tail + 1) % JOB_QUEUE_SIZE;
        g_jobs.count++;
        a3_cond_signal(g_jobs.cond);
        a3_mutex_unlock(g_jobs.mutex);
    }
}

void a3_jobs_wait(A3JobCounter *counter) {
    if (!counter) return;
    while (a3_atomic_load_u32(&counter->pending) != 0) {
        Job j;
        if (g_jobs.worker_count && try_pop(&j)) run_job(&j);
        else a3_cpu_relax();
    }
}

typedef struct ParallelForCtx {
    A3ParallelForFn fn;
    void *user;
    u32 count, batch;
} ParallelForCtx;

static void parallel_for_job(void *user, u32 index) {
    ParallelForCtx *c = (ParallelForCtx *)user;
    u32 begin = index * c->batch;
    u32 end = begin + c->batch;
    if (end > c->count) end = c->count;
    c->fn(c->user, begin, end);
}

void a3_parallel_for(u32 count, u32 batch, A3ParallelForFn fn, void *user) {
    if (!count) return;
    if (batch == 0) batch = 64;
    if (g_jobs.worker_count == 0 || count <= batch) { fn(user, 0, count); return; }
    ParallelForCtx ctx = { fn, user, count, batch };
    A3JobCounter counter = { { 0 } };
    a3_jobs_submit(parallel_for_job, &ctx, (count + batch - 1) / batch, &counter);
    a3_jobs_wait(&counter);
}
