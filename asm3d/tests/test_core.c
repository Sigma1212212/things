/*
 * ASM3D - test_core.c : strings, formatting, memory, hashing, jobs
 */
#include "a3_test.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_format.h"
#include "../engine/core/a3_memory.h"
#include "../engine/core/a3_hash.h"
#include "../engine/jobs/a3_jobs.h"

A3_TEST(string_basics) {
    char buf[8];
    A3_CHECK_EQ_INT(a3_strlen("hello"), 5);
    A3_CHECK_EQ_INT(a3_strcpy(buf, sizeof(buf), "truncate me"), 11);
    A3_CHECK_STR(buf, "truncat");
    A3_CHECK(a3_str_starts_with("Assets/player.a3mesh", "Assets/"));
    A3_CHECK(a3_str_ends_with("scene.a3scene", ".a3scene"));
    A3_CHECK(a3_stristr("Character Controller", "controller") != 0);
    A3_CHECK(a3_fuzzy_score("Build Game", "bg") > 0);
    A3_CHECK(a3_fuzzy_score("Build Game", "xyz") == 0);
    A3_CHECK(a3_fuzzy_score("Export HTML", "exp") > a3_fuzzy_score("Open Profiler Export", "exp"));
    A3_CHECK_STR(a3_path_filename("a/b/c.txt"), "c.txt");
    A3_CHECK_STR(a3_path_extension("a/b/c.tar.gz"), ".gz");
    A3_CHECK_STR(a3_path_extension("a/.hidden"), "");
    char dir[64], joined[64], stem[32];
    a3_path_dirname("a/b/c.txt", dir, sizeof(dir));
    A3_CHECK_STR(dir, "a/b");
    a3_path_join(joined, sizeof(joined), "proj/", "/Assets\\x.png");
    A3_CHECK_STR(joined, "proj/Assets/x.png");
    a3_path_stem("x/y/level01.a3scene", stem, sizeof(stem));
    A3_CHECK_STR(stem, "level01");
    A3Str s = A3_STR("  key = value  ");
    A3Str t = a3_str_trim(s);
    A3_CHECK(a3_str_eq_cstr(t, "key = value"));
    A3Str key = a3_str_trim(a3_str_split_next(&t, '='));
    A3_CHECK(a3_str_eq_cstr(key, "key"));
    A3_CHECK(a3_str_eq_cstr(a3_str_trim(t), "value"));
}

A3_TEST(string_parse) {
    i64 i; f64 f;
    A3_CHECK(a3_parse_i64("-1234", 5, &i) && i == -1234);
    A3_CHECK(a3_parse_i64("0xff", 4, &i) && i == 255);
    A3_CHECK(!a3_parse_i64("12a", 3, &i));
    A3_CHECK(a3_parse_f64("3.5", 3, &f) && f == 3.5);
    A3_CHECK(a3_parse_f64("-2.5e3", 6, &f) && f == -2500.0);
    A3_CHECK(a3_parse_f64("1e-2", 4, &f));
    A3_CHECK_NEAR(f, 0.01, 1e-12);
    A3_CHECK(!a3_parse_f64("abc", 3, &f));
    A3_CHECK(!a3_parse_f64("1.0x", 4, &f));
}

A3_TEST(format) {
    char b[128];
    a3_snprintf(b, sizeof(b), "%d|%5d|%-5d|%05d|%u|%x|%X|%s|%c|%%", -42, 7, 7, 42, 3000000000u, 255, 255, "str", 'Z');
    A3_CHECK_STR(b, "-42|    7|7    |00042|3000000000|ff|FF|str|Z|%");
    a3_snprintf(b, sizeof(b), "%lld %llu %zu", (long long)-9000000000LL, 18000000000ULL, (usize)99);
    A3_CHECK_STR(b, "-9000000000 18000000000 99");
    a3_snprintf(b, sizeof(b), "%.3f %.0f %f %.2f", 3.14159, 2.5, 1.0, -0.004);
    A3_CHECK_STR(b, "3.142 3 1.000000 -0.00");
    a3_snprintf(b, sizeof(b), "%g %g %g %g", 0.5, 100000.0, 1e-5, 123456789.0);
    A3_CHECK_STR(b, "0.5 100000 1e-05 1.23457e+08");
    a3_snprintf(b, sizeof(b), "%.9g", 0.1f);
    A3_CHECK_STR(b, "0.100000001");
    a3_snprintf(b, sizeof(b), "[%8.3s]", "abcdef");
    A3_CHECK_STR(b, "[     abc]");
    int n = a3_snprintf(b, 4, "hello");
    A3_CHECK_EQ_INT(n, 5);
    A3_CHECK_STR(b, "hel");
    A3Str sv = A3_STR("view");
    a3_snprintf(b, sizeof(b), "<%.*s>", A3_STR_ARG(sv));
    A3_CHECK_STR(b, "<view>");
}

