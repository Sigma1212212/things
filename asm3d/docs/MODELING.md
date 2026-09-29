# Modeling

ASM3D includes a polygon modeler: **Edit Mode** in the editor and the
`asm3d_cli mesh` commands. Both use `engine/modeling` (editable meshes with
n-gons) whose hot loops are hand-written x86-64 SSE assembly with
bit-identical C references (`engine/modeling/a3_modeling_x64.S`):

| Kernel | Used for |
|---|---|
| masked point transform | move / rotate / scale of the selection, whole-mesh transforms |
| triangle normals | smooth shading when a model is converted for rendering |
| ray / triangle picking | clicking faces in the viewport, `a3_emesh_raycast` |
| bounds | statistics and framing |

## Edit Mode

Select an object and press **Tab**. An `.obj` model is opened as is; a
primitive (cube, sphere...) is converted to `Assets/Models/<Name>.obj`.
The Modeling panel has a button for every tool; the keys are:

| Key | Action |
|---|---|
| 1 / 2 / 3 | vertex / edge / face selection |
| click, Shift+click, drag | select, add to selection, box select |
| double-click an edge | select its edge loop |
| A, Alt+A, Ctrl+I, L, Ctrl+= | all, none, invert, linked, grow |
| G, R, S | move, rotate, scale (X / Y / Z lock an axis, Ctrl snaps, click confirms, right click cancels) |
| E | extrude selected faces, then move along their normal |
| I | inset selected faces (move the mouse toward the center) |
| Ctrl+R | loop cut (mouse wheel: number of cuts) |
| Ctrl+2 | Catmull-Clark subdivision |
| X / Delete, F, M, Shift+D, Shift+N | delete, fill, merge by distance, duplicate, recalculate normals |
| Alt+Z | X-ray (see and select through the mesh) |
| Ctrl+Z / Ctrl+Y | undo / redo mesh edits |
| Tab | save the model and leave Edit Mode |

The viewport shows every change immediately. Flat-shaded models are saved
with one normal per face so hard edges stay hard in any OBJ reader.

## Command line

```sh
asm3d_cli mesh new cube --size 2 --out House.obj
asm3d_cli mesh edit House.obj --op mode:face --op select-normal:0,1,0 --op extrude:1 --op scale:0.05,1,1
asm3d_cli mesh info House.obj
asm3d_cli mesh ops            # every operation with its arguments
```

## Operations

Primitives (cube, plane, grid, cylinder, cone, UV sphere, torus), selection
modes, select all / none / invert / linked / more / edge loop / by normal / by
box, move / rotate / scale, region extrude, inset, loop cut, Catmull-Clark
and simple subdivision, delete faces or vertices, merge by distance, fill,
duplicate, flip, recalculate normals (consistent and outward), mirror,
flat / smooth shading, OBJ import and export (polygons kept).

**Not implemented yet:** bevel, knife, booleans, UV unwrapping, sculpting,
modifiers stacks. Tested by `tests/test_modeling.c` (topology counts,
closed meshes, outward normals, bit-exact kernels) and the editor self test.
