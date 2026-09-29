/*
 * ASM3D - a3_modules.c
 * Registers the components and systems of every engine module compiled into
 * this build. The web/desktop exporters generate a trimmed version of this
 * list when a game does not use a module (build-time stripping).
 */
#include "a3_engine.h"

void a3_modules_register_all(void) {
    /* Modules add their registration calls here as they are implemented:
     * physics, audio, animation, particles, scripting, AI, vehicles, ... */
}
