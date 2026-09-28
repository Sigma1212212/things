/*
 * ASM3D - a3_memory.c
 */
#include "a3_memory.h"
#include "a3_atomic.h"
#include "a3_log.h"
#include "a3_string.h"
#include "a3_format.h"
#include "../platform/a3_platform.h"

/* ======================================================================== */
/* Stats                                                                    */
/* ======================================================================== */

static struct {
    A3AtomicU64 current[A3_MEM_TAG_COUNT];
    A3AtomicU64 peak[A3_MEM_TAG_COUNT];
    A3AtomicU64 total[A3_MEM_TAG_COUNT];
    A3AtomicU64 live[A3_MEM_TAG_COUNT];
    A3AtomicU64 os_bytes;
    A3AtomicU64 invalid_frees;
} g_mem_stats;

static const char *g_tag_names[A3_MEM_TAG_COUNT] = {
    "Core", "Temp", "Jobs", "ECS", "Scene", "Resource", "Render", "Physics",
    "Audio", "Animation", "Particles", "Script", "World", "AI", "UI", "Editor",
    "Network", "Game",
};

const char *a3_mem_tag_name(A3MemTag tag) {
    return (u32)tag < A3_MEM_TAG_COUNT ? g_tag_names[tag] : "?";
}

void a3_mem_track_alloc(A3MemTag tag, usize bytes) {
    if ((u32)tag >= A3_MEM_TAG_COUNT) tag = A3_MEM_CORE;
    u64 cur = a3_atomic_add_u64(&g_mem_stats.current[tag], bytes);
    a3_atomic_add_u64(&g_mem_stats.total[tag], 1);
    a3_atomic_add_u64(&g_mem_stats.live[tag], 1);
    /* Peak update is racy-but-monotonic; good enough for diagnostics. */
    if (cur > a3_atomic_load_u64(&g_mem_stats.peak[tag])) a3_atomic_store_u64(&g_mem_stats.peak[tag], cur);
}

void a3_mem_track_free(A3MemTag tag, usize bytes) {
    if ((u32)tag >= A3_MEM_TAG_COUNT) tag = A3_MEM_CORE;
    a3_atomic_sub_u64(&g_mem_stats.current[tag], bytes);
    a3_atomic_sub_u64(&g_mem_stats.live[tag], 1);
}

void a3_mem_track_os(i64 delta) {
    if (delta >= 0) a3_atomic_add_u64(&g_mem_stats.os_bytes, (u64)delta);
    else a3_atomic_sub_u64(&g_mem_stats.os_bytes, (u64)(-delta));
}

void a3_mem_get_stats(A3MemStats *out) {
    if (!out) return;
    for (u32 i = 0; i < A3_MEM_TAG_COUNT; ++i) {
        out->tags[i].current_bytes = a3_atomic_load_u64(&g_mem_stats.current[i]);
        out->tags[i].peak_bytes = a3_atomic_load_u64(&g_mem_stats.peak[i]);
        out->tags[i].total_allocs = a3_atomic_load_u64(&g_mem_stats.total[i]);
        out->tags[i].live_allocs = a3_atomic_load_u64(&g_mem_stats.live[i]);
    }
    out->os_reserved_bytes = a3_atomic_load_u64(&g_mem_stats.os_bytes);
    out->invalid_frees = a3_atomic_load_u64(&g_mem_stats.invalid_frees);
}

/* ======================================================================== */
/* Persistent heap                                                          */
/* ======================================================================== */
/*
 * Small blocks (<= 32 KiB) come from per-size-class free lists carved out of
 * 256 KiB spans. Large blocks go straight to the OS layer. Every block has a
 * 16-byte header so free() needs no size and misuse can be detected.
 */

#define HEAP_MAGIC_LIVE 0xA3A1u
#define HEAP_MAGIC_FREE 0xA3F0u
#define HEAP_CLASS_LARGE 0xFFu
#define HEAP_SPAN_SIZE A3_KB(256)
#define HEAP_MAX_SMALL A3_KB(32)

typedef struct HeapHeader {
    u32 size;        /* requested size (small) or low 32 bits (large) */
    u16 magic;
    u8  cls;         /* size class index or HEAP_CLASS_LARGE */
    u8  tag;
    u32 large_hi;    /* high bits of requested size for huge blocks */
    u32 reserved;
} HeapHeader;
A3_STATIC_ASSERT(sizeof(HeapHeader) == 16, "heap header must stay 16 bytes for alignment");