A3_TEST(memory_heap) {
    A3MemStats before, after;
    a3_mem_get_stats(&before);
    void *ptrs[256];
    for (int i = 0; i < 256; ++i) {
        usize sz = (usize)(i * 97 % 5000) + 1;
        ptrs[i] = a3_malloc(sz, A3_MEM_GAME);
        A3_CHECK(ptrs[i] != 0);
        A3_CHECK(((uptr)ptrs[i] & 15) == 0);
        a3_memset(ptrs[i], i & 0xFF, sz);
        A3_CHECK_EQ_INT(a3_alloc_size(ptrs[i]), sz);
    }
    for (int i = 0; i < 256; ++i) {
        usize sz = (usize)(i * 97 % 5000) + 1;
        const u8 *p = (const u8 *)ptrs[i];
        A3_CHECK(p[0] == (u8)(i & 0xFF) && p[sz - 1] == (u8)(i & 0xFF));
    }
    for (int i = 0; i < 256; i += 2) a3_free(ptrs[i]);
    for (int i = 1; i < 256; i += 2) {
        ptrs[i] = a3_realloc(ptrs[i], 9000, A3_MEM_GAME);
        A3_CHECK(ptrs[i] != 0);
        A3_CHECK(((u8 *)ptrs[i])[0] == (u8)(i & 0xFF));
        a3_free(ptrs[i]);
    }
    /* large allocation path */
    u8 *big = (u8 *)a3_calloc(A3_MB(3), A3_MEM_GAME);
    A3_CHECK(big && big[0] == 0 && big[A3_MB(3) - 1] == 0);
    a3_free(big);
    a3_mem_get_stats(&after);
    A3_CHECK_EQ_INT(after.tags[A3_MEM_GAME].current_bytes, before.tags[A3_MEM_GAME].current_bytes);
    A3_CHECK_EQ_INT(after.tags[A3_MEM_GAME].live_allocs, before.tags[A3_MEM_GAME].live_allocs);
    A3_CHECK(after.tags[A3_MEM_GAME].peak_bytes >= A3_MB(3));
    char *d = a3_strdup("dup", A3_MEM_CORE);
    A3_CHECK_STR(d, "dup");
    a3_free(d);
}

A3_TEST(memory_heap_misuse) {
    A3MemStats s0, s1;
    a3_mem_get_stats(&s0);
    b32 console = 1;
    a3_log_set_console(0); /* expected errors: keep test output clean */
    void *p = a3_malloc(64, A3_MEM_CORE);
    a3_free(p);
    a3_free(p);                 /* double free: detected, ignored */
    int local = 0;
    a3_free(&local);            /* foreign pointer: detected, ignored */
    a3_free(0);                 /* NULL: no-op */
    a3_log_set_console(console);
    a3_mem_get_stats(&s1);
    A3_CHECK_EQ_INT(s1.invalid_frees - s0.invalid_frees, 2);
    /* heap still healthy */
    void *q = a3_malloc(64, A3_MEM_CORE);
    A3_CHECK(q != 0);
    a3_free(q);
}

