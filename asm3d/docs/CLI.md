# asm3d_cli

`asm3d_cli` drives the engine from a terminal, a CI job or an AI assistant.
It needs no editor and (except `screenshot`) no window or GPU.

Every command prints **one JSON object** on stdout and exits with 0 (success)
or 1 (failure):

```json
{"ok": true,  "command": "entity set", "result": {...}, "log": []}
{"ok": false, "command": "entity set", "error": "no object called 'Bal' in the scene",
 "hint": "Did you mean 'Ball'?", "log": []}
```

`log` holds engine warnings and errors (and script `print` output for
`script run`). Add `--pretty` for indented JSON, `--verbose` to mirror the
engine log on stderr, and `--dry-run` to make edit commands leave files
untouched. `asm3d_cli help` returns this list as JSON.

## Commands

| Command | What it does |
|---|---|
| `help`, `version` | Commands and usage; engine version, platform, assembly kernel backend |
| `components [Name] [--fields]` | Component types; with a name, every field with type, range, options and description |
| `api [--category Math]` | Every A3Script function and event |
| `project new <folder> [--name N]` | Project with a starter scene (sun, ground, camera) |
| `project info <folder> [--all]` | Settings, scenes, scripts, shaders, animations, models |
| `validate <project> [--scene S]` | Scene checks, missing files, compiles every script (issues carry file and line) |
| `build <project> [--out dir] [--config Release]` | Playable desktop build (refused when validation finds errors) |
| `scene new <file> [--empty]`, `scene dump <file>` | Create a scene; every object with all component values |
| `entity list <scene>` | Hierarchy order with depth and component names |
| `entity get <scene> <object> [Component[.field]]` | An object, a component or one value |
| `entity add <scene> <name> [--parent P] [--at x,y,z] [--rotation x,y,z] [--scale x,y,z] [--primitive cube] [--model m.obj] [--script s.a3script] [--with A,B]` | Create an object |
| `entity set <scene> <object> <Component.field> <value> [...]` | Set values (adds missing components); `active false` disables the object |
| `entity remove / rename / duplicate` | Delete, rename, copy (`--name`, `--at`) |
| `component add / remove <scene> <object> <Component>` | Add or remove a component |
| `script check <files...>` | Compile; errors with line, column and a suggestion |
| `script run <file> [--call fn] [--args '[1,"a",[0,1,0]]']` | Run outside a scene: print output, return value, top-level variables |
| `script eval "<expression>"` | Evaluate one expression |
| `simulate <project> [--scene S] [--frames 120] [--dt 0.0166] [--keys space@10-20,w@0-60] [--watch A,B] [--trace N]` | Play headless; object states, script errors, HUD text |
| `screenshot <project> [--frames 30] [--size 1280x720] [--camera x,y,z --look x,y,z] [--out file.png]` | Render to a PNG (needs OpenGL; on a Linux server run Xvfb). Only the last frames are drawn, so long runs are fast |
| `screenshot <project> --record <dir> [--record-from N] [--dt 0.0333]` | Save every frame from N on (game camera + HUD) as `frame_00000.png`...: turn them into a video with any encoder (see tools/make_trailer.sh) |
| `world city <project> [--seed 1] [--time day\|sunset\|night] [--density 1] [--cars 40] [--pedestrians 60] [--no-traffic] [--no-lights] [--no-neon] [--scene S] [--startup] [--all]` | Generate the Sol Harbor city scene and its road graph (docs/CITY.md) |
| `serve [folder] [--port 8080] [--open]` | Serve the browser editor (`build/web`) on localhost until Ctrl+C; prints one JSON line when listening |
| `mesh ...` | Modeling operations (see docs/MODELING.md) |
| `batch` | Read commands from stdin, one per line; print one JSON line each |

Objects are found by name, by `Parent/Child` path or by 16-digit GUID.
Values: `5`, `true`, `1,2,3` or `[1,2,3]` (vectors, colors), plain text,
option names (`Light.type Spot`); rotations are degrees.

## Example

```sh
asm3d_cli project new MyGame
asm3d_cli entity add MyGame/Assets/Scenes/Main.a3scene Crate --at 0,3,0 --primitive cube --with Collider,RigidBody
asm3d_cli entity set MyGame/Assets/Scenes/Main.a3scene Crate RigidBody.mass 5 MeshRenderer.base_color 1,0.5,0,1
asm3d_cli simulate MyGame --frames 120 --watch Crate
asm3d_cli build MyGame
```

Tested by `tools/test_cli.py` (89 checks, Linux and Windows).