typedef struct HeapFree { struct HeapFree *next; } HeapFree;
A3_INLINE HeapFree *heap_link(HeapHeader *h) { return (HeapFree *)((u8 *)h + 8); }
A3_INLINE HeapHeader *heap_from_link(HeapFree *f) { return (HeapHeader *)((u8 *)f - 8); }

/* Size classes: 16..128 step 16, then 4 steps per power of two up to 32 KiB. */
static u32 g_class_size[64];
static u32 g_class_count;
static HeapFree *g_free_lists[64];
static u8 *g_span_cursor;
static usize g_span_remaining;
static A3Spinlock g_heap_lock;
static b32 g_heap_ready;

static void heap_init_classes(void) {
    u32 n = 0;
    for (u32 s = 16; s <= 128; s += 16) g_class_size[n++] = s;
    for (u32 p = 128; p < HEAP_MAX_SMALL; p *= 2) {
        u32 step = p / 4;
        for (u32 k = 1; k <= 4; ++k) g_class_size[n++] = p + step * k;
    }
    g_class_count = n;
    g_heap_ready = 1;
}

static u32 heap_class_for(usize total) {
    /* binary search smallest class >= total */
    u32 lo = 0, hi = g_class_count - 1;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_class_size[mid] >= total) hi = mid; else lo = mid + 1;
    }
    return lo;
}

static usize heap_header_size(const HeapHeader *h) {
    return (usize)h->size | ((usize)h->large_hi << 16 << 16);
}

void *a3_malloc(usize size, A3MemTag tag) {
    if (size == 0) size = 1;
    if ((u32)tag >= A3_MEM_TAG_COUNT) tag = A3_MEM_CORE;
    usize total = size + sizeof(HeapHeader);
    if (total < size) return 0; /* overflow */
    HeapHeader *h;
    if (total <= HEAP_MAX_SMALL) {
        a3_spin_lock(&g_heap_lock);
        if (!g_heap_ready) heap_init_classes();
        u32 cls = heap_class_for(total);
        u32 csize = g_class_size[cls];
        HeapFree *f = g_free_lists[cls];
        if (f) {
            g_free_lists[cls] = f->next;
            h = heap_from_link(f);
        } else {
            if (g_span_remaining < csize) {
                /* Remaining tail of the old span is recycled into small classes. */
                while (g_span_remaining >= 16) {
                    u32 c = heap_class_for(g_span_remaining);
                    if (g_class_size[c] > g_span_remaining) { if (c == 0) break; --c; }
                    HeapHeader *th = (HeapHeader *)g_span_cursor;
                    th->magic = HEAP_MAGIC_FREE;
                    HeapFree *t = heap_link(th);
                    t->next = g_free_lists[c]; g_free_lists[c] = t;
                    g_span_cursor += g_class_size[c]; g_span_remaining -= g_class_size[c];
                }
                u8 *span = (u8 *)a3_os_alloc(HEAP_SPAN_SIZE);
                if (!span) {
                    a3_spin_unlock(&g_heap_lock);
                    A3_ERROR("memory", "out of memory allocating %zu bytes (%s)", size, a3_mem_tag_name(tag));
                    return 0;
                }
                a3_mem_track_os((i64)HEAP_SPAN_SIZE);
                g_span_cursor = span;
                g_span_remaining = HEAP_SPAN_SIZE;
            }
            h = (HeapHeader *)g_span_cursor;
            g_span_cursor += csize;
            g_span_remaining -= csize;
        }
        a3_spin_unlock(&g_heap_lock);
        h->cls = (u8)cls;
        h->size = (u32)size;
        h->large_hi = 0;
    } else {
        usize page = a3_os_page_size();
        usize bytes = A3_ALIGN_UP(total, page);
        h = (HeapHeader *)a3_os_alloc(bytes);
        if (!h) {
            A3_ERROR("memory", "out of memory allocating %zu bytes (%s)", size, a3_mem_tag_name(tag));
            return 0;
        }
        a3_mem_track_os((i64)bytes);
        h->cls = HEAP_CLASS_LARGE;
        h->size = (u32)size;
        h->large_hi = (u32)((u64)size >> 32);
    }
    h->magic = HEAP_MAGIC_LIVE;
    h->tag = (u8)tag;
    h->reserved = 0;
    a3_mem_track_alloc(tag, size);
    return h + 1;
}