A3_TEST(memory_arena) {
    A3Arena a;
    a3_arena_init(&a, A3_MEM_TEMP, 1024);
    u8 *x = (u8 *)a3_arena_push(&a, 100, 1);
    u32 *y = (u32 *)a3_arena_push(&a, sizeof(u32) * 4, 64);
    A3_CHECK(x && y);
    A3_CHECK(((uptr)y & 63) == 0);
    A3ArenaMark m = a3_arena_mark(&a);
    void *big = a3_arena_push(&a, 5000, 16); /* forces a new block */
    A3_CHECK(big != 0);
    A3_CHECK(a.total_reserved > 1024);
    a3_arena_reset_to(&a, m);
    A3_CHECK_EQ_INT(a.total_used, m.total_used);
    char *s = a3_arena_printf(&a, "%s-%d", "entity", 42);
    A3_CHECK_STR(s, "entity-42");
    a3_arena_reset(&a);
    A3_CHECK_EQ_INT(a.total_used, 0);
    a3_arena_release(&a);
    A3_CHECK(a.current == 0);
    a3_log_set_console(0);
    A3_CHECK(a3_arena_push(&a, 8, 3) == 0); /* non-pow2 alignment rejected with an error */
    a3_log_set_console(1);
    a3_arena_release(&a);
}

A3_TEST(memory_linear_stack) {
    A3_ALIGNAS(16) u8 buf[256];
    A3Linear lin;
    a3_linear_init(&lin, buf, sizeof(buf));
    A3_CHECK(a3_linear_push(&lin, 200, 16) != 0);
    A3_CHECK(a3_linear_push(&lin, 100, 16) == 0); /* full -> NULL, not crash */
    a3_linear_reset(&lin);
    A3_CHECK(a3_linear_push(&lin, 100, 16) != 0);
    A3_CHECK_EQ_INT(lin.peak, 200);

    A3Stack st;
    a3_stack_init(&st, buf, sizeof(buf));
    void *a = a3_stack_push(&st, 32, 16);
    void *b = a3_stack_push(&st, 32, 16);
    A3_CHECK(a && b);
    a3_log_set_console(0);
    A3_CHECK(!a3_stack_pop(&st, a)); /* out of order: rejected */
    a3_log_set_console(1);
    A3_CHECK(a3_stack_pop(&st, b));
    A3_CHECK(a3_stack_pop(&st, a));
    A3_CHECK_EQ_INT(st.top, 0);
}

A3_TEST(memory_pool) {
    A3Pool pool;
    a3_pool_init(&pool, 48, 16, A3_MEM_CORE);
    void *items[100];
    for (int i = 0; i < 100; ++i) { items[i] = a3_pool_alloc(&pool); A3_CHECK(items[i] != 0); }
    A3_CHECK_EQ_INT(pool.live, 100);
    A3_CHECK(pool.capacity >= 100);
    for (int i = 0; i < 100; i += 3) a3_pool_free(&pool, items[i]);
    int x = 0;
    a3_log_set_console(0);
    a3_pool_free(&pool, &x); /* foreign: ignored */
    a3_log_set_console(1);
    A3_CHECK_EQ_INT(pool.live, 100 - 34);
    void *r = a3_pool_alloc(&pool);
    A3_CHECK(r != 0);
    a3_pool_release(&pool);
    A3_CHECK_EQ_INT(pool.live, 0);
}

A3_TEST(memory_frame) {
    a3_frame_alloc_init(A3_KB(64));
    a3_frame_alloc_begin();
    u32 *a = (u32 *)a3_frame_alloc(sizeof(u32), 4);
    *a = 1234;
    a3_frame_alloc_begin();                 /* next frame: previous still valid */
    A3_CHECK_EQ_INT(*a, 1234);
    void *b = a3_frame_alloc(A3_KB(200), 16); /* larger than block -> grows */
    A3_CHECK(b != 0);
    a3_frame_alloc_begin();                 /* frame N+2 reuses frame N memory */
    A3_CHECK(a3_frame_alloc_used() == 0);
    a3_frame_alloc_shutdown();
}

A3_TEST(memory_array) {
    A3_ARRAY_TYPE(i32) arr = { 0 };
    for (i32 i = 0; i < 1000; ++i) A3_CHECK(a3_array_push(arr, i * 2, A3_MEM_CORE));
    A3_CHECK_EQ_INT(arr.count, 1000);
    A3_CHECK_EQ_INT(arr.data[999], 1998);
    a3_array_remove_swap(arr, 0);
    A3_CHECK_EQ_INT(arr.data[0], 1998);
    A3_CHECK_EQ_INT(a3_array_pop(arr), 1996);
    a3_array_free(arr);
    A3_CHECK(arr.data == 0 && arr.count == 0);
}

