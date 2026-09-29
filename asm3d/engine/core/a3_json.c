/*
 * ASM3D - a3_json.c
 */
#include "a3_json.h"
#include "a3_string.h"
#include "a3_format.h"

typedef struct JsonParser {
    const char *p, *end, *start;
    A3Arena *arena;
    A3JsonError *err;
    b32 failed;
    int depth;
} JsonParser;

static void jp_fail(JsonParser *jp, const char *msg) {
    if (jp->failed) return;
    jp->failed = 1;
    if (jp->err) {
        int line = 1, col = 1;
        for (const char *c = jp->start; c < jp->p && c < jp->end; ++c) {
            if (*c == '\n') { ++line; col = 1; } else ++col;
        }
        jp->err->line = line;
        jp->err->column = col;
        a3_strcpy(jp->err->message, sizeof(jp->err->message), msg);
    }
}

static void jp_skip_ws(JsonParser *jp) {
    for (;;) {
        while (jp->p < jp->end && a3_is_space(*jp->p)) ++jp->p;
        if (jp->p + 1 < jp->end && jp->p[0] == '/' && jp->p[1] == '/') {
            while (jp->p < jp->end && *jp->p != '\n') ++jp->p;
            continue;
        }
        if (jp->p + 1 < jp->end && jp->p[0] == '/' && jp->p[1] == '*') {
            jp->p += 2;
            while (jp->p + 1 < jp->end && !(jp->p[0] == '*' && jp->p[1] == '/')) ++jp->p;
            jp->p = jp->p + 2 <= jp->end ? jp->p + 2 : jp->end;
            continue;
        }
        break;
    }
}

static A3Json *jp_node(JsonParser *jp, A3JsonType t) {
    A3Json *n = A3_ARENA_PUSH(jp->arena, A3Json);
    if (!n) { jp_fail(jp, "out of memory"); return 0; }
    n->type = t;
    return n;
}