void *a3_calloc(usize size, A3MemTag tag) {
    void *p = a3_malloc(size, tag);
    if (p) a3_memset(p, 0, size);
    return p;
}

static HeapHeader *heap_validate(const void *ptr, const char *op) {
    if (!ptr) return 0;
    if (((uptr)ptr & 15u) != 0) {
        A3_ERROR("memory", "%s: pointer %p is not a heap pointer (misaligned); ignored", op, ptr);
        a3_atomic_add_u64(&g_mem_stats.invalid_frees, 1);
        return 0;
    }
    HeapHeader *h = (HeapHeader *)ptr - 1;
    if (h->magic == HEAP_MAGIC_FREE) {
        A3_ERROR("memory", "%s: double free of %p detected; ignored", op, ptr);
        a3_atomic_add_u64(&g_mem_stats.invalid_frees, 1);
        return 0;
    }
    if (h->magic != HEAP_MAGIC_LIVE) {
        A3_ERROR("memory", "%s: %p was not allocated by a3_malloc (or header corrupted); ignored", op, ptr);
        a3_atomic_add_u64(&g_mem_stats.invalid_frees, 1);
        return 0;
    }
    return h;
}

usize a3_alloc_size(const void *ptr) {
    const HeapHeader *h = ptr ? (const HeapHeader *)ptr - 1 : 0;
    if (!h || h->magic != HEAP_MAGIC_LIVE) return 0;
    return heap_header_size(h);
}

void a3_free(void *ptr) {
    HeapHeader *h = heap_validate(ptr, "a3_free");
    if (!h) return;
    usize size = heap_header_size(h);
    a3_mem_track_free((A3MemTag)h->tag, size);
    h->magic = HEAP_MAGIC_FREE;
    if (h->cls == HEAP_CLASS_LARGE) {
        usize bytes = A3_ALIGN_UP(size + sizeof(HeapHeader), a3_os_page_size());
        a3_os_free(h, bytes);
        a3_mem_track_os(-(i64)bytes);
        return;
    }
    a3_spin_lock(&g_heap_lock);
    /* The free-list link lives in bytes 8..15 of the header so the magic
     * (bytes 4..5) stays intact and double frees remain detectable. */
    HeapFree *f = heap_link(h);
    f->next = g_free_lists[h->cls];
    g_free_lists[h->cls] = f;
    a3_spin_unlock(&g_heap_lock);
}

void *a3_realloc(void *ptr, usize new_size, A3MemTag tag) {
    if (!ptr) return a3_malloc(new_size, tag);
    if (new_size == 0) { a3_free(ptr); return 0; }
    HeapHeader *h = heap_validate(ptr, "a3_realloc");
    if (!h) return 0;
    usize old = heap_header_size(h);
    if (h->cls != HEAP_CLASS_LARGE && new_size + sizeof(HeapHeader) <= g_class_size[h->cls]) {
        a3_mem_track_free((A3MemTag)h->tag, old);
        a3_mem_track_alloc((A3MemTag)h->tag, new_size);
        a3_atomic_sub_u64(&g_mem_stats.total[h->tag], 1);
        h->size = (u32)new_size;
        return ptr;
    }
    void *n = a3_malloc(new_size, (A3MemTag)h->tag);
    if (!n) return 0;
    a3_memcpy(n, ptr, old < new_size ? old : new_size);
    a3_free(ptr);
    return n;
}

char *a3_strdup(const char *s, A3MemTag tag) {
    usize n = a3_strlen(s);
    char *d = (char *)a3_malloc(n + 1, tag);
    if (d) { if (n) a3_memcpy(d, s, n); d[n] = 0; }
    return d;
}

static void *heap_alloc_fn(void *self, void *old_ptr, usize old_size, usize new_size, usize align) {
    A3_UNUSED(old_size);
    A3MemTag tag = (A3MemTag)(uptr)self;
    if (align > 16) {
        A3_ERROR("memory", "heap allocator supports alignment <= 16 (requested %zu)", align);
        return 0;
    }
    if (!old_ptr) return a3_malloc(new_size, tag);
    if (new_size == 0) { a3_free(old_ptr); return 0; }
    return a3_realloc(old_ptr, new_size, tag);
}

