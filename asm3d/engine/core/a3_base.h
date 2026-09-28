/*
 * ASM3D - a3_base.h
 * Foundation header: fixed-width types, platform/arch detection, core macros.
 *
 * This header must compile in three environments:
 *   - hosted native (Linux/macOS/Windows) with a C11 compiler
 *   - freestanding WebAssembly (clang --target=wasm32 -ffreestanding -nostdlib)
 *   - C++ translation units (editor plugins may be C++)
 * It therefore only includes the freestanding standard headers.
 */
#ifndef A3_BASE_H
#define A3_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
#define A3_EXTERN_C_BEGIN extern "C" {
#define A3_EXTERN_C_END }
#else
#define A3_EXTERN_C_BEGIN
#define A3_EXTERN_C_END
#endif

/* ---- Platform detection ------------------------------------------------ */
#if defined(__wasm__) || defined(__wasm32__)
#  define A3_PLATFORM_WEB 1
#  define A3_ARCH_WASM32 1
#elif defined(_WIN32)
#  define A3_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#  define A3_PLATFORM_MACOS 1
#  define A3_PLATFORM_POSIX 1
#elif defined(__linux__)
#  define A3_PLATFORM_LINUX 1
#  define A3_PLATFORM_POSIX 1
#else
#  error "ASM3D: unsupported platform"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  define A3_ARCH_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define A3_ARCH_ARM64 1
#endif

#ifndef A3_PLATFORM_WEB
#  define A3_PLATFORM_WEB 0
#endif
#ifndef A3_PLATFORM_WINDOWS
#  define A3_PLATFORM_WINDOWS 0
#endif
#ifndef A3_PLATFORM_MACOS
#  define A3_PLATFORM_MACOS 0
#endif
#ifndef A3_PLATFORM_LINUX
#  define A3_PLATFORM_LINUX 0
#endif
#ifndef A3_PLATFORM_POSIX
#  define A3_PLATFORM_POSIX 0
#endif

#define A3_PLATFORM_NATIVE (!A3_PLATFORM_WEB)

/* Assembly fast paths are used when the build enables them and the arch has an
 * implementation. Every assembly routine has a portable C reference that is
 * tested against it (see tests/test_simd.c). */
#if defined(A3_USE_ASM) && A3_USE_ASM && defined(A3_ARCH_X64) && !A3_PLATFORM_WINDOWS
#  define A3_HAS_X64_ASM 1
#else
#  define A3_HAS_X64_ASM 0
#endif

#if A3_PLATFORM_WEB && defined(__wasm_simd128__)
#  define A3_HAS_WASM_SIMD 1
#else
#  define A3_HAS_WASM_SIMD 0
#endif

/* ---- Types ------------------------------------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;
typedef size_t   usize;
typedef ptrdiff_t isize;
typedef uintptr_t uptr;
typedef int32_t  b32;

/* ---- Compiler helpers -------------------------------------------------- */
#if defined(_MSC_VER)
#  define A3_INLINE static __forceinline
#  define A3_NOINLINE __declspec(noinline)
#  define A3_ALIGNAS(n) __declspec(align(n))
#  define A3_LIKELY(x) (x)
#  define A3_UNLIKELY(x) (x)
#  define A3_DEBUG_BREAK() __debugbreak()
#  define A3_PRINTF_LIKE(fmt_idx, args_idx)
#else
#  define A3_INLINE static inline __attribute__((always_inline))
#  define A3_NOINLINE __attribute__((noinline))
#  define A3_ALIGNAS(n) __attribute__((aligned(n)))
#  define A3_LIKELY(x) __builtin_expect(!!(x), 1)
#  define A3_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  if A3_PLATFORM_WEB
#    define A3_DEBUG_BREAK() __builtin_trap()
#  else
#    define A3_DEBUG_BREAK() __builtin_trap()
#  endif
#  define A3_PRINTF_LIKE(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#endif

#if A3_PLATFORM_WEB
#  define A3_WASM_EXPORT(name) __attribute__((export_name(name)))
#  define A3_WASM_IMPORT(mod, name) __attribute__((import_module(mod), import_name(name)))
#else
#  define A3_WASM_EXPORT(name)
#  define A3_WASM_IMPORT(mod, name)
#endif

#define A3_UNUSED(x) ((void)(x))
#define A3_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define A3_KB(x) ((usize)(x) << 10)
#define A3_MB(x) ((usize)(x) << 20)
#define A3_GB(x) ((u64)(x) << 30)
#define A3_MIN(a, b) ((a) < (b) ? (a) : (b))
#define A3_MAX(a, b) ((a) > (b) ? (a) : (b))
#define A3_CLAMP(x, lo, hi) (A3_MIN(A3_MAX((x), (lo)), (hi)))
#define A3_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define A3_IS_POW2(x) ((x) != 0 && (((x) & ((x) - 1)) == 0))
#define A3_OFFSETOF(type, member) offsetof(type, member)
#define A3_STRINGIFY_(x) #x
#define A3_STRINGIFY(x) A3_STRINGIFY_(x)
#define A3_CONCAT_(a, b) a##b
#define A3_CONCAT(a, b) A3_CONCAT_(a, b)

#ifdef __cplusplus
#  define A3_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#  define A3_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

A3_STATIC_ASSERT(sizeof(u8) == 1, "u8");
A3_STATIC_ASSERT(sizeof(u16) == 2, "u16");
A3_STATIC_ASSERT(sizeof(u32) == 4, "u32");
A3_STATIC_ASSERT(sizeof(u64) == 8, "u64");
A3_STATIC_ASSERT(sizeof(f32) == 4, "f32");

/* ---- Engine version ---------------------------------------------------- */
#define A3_VERSION_MAJOR 0
#define A3_VERSION_MINOR 1
#define A3_VERSION_PATCH 0
#define A3_VERSION_STRING "0.1.0"
/* Bumped whenever a serialized format changes; see engine/scene/a3_migrate.c */
#define A3_FORMAT_VERSION 1

/* Result codes used across engine APIs. Never crash on recoverable errors:
 * return a code and log the reason. */
typedef enum A3Result {
    A3_OK = 0,
    A3_ERR_UNKNOWN,
    A3_ERR_OUT_OF_MEMORY,
    A3_ERR_INVALID_ARG,
    A3_ERR_NOT_FOUND,
    A3_ERR_IO,
    A3_ERR_PARSE,
    A3_ERR_UNSUPPORTED,
    A3_ERR_VERSION,
    A3_ERR_FULL,
    A3_ERR_BUSY,
    A3_ERR_COMPILE,
} A3Result;

A3_EXTERN_C_BEGIN
const char *a3_result_str(A3Result r);
A3_EXTERN_C_END

#endif /* A3_BASE_H */
