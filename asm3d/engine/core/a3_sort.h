/*
 * ASM3D - a3_sort.h
 * Sorting utilities used by the renderer (draw keys), physics and UI.
 */
#ifndef A3_SORT_H
#define A3_SORT_H

#include "a3_base.h"

A3_EXTERN_C_BEGIN

typedef struct A3SortPair { u64 key; u32 value; u32 pad; } A3SortPair;

/* Stable LSD radix sort by key (ascending). `tmp` must hold `count` pairs.
 * Passes whose byte is identical for all keys are skipped. */
void a3_radix_sort_pairs(A3SortPair *pairs, A3SortPair *tmp, u32 count);

/* Generic in-place sort (insertion sort below 24 elements, otherwise merge
 * sort with a heap scratch buffer). cmp returns <0, 0, >0. Stable. */
typedef int (*A3CompareFn)(const void *a, const void *b, void *user);
void a3_sort(void *base, u32 count, usize elem_size, A3CompareFn cmp, void *user);

A3_EXTERN_C_END

#endif
