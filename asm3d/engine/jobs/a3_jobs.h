/*
 * ASM3D - a3_jobs.h
 * Job system: a fixed pool of worker threads consuming a shared queue.
 * Jobs are grouped by an A3JobCounter that callers can wait on; waiting
 * threads help execute queued jobs instead of sleeping.
 *
 * On the web runtime (no threads) every job runs inline at submit time, so
 * gameplay code behaves identically, just without parallelism.
 */
#ifndef A3_JOBS_H
#define A3_JOBS_H

#include "../core/a3_base.h"
#include "../core/a3_atomic.h"

A3_EXTERN_C_BEGIN

typedef void (*A3JobFn)(void *user, u32 index);

typedef struct A3JobCounter { A3AtomicU32 pending; } A3JobCounter;

/* worker_count 0 = cpu_count - 1 (at least 1). Returns number of workers
 * started (0 on single-threaded platforms). */
u32  a3_jobs_init(u32 worker_count);
void a3_jobs_shutdown(void);
u32  a3_jobs_worker_count(void);

/* Queues fn(user, i) for i in [0, count). */
void a3_jobs_submit(A3JobFn fn, void *user, u32 count, A3JobCounter *counter);
/* Blocks until counter reaches zero, executing other jobs meanwhile. */
void a3_jobs_wait(A3JobCounter *counter);
/* Splits [0, count) into batches of `batch` items and runs fn per item in
 * parallel, returning when all are done. */
typedef void (*A3ParallelForFn)(void *user, u32 begin, u32 end);
void a3_parallel_for(u32 count, u32 batch, A3ParallelForFn fn, void *user);

A3_EXTERN_C_END

#endif
