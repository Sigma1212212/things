/*
 * ASM3D - a3_audio_null.c
 * Platforms without an output backend (web build, macOS for now): silent.
 */
#include "a3_audio_backend.h"

#if !A3_PLATFORM_WINDOWS && !A3_PLATFORM_LINUX
b32 a3_audio_backend_open(u32 *rate, A3AudioRenderFn render, char *name, usize name_cap) {
    A3_UNUSED(rate); A3_UNUSED(render); A3_UNUSED(name); A3_UNUSED(name_cap);
    return 0;
}
void a3_audio_backend_close(void) {}
#endif
