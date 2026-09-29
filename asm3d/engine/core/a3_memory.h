/*
 * ASM3D - a3_memory.h
 * Engine memory system. Every allocation is attributed to a subsystem tag so
 * the profiler can show exactly where memory goes (native and web).
 *
 * Allocators:
 *   Persistent heap  a3_malloc/a3_realloc/a3_free   size-class heap, thread safe
 *   Arena            A3Arena                        growable chained bump allocator
 *   Linear           A3Linear                       fixed-buffer bump allocator
 *   Stack            A3Stack                        LIFO with checked pops
 *   Pool             A3Pool                         fixed-size blocks, O(1) alloc/free
 *   Frame            a3_frame_alloc                 double-buffered, reset each frame
 *
 * Misuse (double free, foreign pointer, out-of-order stack pop) is detected,
 * logged and ignored rather than corrupting memory.
 */
#ifndef A3_MEMORY_H
#define A3_MEMORY_H

#include "a3_base.h"

A3_EXTERN_C_BEGIN

typedef enum A3MemTag {
    A3_MEM_CORE = 0,
    A3_MEM_TEMP,
    A3_MEM_JOBS,
    A3_MEM_ECS,
    A3_MEM_SCENE,
    A3_MEM_RESOURCE,
    A3_MEM_RENDER,
    A3_MEM_PHYSICS,
    A3_MEM_AUDIO,
    A3_MEM_ANIM,
    A3_MEM_PARTICLES,
    A3_MEM_SCRIPT,
    A3_MEM_WORLD,
    A3_MEM_AI,
    A3_MEM_UI,
    A3_MEM_EDITOR,
    A3_MEM_NET,
    A3_MEM_GAME,
    A3_MEM_TAG_COUNT
} A3MemTag;

typedef struct A3MemTagStats {
    u64 current_bytes;
    u64 peak_bytes;
    u64 total_allocs;
    u64 live_allocs;
} A3MemTagStats;

typedef struct A3MemStats {
    A3MemTagStats tags[A3_MEM_TAG_COUNT];
    u64 os_reserved_bytes; /* bytes obtained from the OS / wasm memory.grow */
    u64 invalid_frees;     /* detected misuse count */
} A3MemStats;

const char *a3_mem_tag_name(A3MemTag tag);
void a3_mem_get_stats(A3MemStats *out);
/* Internal: allocators report into the stats table. */
void a3_mem_track_alloc(A3MemTag tag, usize bytes);
void a3_mem_track_free(A3MemTag tag, usize bytes);
void a3_mem_track_os(i64 delta_bytes);

/* ---- Persistent heap (16-byte aligned) ---- */
void *a3_malloc(usize size, A3MemTag tag);
void *a3_calloc(usize size, A3MemTag tag);
void *a3_realloc(void *ptr, usize new_size, A3MemTag tag);
void  a3_free(void *ptr);
usize a3_alloc_size(const void *ptr); /* usable size, 0 if invalid */
char *a3_strdup(const char *s, A3MemTag tag);

#define A3_NEW(type, tag) ((type *)a3_calloc(sizeof(type), (tag)))
#define A3_NEW_ARRAY(type, n, tag) ((type *)a3_calloc(sizeof(type) * (usize)(n), (tag)))

/* ---- Generic allocator interface (containers accept any allocator) ----
 * fn(self, old_ptr, old_size, new_size, align):
 *   old_ptr == NULL        -> allocate new_size
 *   new_size == 0          -> free old_ptr
 *   otherwise              -> resize
 */
typedef void *(*A3AllocFn)(void *self, void *old_ptr, usize old_size, usize new_size, usize align);
typedef struct A3Allocator {
    A3AllocFn fn;
    void *self;
} A3Allocator;

A3Allocator a3_heap_allocator(A3MemTag tag);
A3_INLINE void *a3_allocator_alloc(A3Allocator a, usize size, usize align) { return a.fn(a.self, 0, 0, size, align); }
A3_INLINE void *a3_allocator_realloc(A3Allocator a, void *p, usize old_size, usize new_size, usize align) { return a.fn(a.self, p, old_size, new_size, align); }
A3_INLINE void  a3_allocator_free(A3Allocator a, void *p, usize size) { if (p) a.fn(a.self, p, size, 0, 1); }

/* ---- Arena (growable, chained blocks) ---- */
typedef struct A3ArenaBlock A3ArenaBlock;
typedef struct A3Arena {
    A3ArenaBlock *current;
    usize block_size;
    usize total_used;
    usize total_reserved;
    A3MemTag tag;
} A3Arena;

typedef struct A3ArenaMark {
    A3ArenaBlock *block;
    usize used;
    usize total_used;
} A3ArenaMark;

