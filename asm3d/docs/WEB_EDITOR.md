# The editor in a browser

`build/web/` holds the ASM3D editor compiled to WebAssembly. It is the same
editor code as the desktop program (`apps/editor`), with three pieces
swapped for the browser:

| Desktop | Browser |
|---|---|
| X11 / Win32 window, OpenGL 3.3 | `engine/platform/window_web.c`: a `<canvas>` with WebGL2. Every OpenGL function the engine loads (the list in `engine/render/a3_gl.h`) becomes a WebAssembly import implemented on WebGL2 in `web/asm3d.js`, so `rhi_gl.c` and the renderer run unchanged. Shaders are compiled as GLSL ES 3.00. |
| Files on disk | `web/a3fs.js`: a file tree in JavaScript. `/user` is saved in the browser (IndexedDB); new projects go to `/user/projects`. |
| Threads | Jobs run on one thread. |
| WASAPI / ALSA sound | `engine/audio/a3_audio_web.c`: WebAudio pulls the engine's mixer on the page's thread. Browsers start sound after the first click or key press. |

## Run it

```sh
cmake -S . -B build -G Ninja && cmake --build build   # needs clang + wasm-ld for the web target
build/asm3d_cli serve build/web --open                # http://localhost:8080 (any static server works)
```

The Windows zip contains the built page in `browser-editor/`: run
`asm3d_cli.exe serve browser-editor --open`.

It needs a browser with WebGL2 (current Chrome, Edge, Firefox or Safari).
Pages opened from `file://` cannot load the `.wasm` file; use a web server.

The toolbar above the editor:

- **Open Neon Tide sample** copies the example game into your browser
  projects and opens it (`?sample=1` in the URL does the same).
- **Open .zip / Open folder** imports a project from your disk.
- **Download .zip** saves a project from the browser to your disk.
- **Reset storage** deletes every project stored in this browser.

URL options: `?project=/user/projects/Name` opens a project,
`?args=...` passes editor command-line options (for example
`?args=--selftest`), `?persist=0` keeps nothing between visits.

## What works

Everything in the editor except building desktop executables: projects and
templates, the hierarchy, inspector, viewport and gizmos, undo/redo, play
mode (physics, scripts, traffic, the Neon Tide city), the code editor with
live script errors, the Shader Maker (materials compile on WebGL2), the
animation timeline and Edit Mode modeling.

Not yet: **Build Game** (it needs a compiler toolchain and writes
executables; download the project and build it with the desktop editor),
multithreading, file watching (files changed outside the editor are not
reloaded), wireframe drawing (WebGL has no polygon mode) and GPU timings.
Sound uses the older ScriptProcessor API (browsers print a deprecation
note); an AudioWorklet version is planned.

## How it is tested

`node tools/test_web_editor.mjs build/web --selftest` serves the folder,
opens it in headless Chromium (software WebGL2 through SwiftShader) with
Playwright, and runs the editor's self test inside the page: the same
checks as `asm3d_editor --selftest`, minus the three that build a desktop
executable (67 checks). `--interactive` clicks and types like a user:
it creates a project from a template card, starts play mode with F5 and
reloads the page to check that the project was saved in IndexedDB.
`--shot out.png --query sample=1 --args "--play-at 3"` plays Neon Tide in the
browser and saves a screenshot.