A3Allocator a3_heap_allocator(A3MemTag tag) {
    A3Allocator a = { heap_alloc_fn, (void *)(uptr)tag };
    return a;
}

/* ======================================================================== */
/* Arena                                                                    */
/* ======================================================================== */

struct A3ArenaBlock {
    A3ArenaBlock *prev;
    usize size;   /* usable bytes after header */
    usize used;
    usize pad;
};

void a3_arena_init(A3Arena *arena, A3MemTag tag, usize block_size) {
    if (!A3_VERIFY(arena)) return;
    a3_zero_struct(arena);
    arena->tag = tag;
    arena->block_size = block_size ? block_size : A3_KB(64);
}

static A3ArenaBlock *arena_new_block(A3Arena *arena, usize min_size) {
    usize size = arena->block_size;
    if (min_size > size) size = min_size;
    A3ArenaBlock *b = (A3ArenaBlock *)a3_malloc(sizeof(A3ArenaBlock) + size, arena->tag);
    if (!b) return 0;
    b->prev = arena->current;
    b->size = size;
    b->used = 0;
    arena->current = b;
    arena->total_reserved += size;
    return b;
}

void *a3_arena_push(A3Arena *arena, usize size, usize align) {
    if (!A3_VERIFY(arena)) return 0;
    if (align == 0) align = 1;
    if (!A3_IS_POW2(align)) { A3_ERROR("memory", "arena alignment %zu is not a power of two", align); return 0; }
    A3ArenaBlock *b = arena->current;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (b) {
            uptr base = (uptr)(b + 1);
            uptr p = A3_ALIGN_UP(base + b->used, (uptr)align);
            usize new_used = (usize)(p - base) + size;
            if (new_used <= b->size) {
                arena->total_used += new_used - b->used;
                b->used = new_used;
                return (void *)p;
            }
        }
        b = arena_new_block(arena, size + align);
        if (!b) {
            A3_ERROR("memory", "arena out of memory (%zu bytes, %s)", size, a3_mem_tag_name(arena->tag));
            return 0;
        }
    }
    return 0;
}

void *a3_arena_push_zero(A3Arena *arena, usize size, usize align) {
    void *p = a3_arena_push(arena, size, align);
    if (p) a3_memset(p, 0, size);
    return p;
}

char *a3_arena_strdup(A3Arena *arena, const char *s) {
    usize n = a3_strlen(s);
    char *d = (char *)a3_arena_push(arena, n + 1, 1);
    if (d) { if (n) a3_memcpy(d, s, n); d[n] = 0; }
    return d;
}

char *a3_arena_printf(A3Arena *arena, const char *fmt, ...) {
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = a3_vsnprintf(0, 0, fmt, a);
    va_end(a);
    char *d = (char *)a3_arena_push(arena, (usize)n + 1, 1);
    if (d) a3_vsnprintf(d, (usize)n + 1, fmt, b);
    va_end(b);
    return d;
}

A3ArenaMark a3_arena_mark(A3Arena *arena) {
    A3ArenaMark m = { arena->current, arena->current ? arena->current->used : 0, arena->total_used };
    return m;
}

void a3_arena_reset_to(A3Arena *arena, A3ArenaMark mark) {
    while (arena->current && arena->current != mark.block) {
        A3ArenaBlock *prev = arena->current->prev;
        arena->total_reserved -= arena->current->size;
        a3_free(arena->current);
        arena->current = prev;
    }
    if (arena->current) arena->current->used = mark.used;
    arena->total_used = mark.total_used;
}

void a3_arena_reset(A3Arena *arena) {
    if (!arena->current) return;
    while (arena->current->prev) {
        A3ArenaBlock *prev = arena->current->prev;
        arena->total_reserved -= arena->current->size;
        a3_free(arena->current);
        arena->current = prev;
    }
    arena->current->used = 0;
    arena->total_used = 0;
}

void a3_arena_release(A3Arena *arena) {
    while (arena->current) {
        A3ArenaBlock *prev = arena->current->prev;
        a3_free(arena->current);
        arena->current = prev;
    }
    arena->total_used = arena->total_reserved = 0;
}