static void utf8_append(char *out, usize *n, u32 cp) {
    if (cp < 0x80) out[(*n)++] = (char)cp;
    else if (cp < 0x800) { out[(*n)++] = (char)(0xC0 | (cp >> 6)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out[(*n)++] = (char)(0xE0 | (cp >> 12)); out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else { out[(*n)++] = (char)(0xF0 | (cp >> 18)); out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *jp_string(JsonParser *jp) {
    if (jp->p >= jp->end || *jp->p != '"') { jp_fail(jp, "expected string"); return 0; }
    ++jp->p;
    const char *s = jp->p;
    usize raw = 0;
    while (s + raw < jp->end && s[raw] != '"') { if (s[raw] == '\\') ++raw; ++raw; }
    if (s + raw >= jp->end) { jp_fail(jp, "unterminated string"); return 0; }
    char *out = (char *)a3_arena_push(jp->arena, raw + 1, 1);
    if (!out) { jp_fail(jp, "out of memory"); return 0; }
    usize n = 0;
    while (jp->p < jp->end && *jp->p != '"') {
        char c = *jp->p++;
        if (c == '\\') {
            if (jp->p >= jp->end) break;
            char e = *jp->p++;
            switch (e) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case 'u': {
                u32 cp = 0;
                for (int i = 0; i < 4; ++i) {
                    int h = jp->p < jp->end ? hexval(*jp->p++) : -1;
                    if (h < 0) { jp_fail(jp, "bad \\u escape"); return 0; }
                    cp = cp * 16 + (u32)h;
                }
                if (cp > 0x7FF && n + 3 > raw) cp = '?'; /* guard: escaped form is >= encoded */
                utf8_append(out, &n, cp);
            } break;
            default: out[n++] = e; break;
            }
        } else {
            out[n++] = c;
        }
    }
    ++jp->p; /* closing quote */
    out[n] = 0;
    return out;
}

static A3Json *jp_value(JsonParser *jp);

static A3Json *jp_container(JsonParser *jp, b32 object) {
    A3Json *node = jp_node(jp, object ? A3_JSON_OBJECT : A3_JSON_ARRAY);
    if (!node) return 0;
    if (++jp->depth > 128) { jp_fail(jp, "nesting too deep"); return 0; }
    char close = object ? '}' : ']';
    ++jp->p;
    for (;;) {
        jp_skip_ws(jp);
        if (jp->p >= jp->end) { jp_fail(jp, object ? "unterminated object" : "unterminated array"); return 0; }
        if (*jp->p == close) { ++jp->p; break; }
        const char *key = 0;
        if (object) {
            key = jp_string(jp);
            if (!key) return 0;
            jp_skip_ws(jp);
            if (jp->p >= jp->end || *jp->p != ':') { jp_fail(jp, "expected ':' after key"); return 0; }
            ++jp->p;
        }
        A3Json *child = jp_value(jp);
        if (!child) return 0;
        child->key = key;
        if (node->v.children.last) node->v.children.last->next = child;
        else node->v.children.first = child;
        node->v.children.last = child;
        node->v.children.count++;
        jp_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { ++jp->p; continue; }
        if (jp->p < jp->end && *jp->p == close) { ++jp->p; break; }
        jp_fail(jp, object ? "expected ',' or '}'" : "expected ',' or ']'");
        return 0;
    }
    --jp->depth;
    return node;
}

static A3Json *jp_value(JsonParser *jp) {
    jp_skip_ws(jp);
    if (jp->p >= jp->end) { jp_fail(jp, "unexpected end of input"); return 0; }
    char c = *jp->p;
    if (c == '{') return jp_container(jp, 1);
    if (c == '[') return jp_container(jp, 0);
    if (c == '"') {
        A3Json *n = jp_node(jp, A3_JSON_STRING);
        if (!n) return 0;
        n->v.string = jp_string(jp);
        return n->v.string ? n : 0;
    }
    if (c == '-' || a3_is_digit(c)) {
        const char *s = jp->p;
        while (jp->p < jp->end && (a3_is_digit(*jp->p) || *jp->p == '-' || *jp->p == '+' || *jp->p == '.' || *jp->p == 'e' || *jp->p == 'E')) ++jp->p;
        A3Json *n = jp_node(jp, A3_JSON_NUMBER);
        if (!n) return 0;
        if (!a3_parse_f64(s, (usize)(jp->p - s), &n->v.number)) { jp->p = s; jp_fail(jp, "malformed number"); return 0; }
        return n;
    }
    usize left = (usize)(jp->end - jp->p);
    if (left >= 4 && a3_strncmp(jp->p, "true", 4) == 0) { jp->p += 4; A3Json *n = jp_node(jp, A3_JSON_BOOL); if (n) n->v.boolean = 1; return n; }
    if (left >= 5 && a3_strncmp(jp->p, "false", 5) == 0) { jp->p += 5; A3Json *n = jp_node(jp, A3_JSON_BOOL); if (n) n->v.boolean = 0; return n; }
    if (left >= 4 && a3_strncmp(jp->p, "null", 4) == 0) { jp->p += 4; return jp_node(jp, A3_JSON_NULL); }
    jp_fail(jp, "unexpected character");
    return 0;
}

A3Json *a3_json_parse(const char *text, usize len, A3Arena *arena, A3JsonError *err) {
    if (err) a3_zero_struct(err);
    if (!text || !arena) {
        if (err) a3_strcpy(err->message, sizeof(err->message), "invalid arguments");
        return 0;
    }
    JsonParser jp = { text, text + len, text, arena, err, 0, 0 };
    /* skip UTF-8 BOM */
    if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) jp.p += 3;
    A3Json *root = jp_value(&jp);
    if (!root || jp.failed) return 0;
    jp_skip_ws(&jp);
    if (jp.p < jp.end) { jp_fail(&jp, "trailing characters after value"); return 0; }
    return root;
}

A3Json *a3_json_get(const A3Json *obj, const char *key) {
    if (!obj || obj->type != A3_JSON_OBJECT) return 0;
    for (A3Json *c = obj->v.children.first; c; c = c->next) if (c->key && a3_streq(c->key, key)) return c;
    return 0;
}

A3Json *a3_json_at(const A3Json *arr, u32 index) {
    if (!arr || (arr->type != A3_JSON_ARRAY && arr->type != A3_JSON_OBJECT)) return 0;
    A3Json *c = arr->v.children.first;
    while (c && index--) c = c->next;
    return c;
}

u32 a3_json_count(const A3Json *n) {
    return (n && (n->type == A3_JSON_ARRAY || n->type == A3_JSON_OBJECT)) ? n->v.children.count : 0;
}

f64 a3_json_number(const A3Json *n, f64 fb) {
    if (!n) return fb;
    if (n->type == A3_JSON_NUMBER) return n->v.number;
    if (n->type == A3_JSON_BOOL) return n->v.boolean ? 1.0 : 0.0;
    return fb;
}
b32 a3_json_bool(const A3Json *n, b32 fb) {
    if (!n) return fb;
    if (n->type == A3_JSON_BOOL) return n->v.boolean;
    if (n->type == A3_JSON_NUMBER) return n->v.number != 0.0;
    return fb;
}
const char *a3_json_string(const A3Json *n, const char *fb) { return (n && n->type == A3_JSON_STRING) ? n->v.string : fb; }
f64 a3_json_get_number(const A3Json *o, const char *k, f64 fb) { return a3_json_number(a3_json_get(o, k), fb); }
b32 a3_json_get_bool(const A3Json *o, const char *k, b32 fb) { return a3_json_bool(a3_json_get(o, k), fb); }
const char *a3_json_get_string(const A3Json *o, const char *k, const char *fb) { return a3_json_string(a3_json_get(o, k), fb); }

u32 a3_json_get_floats(const A3Json *arr, f32 *out, u32 n) {
    u32 i = 0;
    if (!arr) return 0;
    if (arr->type == A3_JSON_NUMBER && n > 0) { out[0] = (f32)arr->v.number; return 1; }
    A3_JSON_FOREACH(c, arr) {
        if (i >= n) break;
        if (c->type == A3_JSON_NUMBER) out[i++] = (f32)c->v.number;
    }
    return i;
}

/* ---- Writer ---- */

void a3_jw_init(A3JsonWriter *w, A3StrBuf *out, b32 compact) {
    a3_zero_struct(w);
    w->out = out;
    w->compact = compact;
}

static void jw_newline(A3JsonWriter *w) {
    if (w->compact) return;
    a3_strbuf_append_char(w->out, '\n');
    a3_strbuf_append_repeat(w->out, ' ', (usize)w->depth * 2);
}

static void jw_prefix(A3JsonWriter *w) {
    if (w->pending_key) { w->pending_key = 0; return; }
    if (w->depth > 0) {
        if (w->need_comma[w->depth]) a3_strbuf_append_char(w->out, ',');
        w->need_comma[w->depth] = 1;
        jw_newline(w);
    }
}

static void jw_escaped(A3StrBuf *out, const char *s) {
    a3_strbuf_append_char(out, '"');
    for (; s && *s; ++s) {
        u8 c = (u8)*s;
        switch (c) {
        case '"': a3_strbuf_append(out, "\\\""); break;
        case '\\': a3_strbuf_append(out, "\\\\"); break;
        case '\n': a3_strbuf_append(out, "\\n"); break;
        case '\r': a3_strbuf_append(out, "\\r"); break;
        case '\t': a3_strbuf_append(out, "\\t"); break;
        default:
            if (c < 0x20) a3_strbuf_appendf(out, "\\u%04x", c);
            else a3_strbuf_append_char(out, (char)c);
        }
    }
    a3_strbuf_append_char(out, '"');
}

void a3_jw_begin_object(A3JsonWriter *w) {
    jw_prefix(w);
    a3_strbuf_append_char(w->out, '{');
    if (w->depth < 31) w->depth++;
    w->need_comma[w->depth] = 0;
}

void a3_jw_end_object(A3JsonWriter *w) {
    b32 had = w->need_comma[w->depth];
    if (w->depth > 0) w->depth--;
    if (had) jw_newline(w);
    a3_strbuf_append_char(w->out, '}');
}

void a3_jw_begin_array(A3JsonWriter *w) {
    jw_prefix(w);
    a3_strbuf_append_char(w->out, '[');
    if (w->depth < 31) w->depth++;
    w->need_comma[w->depth] = 0;
}

void a3_jw_end_array(A3JsonWriter *w) {
    b32 had = w->need_comma[w->depth];
    if (w->depth > 0) w->depth--;
    if (had) jw_newline(w);
    a3_strbuf_append_char(w->out, ']');
}

void a3_jw_key(A3JsonWriter *w, const char *key) {
    jw_prefix(w);
    jw_escaped(w->out, key);
    a3_strbuf_append(w->out, w->compact ? ":" : ": ");
    w->pending_key = 1;
}

void a3_jw_string(A3JsonWriter *w, const char *s) { jw_prefix(w); jw_escaped(w->out, s ? s : ""); }

static void jw_num_raw(A3StrBuf *out, f64 v) {
    if (v != v) { a3_strbuf_append(out, "0"); return; } /* NaN is not valid JSON */
    if (v > 1e300) v = 1e300;
    if (v < -1e300) v = -1e300;
    if (v == (f64)(i64)v && v < 1e15 && v > -1e15) { a3_strbuf_appendf(out, "%lld", (long long)(i64)v); return; }
    /* Shortest text that reads back to the same float: keeps files tidy
     * ("0.1" instead of "0.100000001") while round-tripping exactly. */
    char buf[48];
    b32 is_f32 = (f64)(f32)v == v;
    for (int prec = 6; prec <= 17; ++prec) {
        int n = a3_snprintf(buf, sizeof(buf), "%.*g", prec, v);
        f64 back;
        if (a3_parse_f64(buf, (usize)n, &back) && (is_f32 ? ((f32)back == (f32)v) : (back == v))) break;
    }
    a3_strbuf_append(out, buf);
}

void a3_jw_number(A3JsonWriter *w, f64 v) { jw_prefix(w); jw_num_raw(w->out, v); }
void a3_jw_int(A3JsonWriter *w, i64 v) { jw_prefix(w); a3_strbuf_appendf(w->out, "%lld", (long long)v); }
void a3_jw_bool(A3JsonWriter *w, b32 v) { jw_prefix(w); a3_strbuf_append(w->out, v ? "true" : "false"); }
void a3_jw_null(A3JsonWriter *w) { jw_prefix(w); a3_strbuf_append(w->out, "null"); }

void a3_jw_floats(A3JsonWriter *w, const f32 *v, u32 n) {
    jw_prefix(w);
    a3_strbuf_append_char(w->out, '[');
    for (u32 i = 0; i < n; ++i) {
        if (i) a3_strbuf_append(w->out, w->compact ? "," : ", ");
        jw_num_raw(w->out, (f64)v[i]);
    }
    a3_strbuf_append_char(w->out, ']');
}

void a3_jw_kv_string(A3JsonWriter *w, const char *k, const char *s) { a3_jw_key(w, k); a3_jw_string(w, s); }
void a3_jw_kv_number(A3JsonWriter *w, const char *k, f64 v) { a3_jw_key(w, k); a3_jw_number(w, v); }
void a3_jw_kv_int(A3JsonWriter *w, const char *k, i64 v) { a3_jw_key(w, k); a3_jw_int(w, v); }
void a3_jw_kv_bool(A3JsonWriter *w, const char *k, b32 v) { a3_jw_key(w, k); a3_jw_bool(w, v); }
void a3_jw_kv_floats(A3JsonWriter *w, const char *k, const f32 *v, u32 n) { a3_jw_key(w, k); a3_jw_floats(w, v, n); }

void a3_jw_raw_value(A3JsonWriter *w, const char *raw) { jw_prefix(w); a3_strbuf_append(w->out, raw ? raw : "null"); }

void a3_jw_node(A3JsonWriter *w, const A3Json *n) {
    if (!n) { a3_jw_null(w); return; }
    switch (n->type) {
    case A3_JSON_NULL: a3_jw_null(w); break;
    case A3_JSON_BOOL: a3_jw_bool(w, n->v.boolean); break;
    case A3_JSON_NUMBER: a3_jw_number(w, n->v.number); break;
    case A3_JSON_STRING: a3_jw_string(w, n->v.string); break;
    case A3_JSON_ARRAY: {
        /* short number arrays (vectors, colors) stay on one line */
        b32 numbers = n->v.children.count > 0 && n->v.children.count <= 16;
        for (const A3Json *c = n->v.children.first; c && numbers; c = c->next) if (c->type != A3_JSON_NUMBER) numbers = 0;
        if (numbers) {
            jw_prefix(w);
            a3_strbuf_append_char(w->out, '[');
            for (const A3Json *c = n->v.children.first; c; c = c->next) {
                if (c != n->v.children.first) a3_strbuf_append(w->out, w->compact ? "," : ", ");
                jw_num_raw(w->out, c->v.number);
            }
            a3_strbuf_append_char(w->out, ']');
            break;
        }
        a3_jw_begin_array(w);
        for (const A3Json *c = n->v.children.first; c; c = c->next) a3_jw_node(w, c);
        a3_jw_end_array(w);
    } break;
    case A3_JSON_OBJECT:
        a3_jw_begin_object(w);
        for (const A3Json *c = n->v.children.first; c; c = c->next) { a3_jw_key(w, c->key ? c->key : ""); a3_jw_node(w, c); }
        a3_jw_end_object(w);
        break;
    }
}
