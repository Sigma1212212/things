/*
 * ASM3D - a3_scene_io.h
 * Scene serialization to a Git-friendly JSON format (.a3scene), plus the
 * per-component serializers reused by save games, prefabs, copy/paste and
 * the undo system.
 *
 * Guarantees:
 *   - fields are written by name, never as raw memory
 *   - entity references are written as stable GUIDs
 *   - components from missing plugins are preserved verbatim on re-save
 *   - older format versions are migrated; newer versions are refused with a
 *     clear message instead of being silently misread
 */
#ifndef A3_SCENE_IO_H
#define A3_SCENE_IO_H

#include "../ecs/a3_ecs.h"
#include "../core/a3_json.h"

A3_EXTERN_C_BEGIN

#define A3_SCENE_MAX_WARNINGS 16

typedef struct A3SceneLoadReport {
    u32 entities_loaded;
    u32 components_loaded;
    u32 unknown_components;
    u32 missing_fields;      /* fields absent in file: defaults used */
    u32 migrated_from;       /* 0 if no migration happened */
    u32 warning_count;
    char warnings[A3_SCENE_MAX_WARNINGS][160];
    char error[256];         /* technical error */
    char hint[256];          /* plain-language explanation */
} A3SceneLoadReport;

typedef enum A3SceneSaveFlags {
    A3_SCENE_SAVE_DEFAULT = 0,
    A3_SCENE_SAVE_FOR_BUILD = 1 << 0, /* drop editor-only entities/components */
    A3_SCENE_SAVE_COMPACT = 1 << 1,
    A3_SCENE_SAVE_INCLUDE_RUNTIME = 1 << 2, /* include DONT_SAVE entities (save games) */
} A3SceneSaveFlags;

A3Result a3_scene_save_json(A3World *w, A3StrBuf *out, u32 flags);
A3Result a3_scene_save_file(A3World *w, const char *path, u32 flags);
/* Loads into w (additive; call a3_world_clear first to replace). */
A3Result a3_scene_load_json(A3World *w, const char *text, usize len, A3SceneLoadReport *report);
A3Result a3_scene_load_file(A3World *w, const char *path, A3SceneLoadReport *report);

/* Serialize a subtree (prefab / clipboard). Root GUIDs are remapped on paste. */
A3Result a3_entities_save_json(A3World *w, const A3Entity *roots, u32 count, A3StrBuf *out);
/* Instantiates entities from a3_entities_save_json output with fresh GUIDs.
 * Internal references are remapped. Returns number of roots created. */
u32 a3_entities_load_json(A3World *w, const char *text, usize len, A3Entity parent, A3Entity *out_roots, u32 max_roots);

/* Component-level helpers. */
void a3_component_write_json(A3JsonWriter *jw, const A3ComponentType *t, const void *data);
/* Reads fields present in obj into data; returns number of missing fields. */
u32  a3_component_read_json(const A3Json *obj, const A3ComponentType *t, void *data);
void a3_field_write_json(A3JsonWriter *jw, const A3FieldDesc *f, const void *component_data);
b32  a3_field_read_json(const A3Json *value, const A3FieldDesc *f, void *component_data);

void a3_guid_to_string(u64 guid, char out[17]);
u64  a3_guid_from_string(const char *s);

A3_EXTERN_C_END

#endif