void  a3_arena_init(A3Arena *arena, A3MemTag tag, usize block_size);
void *a3_arena_push(A3Arena *arena, usize size, usize align);
void *a3_arena_push_zero(A3Arena *arena, usize size, usize align);
char *a3_arena_strdup(A3Arena *arena, const char *s);
char *a3_arena_printf(A3Arena *arena, const char *fmt, ...) A3_PRINTF_LIKE(2, 3);
A3ArenaMark a3_arena_mark(A3Arena *arena);
void  a3_arena_reset_to(A3Arena *arena, A3ArenaMark mark);
void  a3_arena_reset(A3Arena *arena);   /* keeps first block */
void  a3_arena_release(A3Arena *arena); /* returns all memory */
A3Allocator a3_arena_allocator(A3Arena *arena);
#define A3_ARENA_PUSH(arena, type) ((type *)a3_arena_push_zero((arena), sizeof(type), _Alignof(type)))
#define A3_ARENA_PUSH_ARRAY(arena, type, n) ((type *)a3_arena_push_zero((arena), sizeof(type) * (usize)(n), _Alignof(type)))

/* ---- Linear (fixed buffer) ---- */
typedef struct A3Linear {
    u8 *base;
    usize capacity;
    usize used;
    usize peak;
} A3Linear;

void  a3_linear_init(A3Linear *lin, void *buffer, usize capacity);
void *a3_linear_push(A3Linear *lin, usize size, usize align); /* NULL when full */
void  a3_linear_reset(A3Linear *lin);

/* ---- Stack (LIFO) ---- */
typedef struct A3Stack {
    u8 *base;
    usize capacity;
    usize top;
    u32 depth;
} A3Stack;

void  a3_stack_init(A3Stack *st, void *buffer, usize capacity);
void *a3_stack_push(A3Stack *st, usize size, usize align);
/* Must pop the most recent allocation; otherwise logs an error and returns false. */
b32   a3_stack_pop(A3Stack *st, void *ptr);

/* ---- Pool (fixed-size blocks) ---- */
typedef struct A3Pool {
    void *free_list;
    void *chunks;       /* linked list of chunk allocations */
    usize elem_size;
    u32 elems_per_chunk;
    u32 live;
    u32 capacity;
    A3MemTag tag;
} A3Pool;

void  a3_pool_init(A3Pool *pool, usize elem_size, u32 elems_per_chunk, A3MemTag tag);
void *a3_pool_alloc(A3Pool *pool);
void  a3_pool_free(A3Pool *pool, void *ptr);
void  a3_pool_release(A3Pool *pool);

/* ---- Frame allocator (global, double buffered) ----
 * Memory from a3_frame_alloc stays valid until the end of the *next* frame,
 * which lets one frame's results be read during the following frame. */
void  a3_frame_alloc_init(usize block_size);
void  a3_frame_alloc_begin(void); /* call once at the start of each frame */
void *a3_frame_alloc(usize size, usize align);
usize a3_frame_alloc_used(void);
void  a3_frame_alloc_shutdown(void);

/* ---- Thread-local scratch arena for short-lived temporaries ----
 *   A3ArenaMark m = a3_scratch_begin(); ... a3_scratch_end(m);          */
A3Arena *a3_scratch(void);
A3ArenaMark a3_scratch_begin(void);
void a3_scratch_end(A3ArenaMark mark);

/* ---- Dynamic arrays ----
 * typedef A3_ARRAY_TYPE(u32) U32Array;  U32Array a = {0};
 * a3_array_push(a, 5, A3_MEM_CORE);     a3_array_free(a);                 */
#define A3_ARRAY_TYPE(T) struct { T *data; u32 count; u32 cap; }
b32 a3__array_fit(void **data, u32 *cap, usize elem_size, u32 needed, A3MemTag tag);
#define a3_array_reserve(arr, n, tag) a3__array_fit((void **)&(arr).data, &(arr).cap, sizeof(*(arr).data), (u32)(n), (tag))
#define a3_array_push(arr, value, tag) \
    (a3__array_fit((void **)&(arr).data, &(arr).cap, sizeof(*(arr).data), (arr).count + 1, (tag)) \
        ? ((arr).data[(arr).count++] = (value), 1) : 0)
#define a3_array_pop(arr) ((arr).data[--(arr).count])
#define a3_array_last(arr) ((arr).data[(arr).count - 1])
#define a3_array_clear(arr) ((arr).count = 0)
#define a3_array_remove_swap(arr, i) ((arr).data[(i)] = (arr).data[--(arr).count])
#define a3_array_free(arr) do { a3_free((arr).data); (arr).data = 0; (arr).count = (arr).cap = 0; } while (0)

A3_EXTERN_C_END

#endif
