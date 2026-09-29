# ASM3D

ASM3D is a desktop game engine and editor written in C11. The hot paths are
x86-64 assembly: SIMD math, frustum culling, and the physics integrator,
broadphase, solver and raycasts. The engine and editor have no third-party
code or libraries (only X11 and OpenGL from the system).

See [docs/STATUS.md](docs/STATUS.md) for exactly what works today.

## Build (Linux)

Requirements: CMake 3.16+, a C11 compiler (gcc or clang), Ninja or Make, the X11
development headers and an OpenGL 3.3 driver.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

```sh
./build/asm3d_editor                   # editor (start screen)
./build/asm3d_editor --project MyGame  # open a project
./build/asm3d_player --demo            # renderer demo scene
```

In the editor, pick a template, press **F5** to play, and use
**Build > Build Game** to package it. The result is
`MyGame/Builds/Desktop-Debug/MyGame/`, a folder holding the game program and a
`data/` folder. Zip that folder to share the game.

## Test

```sh
./build/asm3d_tests                                   # native (assembly kernels)
node tools/run_wasm_tests.mjs build/asm3d_tests.wasm  # WebAssembly (C kernels)
./build/asm3d_editor --selftest                       # editor end to end (needs a display)
```

## Layout

```
engine/core      memory, math, SIMD (asm + C), strings, JSON, images, hashing
engine/platform  POSIX / web platform layers, X11 window
engine/ecs       entities, components, reflection
engine/scene     built-in components, scene files
engine/physics   rigid bodies, colliders, character controller, x86-64 kernels
engine/render    RHI (OpenGL / null), PBR renderer, shader graph compiler
engine/ui        immediate-mode UI toolkit, docking
engine/runtime   engine loop, systems, modules
apps/editor      the editor
apps/player      the game player (built games are this program + data/)
tests            unit, determinism and scenario tests
```
