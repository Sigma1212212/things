# ASM3D Status

This file says exactly what works today. If a feature is not listed under
**Working**, it does not work yet. Last updated with the scripting milestone.

Legend: **Working** = implemented, used by the editor or games, and covered by
tests or the editor self test. **Partial** = usable but with the limits
listed. **Planned** = not implemented yet.

## Platforms

| Area | Status | Notes |
|---|---|---|
| Windows 10/11 x64 (Win32 + OpenGL 3.3) | Working | Editor, player and built games. Cross-compiled with MinGW-w64. The test suite and the editor self test pass on Windows (checked under Wine 9 here). The UI is not DPI-aware yet: on high-DPI screens Windows scales it up. |
| Linux x86-64 desktop (X11 + OpenGL 3.3) | Working | Editor, player and built games. |
| x86-64 SSE assembly kernels | Working | SIMD math, frustum culling and physics, with the same kernel bodies on Windows and Linux (Windows goes through small ABI entry stubs). Each kernel has a C reference, and tests prove bit-identical results on both operating systems. |
| Other CPUs (C reference path) | Working in tests | The same code without assembly, used for WebAssembly. |
| WebAssembly (engine core + physics) | Partial | Compiles with clang/wasm-ld. The full test suite passes in Node. There is no browser renderer or HTML exporter: ASM3D is desktop-first. |
| macOS | Planned | The platform and window layers are isolated (`a3_platform.h`, `a3_window.h`). |

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
| Physics: rigid bodies, box/sphere/capsule/mesh colliders, SAT contacts, warm starting, sleeping, triggers, raycasts | Working | Assembly kernels for integration, AABBs, sweep-and-prune, the solver and raycasts. Character controllers are detected by trigger colliders (a sensor capsule that never collides or blocks rays). |
| Character controller (walk, run, jump, stairs, slopes, 1st/3rd person camera) | Working | |
| Audio: mixer, WAV, 3D sound, AudioSource/AudioListener | Working | SSE assembly mixing kernels (bit-exact C reference). Output via WASAPI on Windows and ALSA on Linux; silent if there is no device. 12 synthesized built-in sounds, plus automatic footsteps and jump sounds for the character. OGG/MP3 decoding is planned. |
| Animation: keyframe clips, Animator, Motion, timeline editor | Working | Tracks animate any reflected field (transform, light, colors, custom components) with smooth, linear or step keys. |
| Skeletal animation (bones, skinning, blending) | Planned | Needs glTF import. |
| Particles: emitters, 10 presets, instanced billboards | Working | SSE assembly integration kernel (bit-exact C reference). Additive and alpha-sorted blending; previewed live in the editor. GPU simulation and collision are planned. |
| Scripting (A3Script) | Working | Bytecode compiler + stack VM (C). Script component with on_start / on_update / on_fixed_update / on_trigger_enter / on_trigger_exit / on_collision; any component field readable and writable by name; 97 built-in functions (math, vectors, lists, text, objects, input, physics, sound, particles, animation, HUD, scenes, saved values); hot reload that keeps variables; instruction budget against endless loops; plain-language errors with line numbers and "did you mean" suggestions. See docs/SCRIPTING.md. Not yet: dictionaries/maps, closures, classes, a debugger with breakpoints. |
| Script HUD (text, rectangles, bars) | Working | Drawn over the game window and the editor viewport on a 1280 x 720 canvas. Text is a scaled bitmap font (large sizes look soft). |
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
| Code editor: highlighting, find/replace, undo, error markers | Working | Scripts are checked while typing (marker + explanation on the line); saving reloads running objects. JSON files are checked on save. |
| Script component tools | Working | "Create New Script" / "Edit Script" in the Inspector, the script's latest error shown in red, Script API reference in Docs (generated from the registered functions). |
| Command palette (Ctrl+P) over commands, objects, assets and components | Working | |
| Shader Maker: node graph, live preview, parameters, errors shown on nodes | Working | |
| One-click desktop build (Windows or Linux, the OS the editor runs on) with project checks | Working | Debug/Release only change the folder name for now. |
| Animation timeline panel | Working | Key, scrub, preview (the object is restored afterwards), drag keys, set interpolation. |
| Visual script editor, terrain tools | Planned | |

## Command line (`asm3d_cli`)

| Feature | Status | Notes |
|---|---|---|
| JSON output for every command, batch mode (JSON Lines) | Working | See docs/CLI.md. |
| Projects, scenes, objects, components (create, inspect, edit) | Working | Objects by name, path or GUID; "did you mean" hints. |
| Script check / run / eval | Working | |
| Headless simulation with scripted key input, traces and HUD text | Working | No window or GPU needed (`a3_engine_create_headless`). |
| Validate and build | Working | Shared with the editor (`engine/runtime/a3_project.c`); validation also compiles every script. |
| Screenshots | Working | Needs OpenGL 3.3 (a desktop session, or Xvfb on Linux servers). |
| Project templates from the command line | Planned | `project new` makes the starter scene only; templates live in the editor. |

## Games

Two complete example games are **planned**. The 10 templates are playable
starting points, not finished games. The Platformer template is scripted:
collect 12 orbs, with a HUD counter, a timer and a best time saved between runs.

## How this is verified

- `asm3d_tests`: 70 tests (unit, determinism golden hashes, physics
  scenarios, shader graph, audio, particles, animation, the scripting
  language and its engine bindings). They run natively and as WebAssembly
  (`node tools/run_wasm_tests.mjs build/asm3d_tests.wasm`).
- `tools/test_cli.py`: 64 end-to-end checks of `asm3d_cli` (Linux and
  Windows builds).
- `asm3d_editor --selftest`: drives the real editor end to end. It creates a
  project, runs undo/redo, play/stop, save/reopen and a build, compiles every
  Shader Maker preset on the GPU, checks that shader errors are mapped to
  the right node, runs a script in play mode (including hot reload and error
  markers) and collects an orb in the scripted Platformer template (56 checks).
- `asm3d_player --frames N --screenshot out.png` and
  `asm3d_editor --frames N --screenshot out.png` are used for visual checks.
