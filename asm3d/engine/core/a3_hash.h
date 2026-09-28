/*
 * ASM3D - a3_hash.h
 * Hashing, deterministic random numbers and a u64 -> u64 hash map.
 */
#ifndef A3_HASH_H
#define A3_HASH_H

#include "a3_base.h"
#include "a3_memory.h"

A3_EXTERN_C_BEGIN

/* FNV-1a, used for names/identifiers (stable across builds and targets). */
u64 a3_fnv1a64(const void *data, usize len);
u64 a3_hash_str(const char *s);
u32 a3_hash_str32(const char *s);
/* Fast 64-bit content hash for asset packaging (stable, seedable). */
u64 a3_hash64(const void *data, usize len, u64 seed);
A3_INLINE u64 a3_hash_mix64(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}
A3_INLINE u64 a3_hash_combine(u64 a, u64 b) { return a3_hash_mix64(a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2))); }
/* Formats a hash as 16 lowercase hex chars + NUL (buf must hold 17). */
void a3_hash_to_hex(u64 h, char buf[17]);

/* ---- PCG32 deterministic random ---- */
typedef struct A3Rng { u64 state; u64 inc; } A3Rng;
void a3_rng_seed(A3Rng *rng, u64 seed, u64 stream);
u32  a3_rng_u32(A3Rng *rng);
u32  a3_rng_range_u32(A3Rng *rng, u32 bound);        /* [0, bound) unbiased */
i32  a3_rng_range_i32(A3Rng *rng, i32 lo, i32 hi);   /* [lo, hi] */
f32  a3_rng_f32(A3Rng *rng);                          /* [0, 1) */
f32  a3_rng_range_f32(A3Rng *rng, f32 lo, f32 hi);
b32  a3_rng_chance(A3Rng *rng, f32 probability);

/* Deterministic coordinate hashing for procedural generation. */
u32 a3_hash_coords2(i32 x, i32 y, u32 seed);
u32 a3_hash_coords3(i32 x, i32 y, i32 z, u32 seed);
/* Value noise / fractal noise in [-1,1] (deterministic on all targets). */
f32 a3_noise2(f32 x, f32 y, u32 seed);
f32 a3_fbm2(f32 x, f32 y, u32 seed, int octaves, f32 lacunarity, f32 gain);

/* ---- Hash map u64 -> u64 (open addressing, linear probing) ----
 * Key 0 is reserved (use a3_hash_mix64 or offset your keys). */
typedef struct A3HashMap {
    u64 *keys;
    u64 *values;
    u32 capacity;  /* power of two */
    u32 count;
    u32 tombstones;
    A3MemTag tag;
} A3HashMap;

#define A3_HASHMAP_TOMBSTONE (~0ull)

void a3_hashmap_init(A3HashMap *m, u32 initial_capacity, A3MemTag tag);
void a3_hashmap_free(A3HashMap *m);
b32  a3_hashmap_put(A3HashMap *m, u64 key, u64 value);
b32  a3_hashmap_get(const A3HashMap *m, u64 key, u64 *out_value);
b32  a3_hashmap_remove(A3HashMap *m, u64 key);
void a3_hashmap_clear(A3HashMap *m);

A3_EXTERN_C_END

#endif