static void *arena_alloc_fn(void *self, void *old_ptr, usize old_size, usize new_size, usize align) {
    A3Arena *arena = (A3Arena *)self;
    if (new_size == 0) return 0; /* arenas free in bulk */
    void *p = a3_arena_push(arena, new_size, align ? align : 16);
    if (p && old_ptr && old_size) a3_memcpy(p, old_ptr, old_size < new_size ? old_size : new_size);
    return p;
}

A3Allocator a3_arena_allocator(A3Arena *arena) {
    A3Allocator a = { arena_alloc_fn, arena };
    return a;
}

/* ======================================================================== */
/* Linear                                                                   */
/* ======================================================================== */

void a3_linear_init(A3Linear *lin, void *buffer, usize capacity) {
    if (!A3_VERIFY(lin)) return;
    lin->base = (u8 *)buffer;
    lin->capacity = buffer ? capacity : 0;
    lin->used = 0;
    lin->peak = 0;
}

void *a3_linear_push(A3Linear *lin, usize size, usize align) {
    if (!A3_VERIFY(lin)) return 0;
    if (align == 0) align = 1;
    uptr base = (uptr)lin->base;
    uptr p = A3_ALIGN_UP(base + lin->used, (uptr)align);
    usize end = (usize)(p - base) + size;
    if (end > lin->capacity) return 0;
    lin->used = end;
    if (end > lin->peak) lin->peak = end;
    return (void *)p;
}

void a3_linear_reset(A3Linear *lin) { if (lin) lin->used = 0; }

/* ======================================================================== */
/* Stack                                                                    */
/* ======================================================================== */

typedef struct StackHeader {
    u32 prev_top;
    u32 magic;
} StackHeader;

void a3_stack_init(A3Stack *st, void *buffer, usize capacity) {
    if (!A3_VERIFY(st)) return;
    st->base = (u8 *)buffer;
    st->capacity = buffer ? capacity : 0;
    st->top = 0;
    st->depth = 0;
}

void *a3_stack_push(A3Stack *st, usize size, usize align) {
    if (!A3_VERIFY(st)) return 0;
    if (align < 8) align = 8;
    uptr base = (uptr)st->base;
    uptr p = A3_ALIGN_UP(base + st->top + sizeof(StackHeader), (uptr)align);
    usize end = (usize)(p - base) + size;
    if (end > st->capacity || end > 0xFFFFFFFFu) return 0;
    StackHeader *h = (StackHeader *)p - 1;
    h->prev_top = (u32)st->top;
    h->magic = 0x5747A3u + st->depth;
    st->top = end;
    st->depth++;
    return (void *)p;
}

b32 a3_stack_pop(A3Stack *st, void *ptr) {
    if (!A3_VERIFY(st) || !ptr || st->depth == 0) {
        A3_ERROR("memory", "stack pop with nothing to pop");
        return 0;
    }
    StackHeader *h = (StackHeader *)ptr - 1;
    if (h->magic != 0x5747A3u + (st->depth - 1)) {
        A3_ERROR("memory", "stack pop out of LIFO order (%p); ignored", ptr);
        return 0;
    }
    h->magic = 0;
    st->top = h->prev_top;
    st->depth--;
    return 1;
}

/* ======================================================================== */
/* Pool                                                                     */
/* ======================================================================== */

void a3_pool_init(A3Pool *pool, usize elem_size, u32 elems_per_chunk, A3MemTag tag) {
    if (!A3_VERIFY(pool)) return;
    a3_zero_struct(pool);
    if (elem_size < sizeof(void *)) elem_size = sizeof(void *);
    pool->elem_size = A3_ALIGN_UP(elem_size, 8);
    pool->elems_per_chunk = elems_per_chunk ? elems_per_chunk : 64;
    pool->tag = tag;
}

