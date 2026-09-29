# ASM3D Status

This file says exactly what works today. If a feature is not listed under
**Working**, it does not work yet. Last updated with the editor + Shader Maker
milestone.

Legend: **Working** = implemented, used by the editor or games, and covered by
tests or the editor self test. **Partial** = usable but with the limits
listed. **Planned** = not implemented yet.

## Platforms

| Area | Status | Notes |
|---|---|---|
| Linux x86-64 desktop (X11 + OpenGL 3.3) | Working | The main target: the editor, the player and built games. |
| x86-64 SSE assembly kernels | Working | SIMD math, frustum culling and physics. Each kernel has a C reference and tests prove the results are bit-identical. |
| Other CPUs (C reference path) | Working in tests | The same code without assembly, used for WebAssembly. |
| WebAssembly (engine core + physics) | Partial | Compiles with clang/wasm-ld. The full test suite passes in Node. There is no browser renderer or HTML exporter: ASM3D is desktop-first. |
| Windows, macOS | Planned | Platform and window layers are isolated (`a3_platform.h`, `a3_window.h`) so they can be added. |

## Engine

| System | Status | Notes |
|---|---|---|
| Memory (heap, arenas, pools, frame, scratch, tagged stats, double-free detection) | Working | |
| Math, deterministic transcendental functions, noise, RNG | Working | Golden-hash tests pass on both native and wasm. |
| Logging with plain-language hints, JSON, strings, formatting | Working | |
| Images: PNG/TGA/BMP/PPM decode, PNG encode | Working | |
| Job system | Working | Native only; web falls back to a single thread. |
| ECS: generational handles, GUIDs, sparse sets, hierarchy, queries, world clone | Working | |
| Reflection-driven components, custom data-only components | Working | Custom components are defined in the editor and saved as `.a3comp`. |
| Scenes (`.a3scene` JSON, GUID references, unknown-component preservation, version check) | Working | |
| Prefabs | Partial | Engine API (`a3_entities_save_json` / `load`) only. No editor workflow yet. |
| Input actions (keyboard/mouse) | Working | Gamepad is planned. |
| Assets: OBJ meshes, textures, placeholders on failure, hot reload API | Working | glTF/FBX import is planned. |
| Renderer: PBR forward, sun shadows, 16 local lights, sky, fog, ACES, FXAA, instancing, SIMD culling | Working | OpenGL 3.3 backend, plus a null backend for tests. |
| Custom materials (`.a3shader`) | Working | Made with the Shader Maker, loaded by the renderer in the editor and in games. |
| Physics: rigid bodies, box/sphere/capsule/mesh colliders, SAT contacts, warm starting, sleeping, triggers, raycasts | Working | Assembly kernels for integration, AABBs, sweep-and-prune, the solver and raycasts. |
| Character controller (walk, run, jump, stairs, slopes, 1st/3rd person camera) | Working | |
| Audio | Planned | |
| Animation (skeletal, blending) | Planned | |
| Particles | Planned | |
| Scripting language runtime | Planned | The code editor can already write and save `.a3script` files, but they are **not executed**. |
| Visual scripting | Planned | |
| Terrain, world streaming, LOD, procedural generation | Planned | The Open World template uses plain props. |
| AI navigation, vehicles, weather, water | Planned | The Racing template's car is a plain rigid body. |
| Plugins (runtime-loaded modules) | Planned | Components from missing plugins are already preserved in scenes. |

## Editor (`asm3d_editor`)

| Feature | Status | Notes |
|---|---|---|
| Start screen, 10 playable templates, recent projects | Working | |
| Docking panels, 5 layout presets, per-project layout | Working | |
| Beginner / Advanced modes, contextual tips, tooltips on every setting | Working | |
| Hierarchy: search, drag to re-parent, context menus | Working | |
| Inspector generated from reflection, add/remove components | Working | |
| Viewport: fly/orbit/pan, picking, move/rotate/scale gizmos, snapping, overlays | Working | |
| Undo/redo for every change, autosave every 30 s, crash recovery | Working | |
| Play / Pause / Step in the editor (on a copy of the scene) | Working | |
| Asset browser: thumbnails, drag and drop, move to trash | Working | |
| Console, profiler (CPU, systems, physics, memory), docs, settings | Working | GPU timings are planned. |
| Code editor: highlighting, find/replace, undo, JSON error markers | Working | |
| Command palette (Ctrl+P) over commands, objects, assets and components | Working | |
| Shader Maker: node graph, live preview, parameters, errors shown on nodes | Working | |
| One-click desktop build (Linux) with project checks | Working | Debug/Release only change the folder name for now. |
| Visual script editor, animation editor, terrain tools | Planned | |

## Games

Two complete example games are **planned**. The 10 templates are playable
starting points, not finished games.

## How this is verified

- `asm3d_tests`: 52 tests (unit, determinism golden hashes, physics
  scenarios, shader graph). They run natively and as WebAssembly
  (`node tools/run_wasm_tests.mjs build/asm3d_tests.wasm`).
- `asm3d_editor --selftest`: drives the real editor end to end. It creates a
  project, runs undo/redo, play/stop, save/reopen and a build, compiles every
  Shader Maker preset on the GPU, and checks that shader errors are mapped to
  the right node.
- `asm3d_player --frames N --screenshot out.png` and
  `asm3d_editor --frames N --screenshot out.png` are used for visual checks.