A3_TEST(hash_and_rng) {
    A3_CHECK(a3_hash_str("Player") == a3_hash_str("Player"));
    A3_CHECK(a3_hash_str("Player") != a3_hash_str("player"));
    A3_CHECK(a3_fnv1a64("", 0) == 0xcbf29ce484222325ull);
    u8 data[100];
    for (int i = 0; i < 100; ++i) data[i] = (u8)i;
    u64 h1 = a3_hash64(data, 100, 0), h2 = a3_hash64(data, 100, 1);
    data[50] ^= 1;
    A3_CHECK(h1 != h2);
    A3_CHECK(a3_hash64(data, 100, 0) != h1);
    char hex[17];
    a3_hash_to_hex(0x0123456789abcdefull, hex);
    A3_CHECK_STR(hex, "0123456789abcdef");

    A3Rng r1, r2;
    a3_rng_seed(&r1, 42, 54);
    a3_rng_seed(&r2, 42, 54);
    for (int i = 0; i < 100; ++i) A3_CHECK(a3_rng_u32(&r1) == a3_rng_u32(&r2));
    /* PCG32 reference output for seed 42, stream 54 (from the PCG paper demo) */
    A3Rng ref;
    a3_rng_seed(&ref, 42, 54);
    A3_CHECK(a3_rng_u32(&ref) == 0xa15c02b7u);
    A3_CHECK(a3_rng_u32(&ref) == 0x7b47f409u);
    for (int i = 0; i < 1000; ++i) {
        f32 f = a3_rng_f32(&r1);
        A3_CHECK(f >= 0.0f && f < 1.0f);
        i32 v = a3_rng_range_i32(&r1, -3, 3);
        A3_CHECK(v >= -3 && v <= 3);
    }
    f32 n = a3_fbm2(12.3f, 4.5f, 7, 5, 2.0f, 0.5f);
    A3_CHECK(n >= -1.0f && n <= 1.0f);
    A3_CHECK(a3_noise2(1.0f, 2.0f, 3) == a3_noise2(1.0f, 2.0f, 3));
}

A3_TEST(hashmap) {
    A3HashMap m;
    a3_hashmap_init(&m, 0, A3_MEM_CORE);
    for (u64 i = 1; i <= 5000; ++i) A3_CHECK(a3_hashmap_put(&m, i * 7919, i));
    A3_CHECK_EQ_INT(m.count, 5000);
    u64 v = 0;
    A3_CHECK(a3_hashmap_get(&m, 7919 * 123, &v) && v == 123);
    for (u64 i = 1; i <= 5000; i += 2) A3_CHECK(a3_hashmap_remove(&m, i * 7919));
    A3_CHECK(!a3_hashmap_get(&m, 7919 * 1, &v));
    A3_CHECK(a3_hashmap_get(&m, 7919 * 2, &v) && v == 2);
    A3_CHECK(a3_hashmap_put(&m, 7919 * 2, 99) && a3_hashmap_get(&m, 7919 * 2, &v) && v == 99);
    A3_CHECK_EQ_INT(m.count, 2500);
    a3_hashmap_free(&m);
}

typedef struct SumCtx { u32 *data; A3AtomicU64 sum; } SumCtx;
static void sum_range(void *user, u32 begin, u32 end) {
    SumCtx *c = (SumCtx *)user;
    u64 local = 0;
    for (u32 i = begin; i < end; ++i) { c->data[i] = i; local += i; }
    a3_atomic_add_u64(&c->sum, local);
}

A3_TEST(jobs_parallel_for) {
    a3_jobs_init(0);
    const u32 N = 100000;
    SumCtx ctx;
    ctx.data = A3_NEW_ARRAY(u32, N, A3_MEM_TEMP);
    ctx.sum.v = 0;
    a3_parallel_for(N, 1000, sum_range, &ctx);
    A3_CHECK(a3_atomic_load_u64(&ctx.sum) == (u64)N * (N - 1) / 2);
    A3_CHECK(ctx.data[N - 1] == N - 1);
    a3_free(ctx.data);
    a3_jobs_shutdown();
}
