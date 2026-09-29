# ASM3D Status

This file says exactly what works today. If a feature is not listed under
**Working**, it does not work yet. Last updated with the city, vehicles and Neon Tide milestone.

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
| WebAssembly (engine) | Working | Compiles with clang/wasm-ld (no emscripten, no libc). The full test suite passes in Node. |
| Browser editor (WebAssembly + WebGL2) | Working | The real editor in a browser tab: `build/web/`, see docs/WEB_EDITOR.md. Projects are stored in the browser and can be imported/exported as zip. Its self test passes in headless Chromium (67 checks). Not yet: Build Game, sound, threads, wireframe. |
| Exporting games to the web (HTML player) | Planned | The browser editor runs games in play mode; a standalone web player export is not built yet. |
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
| Renderer: PBR forward, sun shadows, sky, instancing, SIMD culling | Working | OpenGL 3.3 backend, plus a null backend for tests. |
| Local lights | Working | Up to 1024 point/spot lights per frame in a light texture; each object is shaded by its 8 nearest (picked on the CPU with a linear scan: fine for a city's ~800 lamps, a spatial grid is planned). |
| Post-processing: SSAO, screen-space reflections, exponential height fog with sun glow, 5-level bloom, exposure / contrast / saturation / tint, ACES, vignette, FXAA | Working | All screen-space on OpenGL 3.3. This is not hardware ray tracing: reflections only show what is on screen, and fall back to the sky gradient. |
| Builtin procedural materials (`builtin:building`, `artdeco`, `tower`, `road`, `sidewalk`, `sand`, `water`, `glass`, `neon`, `carpaint`, `palm_trunk`, `foliage`, `metal`) | Working | Computed from world position (no textures). Windows and neon light up automatically at night. |
| Custom materials (`.a3shader`) | Working | Made with the Shader Maker, loaded by the renderer in the editor and in games. |
| Physics: rigid bodies, box/sphere/capsule/mesh colliders, SAT contacts, warm starting, sleeping, triggers, raycasts | Working | Assembly kernels for integration, AABBs, sweep-and-prune, the solver and raycasts. Character controllers are detected by trigger colliders (a sensor capsule that never collides or blocks rays). |
| Character controller (walk, run, jump, stairs, slopes, 1st/3rd person camera) | Working | |
| Audio: mixer, WAV, 3D sound, AudioSource/AudioListener | Working | SSE assembly mixing kernels (bit-exact C reference). Output via WASAPI on Windows and ALSA on Linux; silent if there is no device. 12 synthesized built-in sounds, plus automatic footsteps and jump sounds for the character. OGG/MP3 decoding is planned. |
| Animation: keyframe clips, Animator, Motion, timeline editor | Working | Tracks animate any reflected field (transform, light, colors, custom components) with smooth, linear or step keys. |
| Skeletal animation (bones, skinning, blending) | Planned | Needs glTF import. |
| Particles: emitters, 10 presets, instanced billboards | Working | SSE assembly integration kernel (bit-exact C reference). Additive and alpha-sorted blending; previewed live in the editor. GPU simulation and collision are planned. |
| Modeling library (editable polygon meshes) | Working | x86-64 SSE kernels (masked transform, triangle normals, ray picking, bounds) with bit-exact C references; operations listed in docs/MODELING.md. |
| Scripting (A3Script) | Working | Bytecode compiler + stack VM (C). Script component with on_start / on_update / on_fixed_update / on_trigger_enter / on_trigger_exit / on_collision; any component field readable and writable by name; 97 built-in functions (math, vectors, lists, text, objects, input, physics, sound, particles, animation, HUD, scenes, saved values); hot reload that keeps variables; instruction budget against endless loops; plain-language errors with line numbers and "did you mean" suggestions. See docs/SCRIPTING.md. Not yet: dictionaries/maps, closures, classes, a debugger with breakpoints. |
| Script HUD (text, rectangles, bars) | Working | Drawn over the game window and the editor viewport on a 1280 x 720 canvas. Text is a scaled bitmap font (large sizes look soft). |
| Visual scripting | Planned | |
| Procedural city (Sol Harbor) | Working | `engine/world/a3_citygen.c`: a fictional coastal city whose layout is loosely inspired by Miami (downtown towers by a bay, causeways to a barrier island with an Art Deco beachfront, a port). About 6,000 objects and 800 street lights, generated in ~0.1 s from a seed, with day / sunset / night looks and a road graph. Made of boxes and procedural meshes; it is not a map of the real city. See docs/CITY.md. |
| Procedural meshes (`builtin:palm_crown`, `car_body`, `car_glass`, `wheel`) | Working | Built with the modeling library; any code can register more generators. |
| Terrain, world streaming, LOD | Planned | The city is small enough to load whole; there is no streaming or LOD yet. |
| Vehicles (arcade car model) | Working | Vehicle component: throttle/brake/reverse, steering with a cornering limit, handbrake drift, ground rays, wall collisions with impact and damage, chase camera. Not a tire/suspension simulation; cars do not roll over. |
| Traffic (AI cars and pedestrians on a road graph) | Working | Cars drive on the right, turn at intersections, slow for corners and keep their distance; jams resolve by timeout. Not yet: traffic lights, lane changes, pedestrians reacting to cars, navigation meshes. |
| Water | Partial | An animated reflective water material on a plane. No waves that move geometry, no swimming or buoyancy. |
| Weather | Planned | Time of day presets only (day, sunset, night). |
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
| Edit Mode (polygon modeling) | Working | Vertex/edge/face selection, box select, edge loops, G/R/S with axis locks and snapping, extrude, inset, loop cut, Catmull-Clark, delete, fill, merge, duplicate, mirror, normals, per-mesh undo. See docs/MODELING.md. Not yet: bevel, knife, booleans, UV unwrapping, sculpting. |
| Visual script editor, terrain tools | Planned | |

## Command line (`asm3d_cli`)

| Feature | Status | Notes |
|---|---|---|
| JSON output for every command, batch mode (JSON Lines) | Working | See docs/CLI.md. |
| Projects, scenes, objects, components (create, inspect, edit) | Working | Objects by name, path or GUID; "did you mean" hints. |
| Script check / run / eval | Working | |
| Headless simulation with scripted key input, traces and HUD text | Working | No window or GPU needed (`a3_engine_create_headless`). |
| Validate and build | Working | Shared with the editor (`engine/runtime/a3_project.c`); validation also compiles every script. |
| Screenshots and frame recording (`screenshot --record`) | Working | Needs OpenGL 3.3 (a desktop session, or Xvfb on Linux servers). |
| City generation (`world city`) | Working | Writes a city scene, its road graph and a Traffic object into a project. |
| Project templates from the command line | Planned | `project new` makes the starter scene only; templates live in the editor. |

## Games

| Game | Status | Notes |
|---|---|---|
| Neon Tide (`games/NeonTide`) | Working (small) | An open-world courier game in Sol Harbor at night: walk, take any car (E), timed delivery jobs, cash and streaks saved between runs, a wanted level with police pursuit, busted / evade rules, a radar, a speedometer. Written in A3Script (`Assets/Scripts/Game.a3script`). Not yet: weapons, story missions, character animation (people are capsules), interiors, audio beyond built-in sounds. |
| Neon Tide trailer | Working | `tools/make_trailer.sh` records the Trailer scene (scripted camera: `Director.a3script`) frame by frame with `asm3d_cli screenshot --record` and encodes an MP4 with ffmpeg; the soundtrack is synthesized by `tools/trailer_music.py`. ffmpeg is an offline tool, not part of the engine. |
| Platformer template | Working | Collect 12 orbs, with a HUD counter, a timer and a best time saved between runs. The other templates are playable starting points, not finished games. |

## How this is verified

- `asm3d_tests`: 79 tests (unit, determinism golden hashes, physics
  scenarios, shader graph, audio, particles, animation, the scripting
  language and its engine bindings, the modeling kernels and operations,
  the city generator and road graph, driving and crashing a car, traffic). They run natively and as WebAssembly
  (`node tools/run_wasm_tests.mjs build/asm3d_tests.wasm`).
- `tools/test_cli.py`: 79 end-to-end checks of `asm3d_cli` (Linux and
  Windows builds).
- `asm3d_editor --selftest`: drives the real editor end to end. It creates a
  project, runs undo/redo, play/stop, save/reopen and a build, compiles every
  Shader Maker preset on the GPU, checks that shader errors are mapped to
  the right node, runs a script in play mode (including hot reload and error
  markers), models a tower in Edit Mode and collects an orb in the scripted
  Platformer template (70 checks).
- `node tools/test_web_editor.mjs build/web --selftest`: the editor self
  test inside headless Chromium with WebGL2 (67 checks).
- `asm3d_player --frames N --screenshot out.png` and
  `asm3d_editor --frames N --screenshot out.png` are used for visual checks.