void *a3_pool_alloc(A3Pool *pool) {
    if (!A3_VERIFY(pool) || !pool->elem_size) return 0;
    if (!pool->free_list) {
        usize chunk_bytes = 16 + pool->elem_size * pool->elems_per_chunk;
        u8 *chunk = (u8 *)a3_malloc(chunk_bytes, pool->tag);
        if (!chunk) return 0;
        *(void **)chunk = pool->chunks;
        pool->chunks = chunk;
        u8 *first = chunk + 16;
        for (u32 i = pool->elems_per_chunk; i-- > 0;) {
            void **slot = (void **)(first + i * pool->elem_size);
            *slot = pool->free_list;
            pool->free_list = slot;
        }
        pool->capacity += pool->elems_per_chunk;
    }
    void **slot = (void **)pool->free_list;
    pool->free_list = *slot;
    pool->live++;
    a3_memset(slot, 0, pool->elem_size);
    return slot;
}

void a3_pool_free(A3Pool *pool, void *ptr) {
    if (!A3_VERIFY(pool) || !ptr) return;
    /* Verify ownership: pointer must lie inside one of our chunks on a slot boundary. */
    b32 owned = 0;
    usize span = pool->elem_size * pool->elems_per_chunk;
    for (u8 *c = (u8 *)pool->chunks; c; c = *(u8 **)c) {
        u8 *first = c + 16;
        if ((u8 *)ptr >= first && (u8 *)ptr < first + span && ((usize)((u8 *)ptr - first) % pool->elem_size) == 0) {
            owned = 1;
            break;
        }
    }
    if (!owned) {
        A3_ERROR("memory", "pool free of foreign pointer %p; ignored", ptr);
        return;
    }
    *(void **)ptr = pool->free_list;
    pool->free_list = ptr;
    pool->live--;
}

void a3_pool_release(A3Pool *pool) {
    if (!pool) return;
    u8 *c = (u8 *)pool->chunks;
    while (c) { u8 *next = *(u8 **)c; a3_free(c); c = next; }
    pool->chunks = 0;
    pool->free_list = 0;
    pool->live = pool->capacity = 0;
}

/* ======================================================================== */
/* Frame allocator                                                          */
/* ======================================================================== */

static A3Arena g_frame_arenas[2];
static u32 g_frame_index;
static b32 g_frame_ready;

void a3_frame_alloc_init(usize block_size) {
    for (int i = 0; i < 2; ++i) a3_arena_init(&g_frame_arenas[i], A3_MEM_TEMP, block_size ? block_size : A3_MB(4));
    g_frame_index = 0;
    g_frame_ready = 1;
}

void a3_frame_alloc_begin(void) {
    if (!g_frame_ready) a3_frame_alloc_init(0);
    g_frame_index ^= 1u;
    a3_arena_reset(&g_frame_arenas[g_frame_index]);
}

void *a3_frame_alloc(usize size, usize align) {
    if (!g_frame_ready) a3_frame_alloc_init(0);
    return a3_arena_push(&g_frame_arenas[g_frame_index], size, align ? align : 16);
}

usize a3_frame_alloc_used(void) { return g_frame_arenas[g_frame_index].total_used; }

void a3_frame_alloc_shutdown(void) {
    a3_arena_release(&g_frame_arenas[0]);
    a3_arena_release(&g_frame_arenas[1]);
    g_frame_ready = 0;
}

/* ======================================================================== */
/* Scratch                                                                  */
/* ======================================================================== */

static _Thread_local A3Arena t_scratch;
static _Thread_local b32 t_scratch_ready;

A3Arena *a3_scratch(void) {
    if (!t_scratch_ready) { a3_arena_init(&t_scratch, A3_MEM_TEMP, A3_MB(1)); t_scratch_ready = 1; }
    return &t_scratch;
}
A3ArenaMark a3_scratch_begin(void) { return a3_arena_mark(a3_scratch()); }
void a3_scratch_end(A3ArenaMark mark) { a3_arena_reset_to(a3_scratch(), mark); }

/* ======================================================================== */
/* Dynamic arrays                                                           */
/* ======================================================================== */

b32 a3__array_fit(void **data, u32 *cap, usize elem_size, u32 needed, A3MemTag tag) {
    if (needed <= *cap) return 1;
    u32 new_cap = *cap ? *cap * 2 : 8;
    while (new_cap < needed) new_cap *= 2;
    void *p = a3_realloc(*data, (usize)new_cap * elem_size, tag);
    if (!p) {
        A3_ERROR("memory", "dynamic array growth failed (%u elements of %zu bytes)", new_cap, elem_size);
        return 0;
    }
    *data = p;
    *cap = new_cap;
    return 1;
}
