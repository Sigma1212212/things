/*
 * ASM3D - a3_json.h
 * JSON reader (DOM in an arena) and pretty writer. Used for scenes, project
 * settings, prefabs, visual scripts and shader graphs: text formats that diff
 * and merge cleanly in Git.
 *
 * Parse errors report line and column so the editor can point at the problem.
 */
#ifndef A3_JSON_H
#define A3_JSON_H

#include "a3_base.h"
#include "a3_memory.h"
#include "a3_strbuf.h"

A3_EXTERN_C_BEGIN

typedef enum A3JsonType {
    A3_JSON_NULL = 0,
    A3_JSON_BOOL,
    A3_JSON_NUMBER,
    A3_JSON_STRING,
    A3_JSON_ARRAY,
    A3_JSON_OBJECT,
} A3JsonType;

typedef struct A3Json A3Json;
struct A3Json {
    A3JsonType type;
    const char *key;     /* member name when inside an object */
    union {
        b32 boolean;
        f64 number;
        const char *string;
        struct { A3Json *first; A3Json *last; u32 count; } children;
    } v;
    A3Json *next;        /* next sibling */
};

typedef struct A3JsonError {
    int line, column;
    char message[128];
} A3JsonError;

/* Parses text (not required to be NUL terminated). Nodes live in `arena`.
 * Returns NULL on error and fills `err` (optional). Supports // comments and
 * trailing commas for hand-edited files. */
A3Json *a3_json_parse(const char *text, usize len, A3Arena *arena, A3JsonError *err);

A3Json *a3_json_get(const A3Json *obj, const char *key);     /* object member or NULL */
A3Json *a3_json_at(const A3Json *arr, u32 index);            /* array element or NULL */
u32     a3_json_count(const A3Json *node);
f64         a3_json_number(const A3Json *node, f64 fallback);
b32         a3_json_bool(const A3Json *node, b32 fallback);
const char *a3_json_string(const A3Json *node, const char *fallback);
/* Convenience getters on object members. */
f64         a3_json_get_number(const A3Json *obj, const char *key, f64 fallback);
b32         a3_json_get_bool(const A3Json *obj, const char *key, b32 fallback);
const char *a3_json_get_string(const A3Json *obj, const char *key, const char *fallback);
/* Reads up to n numbers from an array into out; returns count read. */
u32         a3_json_get_floats(const A3Json *arr, f32 *out, u32 n);

#define A3_JSON_FOREACH(child, parent) \
    for (A3Json *child = ((parent) && ((parent)->type == A3_JSON_ARRAY || (parent)->type == A3_JSON_OBJECT)) ? (parent)->v.children.first : 0; \
         child; child = child->next)

/* ---- Writer ---- */
typedef struct A3JsonWriter {
    A3StrBuf *out;
    int depth;
    b32 need_comma[32];
    b32 compact;
    b32 pending_key;
} A3JsonWriter;

void a3_jw_init(A3JsonWriter *w, A3StrBuf *out, b32 compact);
void a3_jw_begin_object(A3JsonWriter *w);
void a3_jw_end_object(A3JsonWriter *w);
void a3_jw_begin_array(A3JsonWriter *w);
void a3_jw_end_array(A3JsonWriter *w);
void a3_jw_key(A3JsonWriter *w, const char *key);
void a3_jw_string(A3JsonWriter *w, const char *s);
void a3_jw_number(A3JsonWriter *w, f64 v);
void a3_jw_int(A3JsonWriter *w, i64 v);
void a3_jw_bool(A3JsonWriter *w, b32 v);
void a3_jw_null(A3JsonWriter *w);
/* Writes a short numeric array on one line: [1, 2, 3] */
void a3_jw_floats(A3JsonWriter *w, const f32 *v, u32 n);
/* key + value helpers */
/* Inserts pre-formatted JSON text as a value (caller guarantees validity). */
void a3_jw_raw_value(A3JsonWriter *w, const char *raw_json);
/* Writes a parsed DOM node (and children) as a value. */
void a3_jw_node(A3JsonWriter *w, const A3Json *node);
void a3_jw_kv_string(A3JsonWriter *w, const char *key, const char *s);
void a3_jw_kv_number(A3JsonWriter *w, const char *key, f64 v);
void a3_jw_kv_int(A3JsonWriter *w, const char *key, i64 v);
void a3_jw_kv_bool(A3JsonWriter *w, const char *key, b32 v);
void a3_jw_kv_floats(A3JsonWriter *w, const char *key, const f32 *v, u32 n);

A3_EXTERN_C_END

#endif
