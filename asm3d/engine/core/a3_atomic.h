/*
 * ASM3D - a3_atomic.h
 * Minimal atomics + spinlock built on compiler builtins. On single-threaded
 * WebAssembly builds these compile to plain loads/stores.
 */
#ifndef A3_ATOMIC_H
#define A3_ATOMIC_H

#include "a3_base.h"

typedef struct A3AtomicU32 { volatile u32 v; } A3AtomicU32;
typedef struct A3AtomicU64 { volatile u64 v; } A3AtomicU64;
typedef struct A3Spinlock { volatile u32 locked; } A3Spinlock;

A3_INLINE u32 a3_atomic_load_u32(const A3AtomicU32 *a) { return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE); }
A3_INLINE void a3_atomic_store_u32(A3AtomicU32 *a, u32 x) { __atomic_store_n(&a->v, x, __ATOMIC_RELEASE); }
A3_INLINE u32 a3_atomic_add_u32(A3AtomicU32 *a, u32 x) { return __atomic_add_fetch(&a->v, x, __ATOMIC_ACQ_REL); }
A3_INLINE u32 a3_atomic_sub_u32(A3AtomicU32 *a, u32 x) { return __atomic_sub_fetch(&a->v, x, __ATOMIC_ACQ_REL); }
A3_INLINE b32 a3_atomic_cas_u32(A3AtomicU32 *a, u32 expected, u32 desired) {
    return __atomic_compare_exchange_n(&a->v, &expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
A3_INLINE u64 a3_atomic_load_u64(const A3AtomicU64 *a) { return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE); }
A3_INLINE void a3_atomic_store_u64(A3AtomicU64 *a, u64 x) { __atomic_store_n(&a->v, x, __ATOMIC_RELEASE); }
A3_INLINE u64 a3_atomic_add_u64(A3AtomicU64 *a, u64 x) { return __atomic_add_fetch(&a->v, x, __ATOMIC_ACQ_REL); }
A3_INLINE u64 a3_atomic_sub_u64(A3AtomicU64 *a, u64 x) { return __atomic_sub_fetch(&a->v, x, __ATOMIC_ACQ_REL); }

A3_INLINE void a3_cpu_relax(void) {
#if defined(A3_ARCH_X64)
    __builtin_ia32_pause();
#endif
}

A3_INLINE void a3_spin_lock(A3Spinlock *l) {
    for (;;) {
        if (!__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) return;
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) a3_cpu_relax();
    }
}
A3_INLINE b32 a3_spin_trylock(A3Spinlock *l) { return !__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE); }
A3_INLINE void a3_spin_unlock(A3Spinlock *l) { __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE); }

#endif
