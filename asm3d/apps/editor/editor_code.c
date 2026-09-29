/*
 * ASM3D Editor - built-in code editor.
 *
 * Tabs, syntax highlighting (ASM3D script, GLSL, JSON), line numbers,
 * current-line and selection highlight, error markers with messages,
 * mouse selection, clipboard, undo/redo, find & replace, auto-indent.
 * Text is UTF-8; the cursor never lands inside a multi-byte sequence.
 */
#include "editor.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/core/a3_json.h"
#include "../../engine/platform/a3_platform.h"

#define GUTTER_W 54.0f

enum { LANG_TEXT = 0, LANG_SCRIPT, LANG_GLSL, LANG_JSON };
enum { EK_NONE = 0, EK_TYPE, EK_DELETE, EK_OTHER };

static u32 colr(A3Ui *ui, A3UiColor c) { return a3_ui_theme(ui)->colors[c]; }

/* ======================================================================== */
/* Buffer                                                                   */
/* ======================================================================== */

static b32 doc_reserve(EdDocument *d, u32 extra) {
    u32 need = d->len + extra + 1;
    if (need <= d->cap) return 1;
    u32 cap = d->cap ? d->cap : 256;
    while (cap < need) cap *= 2;
    char *t = (char *)a3_realloc(d->text, cap, A3_MEM_EDITOR);
    if (!t) { A3_ERROR("code", "out of memory while editing %s", d->path); return 0; }
    d->text = t;
    d->cap = cap;
    return 1;
}

static void doc_insert(EdDocument *d, i32 pos, const char *s, u32 n) {
    if (!n || !doc_reserve(d, n)) return;
    pos = a3_clampi(pos, 0, (i32)d->len);
    a3_memmove(d->text + pos + n, d->text + pos, d->len - (u32)pos + 1);
    a3_memcpy(d->text + pos, s, n);
    d->len += n;
    d->dirty = 1;
    d->lines_dirty = 1;
}

static void doc_erase(EdDocument *d, i32 a, i32 b) {
    a = a3_clampi(a, 0, (i32)d->len);
    b = a3_clampi(b, 0, (i32)d->len);
    if (b <= a) return;
    a3_memmove(d->text + a, d->text + b, d->len - (u32)b + 1);
    d->len -= (u32)(b - a);
    d->dirty = 1;
    d->lines_dirty = 1;
}

static void doc_set_text(EdDocument *d, const char *s) {
    u32 n = (u32)a3_strlen(s);
    d->len = 0;
    if (!doc_reserve(d, n)) return;
    a3_memcpy(d->text, s, n + 1);
    d->len = n;
    d->lines_dirty = 1;
}

static void doc_lines(EdDocument *d) {
    if (!d->lines_dirty && d->line_starts) return;
    u32 n = 1;
    for (u32 i = 0; i < d->len; ++i) n += d->text[i] == '\n';
    if (n > d->line_cap) {
        u32 cap = a3_maxu(n, d->line_cap * 2);
        u32 *ls = (u32 *)a3_realloc(d->line_starts, sizeof(u32) * cap, A3_MEM_EDITOR);
        if (!ls) return;
        d->line_starts = ls;
        d->line_cap = cap;
    }
    u32 k = 0;
    d->line_starts[k++] = 0;
    for (u32 i = 0; i < d->len; ++i) if (d->text[i] == '\n') d->line_starts[k++] = i + 1;
    d->line_count = k;
    d->lines_dirty = 0;
}

static u32 line_of(EdDocument *d, i32 pos) {
    doc_lines(d);
    u32 lo = 0, hi = d->line_count;
    while (hi - lo > 1) { u32 mid = (lo + hi) / 2; if ((i32)d->line_starts[mid] <= pos) lo = mid; else hi = mid; }
    return lo;
}
static i32 line_start(EdDocument *d, u32 line) { doc_lines(d); return line < d->line_count ? (i32)d->line_starts[line] : (i32)d->len; }
static i32 line_end(EdDocument *d, u32 line) { doc_lines(d); return line + 1 < d->line_count ? (i32)d->line_starts[line + 1] - 1 : (i32)d->len; }

static b32 is_cont(char c) { return ((u8)c & 0xC0) == 0x80; }
static i32 prev_char(EdDocument *d, i32 p) { if (p <= 0) return 0; --p; while (p > 0 && is_cont(d->text[p])) --p; return p; }
static i32 next_char(EdDocument *d, i32 p) { if (p >= (i32)d->len) return (i32)d->len; ++p; while (p < (i32)d->len && is_cont(d->text[p])) ++p; return p; }

/* column (in characters, tabs = 4) of a byte position within its line */
static i32 column_of(EdDocument *d, i32 pos) {
    i32 ls = line_start(d, line_of(d, pos)), c = 0;
    for (i32 i = ls; i < pos; ++i) { if (is_cont(d->text[i])) continue; c += d->text[i] == '\t' ? 4 - (c % 4) : 1; }
    return c;
}
static i32 pos_at_column(EdDocument *d, u32 line, i32 col) {
    i32 p = line_start(d, line), e = line_end(d, line), c = 0;
    while (p < e) {
        i32 w = d->text[p] == '\t' ? 4 - (c % 4) : 1;
        if (c + w > col) break;
        c += w;
        p = next_char(d, p);
    }
    return p;
}

static i32 sel_min(EdDocument *d) { return a3_mini(d->cursor, d->anchor); }
static i32 sel_max(EdDocument *d) { return a3_maxi(d->cursor, d->anchor); }
static b32 has_sel(EdDocument *d) { return d->cursor != d->anchor; }

/* ---- undo ---- */

static void stack_push(char **texts, i32 *cursors, u32 *count, char *text, i32 cursor) {
    if (*count == ED_TEXT_UNDO) {
        a3_free(texts[0]);
        a3_memmove(texts, texts + 1, sizeof(char *) * (ED_TEXT_UNDO - 1));
        a3_memmove(cursors, cursors + 1, sizeof(i32) * (ED_TEXT_UNDO - 1));
        (*count)--;
    }
    texts[*count] = text;
    cursors[*count] = cursor;
    (*count)++;
}

static void clear_stack(char **texts, u32 *count) { for (u32 i = 0; i < *count; ++i) a3_free(texts[i]); *count = 0; }

/* Records the state before an edit. Consecutive typing is grouped. */
static void undo_checkpoint(EdDocument *d, i32 kind, f64 now) {
    b32 group = kind == d->last_edit_kind && kind != EK_OTHER && now - d->last_edit_time < 1.0;
    d->last_edit_kind = kind;
    d->last_edit_time = now;
    if (group) return;
    char *copy = a3_strdup(d->text ? d->text : "", A3_MEM_EDITOR);
    if (!copy) return;
    stack_push(d->undo_text, d->undo_cursor, &d->undo_count, copy, d->cursor);
    clear_stack(d->redo_text, &d->redo_count);
}

static void doc_undo(EdDocument *d) {
    if (!d->undo_count) return;
    char *cur = a3_strdup(d->text ? d->text : "", A3_MEM_EDITOR);
    if (cur) stack_push(d->redo_text, d->redo_cursor, &d->redo_count, cur, d->cursor);
    d->undo_count--;
    doc_set_text(d, d->undo_text[d->undo_count]);
    d->cursor = d->anchor = a3_clampi(d->undo_cursor[d->undo_count], 0, (i32)d->len);
    a3_free(d->undo_text[d->undo_count]);
    d->dirty = 1;
    d->last_edit_kind = EK_NONE;
}

static void doc_redo(EdDocument *d) {
    if (!d->redo_count) return;
    char *cur = a3_strdup(d->text ? d->text : "", A3_MEM_EDITOR);
    if (cur) stack_push(d->undo_text, d->undo_cursor, &d->undo_count, cur, d->cursor);
    d->redo_count--;
    doc_set_text(d, d->redo_text[d->redo_count]);
    d->cursor = d->anchor = a3_clampi(d->redo_cursor[d->redo_count], 0, (i32)d->len);
    a3_free(d->redo_text[d->redo_count]);
    d->dirty = 1;
    d->last_edit_kind = EK_NONE;
}

static void delete_selection(EdDocument *d) {
    if (!has_sel(d)) return;
    i32 a = sel_min(d), b = sel_max(d);
    doc_erase(d, a, b);
    d->cursor = d->anchor = a;
}

static void replace_selection(EdDocument *d, const char *s, u32 n) {
    delete_selection(d);
    doc_insert(d, d->cursor, s, n);
    d->cursor += (i32)n;
    d->anchor = d->cursor;
}

/* ======================================================================== */
/* Open / save                                                              */
/* ======================================================================== */

static i32 language_for(const char *path) {
    const char *ext = a3_path_extension(path);
    if (a3_streq(ext, ".a3script")) return LANG_SCRIPT;
    if (a3_streq(ext, ".glsl") || a3_streq(ext, ".vert") || a3_streq(ext, ".frag")) return LANG_GLSL;
    if (a3_streq(ext, ".json") || a3_streq(ext, ".a3comp") || a3_streq(ext, ".a3mat") || a3_streq(ext, ".a3proj") || a3_streq(ext, ".a3shader") || a3_streq(ext, ".a3anim")) return LANG_JSON;
    return LANG_TEXT;
}

static void doc_free(EdDocument *d) {
    a3_free(d->text);
    a3_free(d->line_starts);
    clear_stack(d->undo_text, &d->undo_count);
    clear_stack(d->redo_text, &d->redo_count);
    a3_zero_struct(d);
}

i32 ed_code_open(A3Editor *ed, const char *path) {
    for (u32 i = 0; i < ed->doc_count; ++i) if (a3_streq(ed->docs[i].path, path)) { ed->doc_active = (i32)i; return (i32)i; }
    if (ed->doc_count >= A3_ARRAY_COUNT(ed->docs)) { a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_WARNING), "Close a tab first (16 files max)"); return -1; }
    EdDocument *d = &ed->docs[ed->doc_count];
    a3_zero_struct(d);
    a3_strcpy(d->path, sizeof(d->path), path);
    d->language = language_for(path);
    char abs[ED_PATH];
    ed_project_path(ed, path, abs, sizeof(abs));
    A3FileData fd;
    if (a3_file_read_all(abs, A3_MEM_EDITOR, &fd) == A3_OK) {
        /* normalise line endings */
        u32 w = 0;
        for (u32 r = 0; r < fd.size; ++r) if (fd.data[r] != '\r') fd.data[w++] = fd.data[r];
        fd.data[w] = 0;
        doc_set_text(d, (const char *)fd.data);
        a3_free(fd.data);
    } else {
        doc_set_text(d, ""); /* new file, created on save */
        d->dirty = 1;
    }
    if (!d->text) { doc_free(d); return -1; }
    ed->doc_active = (i32)ed->doc_count++;
    return ed->doc_active;
}

static void validate(A3Editor *ed, EdDocument *d) {
    d->error_count = 0;
    if (d->language != LANG_JSON) return;
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(64));
    A3JsonError err;
    if (!a3_json_parse(d->text, d->len, &ar, &err)) {
        d->error_lines[0] = err.line;
        a3_snprintf(d->error_msgs[0], sizeof(d->error_msgs[0]), "%s", err.message);
        d->error_count = 1;
        a3_log_hint(A3_LOG_WARN, "code", "Check for a missing comma, quote or bracket on or just before that line.", "%s:%d: %s", d->path, err.line, err.message);
    }
    a3_arena_release(&ar);
    A3_UNUSED(ed);
}

b32 ed_code_save(A3Editor *ed, i32 i) {
    if (i < 0 || i >= (i32)ed->doc_count) return 0;
    EdDocument *d = &ed->docs[i];
    char abs[ED_PATH], dir[ED_PATH];
    ed_project_path(ed, d->path, abs, sizeof(abs));
    a3_path_dirname(abs, dir, sizeof(dir));
    a3_dir_create(dir);
    if (a3_file_write_atomic(abs, d->text, d->len) != A3_OK) {
        a3_log_hint(A3_LOG_ERROR, "code", "Check that the folder exists and is writable.", "could not save %s", d->path);
        return 0;
    }
    d->dirty = 0;
    validate(ed, d);
    a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_SUCCESS), "Saved %s", a3_path_filename(d->path));
    return 1;
}

static void close_doc(A3Editor *ed, i32 i) {
    doc_free(&ed->docs[i]);
    a3_memmove(&ed->docs[i], &ed->docs[i + 1], sizeof(EdDocument) * (ed->doc_count - (u32)i - 1));
    ed->doc_count--;
    a3_zero_struct(&ed->docs[ed->doc_count]);
    if (ed->doc_active >= (i32)ed->doc_count) ed->doc_active = (i32)ed->doc_count - 1;
}

void ed_code_free(A3Editor *ed) {
    for (u32 i = 0; i < ed->doc_count; ++i) doc_free(&ed->docs[i]);
    ed->doc_count = 0;
    ed->doc_active = -1;
}

/* ======================================================================== */
/* Syntax highlighting                                                      */
/* ======================================================================== */

static const char *const kw_script[] = { "fn", "let", "var", "const", "if", "else", "while", "for", "in", "return", "break", "continue",
                                         "true", "false", "null", "and", "or", "not", "import", "self", "entity", "on", 0 };
static const char *const kw_glsl[] = { "if", "else", "for", "while", "do", "return", "break", "continue", "discard", "struct", "const",
                                       "uniform", "in", "out", "inout", "layout", "true", "false", "precision", "highp", "mediump", "lowp", 0 };
static const char *const ty_glsl[] = { "void", "float", "int", "uint", "bool", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "mat2", "mat3",
                                       "mat4", "sampler2D", "samplerCube", "A3Surface", 0 };
static const char *const ty_script[] = { "number", "string", "vec3", "bool", "Entity", "Transform", 0 };

static b32 word_in(const char *s, i32 n, const char *const *list) {
    for (u32 i = 0; list[i]; ++i) if ((i32)a3_strlen(list[i]) == n && a3_strncmp(list[i], s, (usize)n) == 0) return 1;
    return 0;
}

typedef struct Token { i32 start, len; u32 color; } Token;

/* Tokenizes one line. in_block tracks C block comments across lines. */
static u32 tokenize(A3Ui *ui, i32 lang, const char *s, i32 n, b32 *in_block, Token *out, u32 max) {
    u32 k = 0;
    u32 c_text = colr(ui, A3_UIC_TEXT), c_kw = a3_rgb(198, 120, 221), c_type = a3_rgb(86, 182, 194), c_str = a3_rgb(152, 195, 121),
        c_num = a3_rgb(209, 154, 102), c_com = a3_rgb(110, 128, 110), c_fn = a3_rgb(229, 192, 123), c_key = a3_rgb(224, 108, 117);
    i32 i = 0;
    while (i < n && k < max) {
        i32 st = i;
        u32 cc = c_text;
        if (*in_block) {
            while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) ++i;
            if (i < n) { i += 2; *in_block = 0; }
            cc = c_com;
        } else if (lang != LANG_TEXT && lang != LANG_JSON && s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            i = n; cc = c_com;
        } else if (lang != LANG_TEXT && lang != LANG_JSON && s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2; *in_block = 1;
            while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) ++i;
            if (i < n) { i += 2; *in_block = 0; }
            cc = c_com;
        } else if (lang != LANG_TEXT && (s[i] == '"' || (s[i] == '\'' && lang == LANG_SCRIPT))) {
            char q = s[i++];
            while (i < n && s[i] != q) { if (s[i] == '\\') ++i; ++i; }
            if (i < n) ++i;
            cc = c_str;
            if (lang == LANG_JSON) { i32 j = i; while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j; if (j < n && s[j] == ':') cc = c_key; }
        } else if (lang != LANG_TEXT && (a3_is_digit(s[i]) || (s[i] == '.' && i + 1 < n && a3_is_digit(s[i + 1])) || (s[i] == '-' && lang == LANG_JSON && i + 1 < n && a3_is_digit(s[i + 1])))) {
            ++i;
            while (i < n && (a3_is_alnum(s[i]) || s[i] == '.')) ++i;
            cc = c_num;
        } else if (lang != LANG_TEXT && (a3_is_alpha(s[i]) || s[i] == '_')) {
            while (i < n && a3_is_ident(s[i])) ++i;
            i32 len = i - st;
            if (lang == LANG_JSON) cc = word_in(s + st, len, (const char *const[]){ "true", "false", "null", 0 }) ? c_kw : c_text;
            else if (word_in(s + st, len, lang == LANG_GLSL ? kw_glsl : kw_script)) cc = c_kw;
            else if (word_in(s + st, len, lang == LANG_GLSL ? ty_glsl : ty_script)) cc = c_type;
            else { i32 j = i; while (j < n && s[j] == ' ') ++j; if (j < n && s[j] == '(') cc = c_fn; }
        } else {
            ++i;
            while (i < n && !a3_is_ident(s[i]) && s[i] != '"' && s[i] != '/' && s[i] != '\'' && !a3_is_digit(s[i])) ++i;
            cc = lang == LANG_TEXT ? c_text : colr(ui, A3_UIC_TEXT_DIM);
        }
        if (lang == LANG_TEXT) cc = c_text;
        out[k].start = st;
        out[k].len = i - st;
        out[k].color = cc;
        ++k;
    }
    return k;
}

/* ======================================================================== */
/* Find                                                                     */
/* ======================================================================== */

static i32 find_from(EdDocument *d, const char *needle, i32 from, b32 backwards) {
    i32 n = (i32)a3_strlen(needle);
    if (!n || n > (i32)d->len) return -1;
    if (!backwards) {
        for (i32 pass = 0; pass < 2; ++pass)
            for (i32 i = pass ? 0 : from; i + n <= (i32)d->len; ++i)
                if (a3_to_lower(d->text[i]) == a3_to_lower(needle[0])) {
                    i32 j = 1;
                    while (j < n && a3_to_lower(d->text[i + j]) == a3_to_lower(needle[j])) ++j;
                    if (j == n) return i;
                }
    } else {
        for (i32 pass = 0; pass < 2; ++pass)
            for (i32 i = pass ? (i32)d->len - n : from - 1; i >= 0; --i) {
                i32 j = 0;
                while (j < n && a3_to_lower(d->text[i + j]) == a3_to_lower(needle[j])) ++j;
                if (j == n) return i;
            }
    }
    return -1;
}

/* ======================================================================== */
/* Panel                                                                    */
/* ======================================================================== */

static void ensure_cursor_visible(EdDocument *d, f32 view_h, f32 view_w, f32 lh, f32 cw) {
    u32 line = line_of(d, d->cursor);
    f32 y = (f32)line * lh;
    if (y < d->scroll_y) d->scroll_y = y;
    if (y + lh > d->scroll_y + view_h) d->scroll_y = y + lh - view_h;
    f32 x = (f32)column_of(d, d->cursor) * cw;
    if (x < d->scroll_x) d->scroll_x = a3_maxf(x - cw * 4, 0);
    if (x > d->scroll_x + view_w - cw * 2) d->scroll_x = x - view_w + cw * 6;
}

static void handle_keys(A3Editor *ed, A3Ui *ui, EdDocument *d, i32 doc_index, f32 view_h, b32 *moved) {
    const A3InputState *in = a3_ui_input(ui);
    b32 ctrl = (in->mods & A3_MOD_CTRL) != 0, shift = (in->mods & A3_MOD_SHIFT) != 0;
    f64 now = a3_ui_time(ui);
    f32 lh = a3_font_line_height(A3_FONT_MONO);
    i32 page = a3_maxi((i32)(view_h / lh) - 1, 1);
    #define MOVE_TO(p) do { d->cursor = (p); if (!shift) d->anchor = d->cursor; *moved = 1; } while (0)
    if (ctrl && in->keys_pressed[A3_KEY_S]) ed_code_save(ed, doc_index);
    if (ctrl && in->keys_pressed[A3_KEY_F]) { ed->show_find = 1; if (has_sel(d) && sel_max(d) - sel_min(d) < 120) { a3_str_to_buf(a3_str_n(d->text + sel_min(d), (usize)(sel_max(d) - sel_min(d))), ed->find_text, sizeof(ed->find_text)); } }
    if (ctrl && in->keys_pressed[A3_KEY_A]) { d->anchor = 0; d->cursor = (i32)d->len; }
    if (ctrl && in->keys_repeat[A3_KEY_Z]) { if (shift) doc_redo(d); else doc_undo(d); *moved = 1; }
    if (ctrl && in->keys_repeat[A3_KEY_Y]) { doc_redo(d); *moved = 1; }
    if (ctrl && (in->keys_pressed[A3_KEY_C] || in->keys_pressed[A3_KEY_X]) && has_sel(d)) {
        i32 a = sel_min(d), b = sel_max(d);
        char *tmp = (char *)a3_malloc((usize)(b - a) + 1, A3_MEM_TEMP);
        if (tmp) { a3_memcpy(tmp, d->text + a, (usize)(b - a)); tmp[b - a] = 0; a3_ui_set_clipboard(ui, tmp); a3_free(tmp); }
        if (in->keys_pressed[A3_KEY_X]) { undo_checkpoint(d, EK_OTHER, now); delete_selection(d); *moved = 1; }
    }
    if (ctrl && in->keys_pressed[A3_KEY_V]) {
        const char *clip = a3_ui_get_clipboard(ui);
        usize n = a3_strlen(clip);
        if (n) {
            undo_checkpoint(d, EK_OTHER, now);
            char *tmp = (char *)a3_malloc(n + 1, A3_MEM_TEMP);
            if (tmp) {
                usize w = 0;
                for (usize r = 0; r < n; ++r) if (clip[r] != '\r') tmp[w++] = clip[r];
                replace_selection(d, tmp, (u32)w);
                a3_free(tmp);
            }
            *moved = 1;
        }
    }
    /* typing */
    if (!ctrl && in->text_count) {
        undo_checkpoint(d, EK_TYPE, now);
        for (u32 i = 0; i < in->text_count; ++i) {
            char buf[4];
            u32 n = a3_utf8_encode(in->text[i], buf);
            replace_selection(d, buf, n);
            /* auto-close brackets */
            if (in->text[i] == '{' || in->text[i] == '(' || in->text[i] == '[') {
                char close = in->text[i] == '{' ? '}' : (in->text[i] == '(' ? ')' : ']');
                doc_insert(d, d->cursor, &close, 1);
            }
        }
        *moved = 1;
    }
    if (in->keys_repeat[A3_KEY_ENTER]) {
        undo_checkpoint(d, EK_OTHER, now);
        /* keep the indentation of the current line; indent after '{' */
        i32 ls = line_start(d, line_of(d, d->cursor));
        char indent[128];
        u32 n = 0;
        indent[n++] = '\n';
        for (i32 i = ls; i < d->cursor && (d->text[i] == ' ' || d->text[i] == '\t') && n < 100; ++i) indent[n++] = d->text[i];
        b32 open_brace = d->cursor > 0 && d->text[d->cursor - 1] == '{';
        b32 close_next = open_brace && d->cursor < (i32)d->len && d->text[d->cursor] == '}';
        if (open_brace) for (int k = 0; k < 4; ++k) indent[n++] = ' ';
        replace_selection(d, indent, n);
        if (close_next) {
            i32 keep = d->cursor;
            char tail[128];
            u32 m = 0;
            tail[m++] = '\n';
            for (u32 k = 1; k + 4 < n; ++k) tail[m++] = indent[k];
            doc_insert(d, d->cursor, tail, m);
            d->cursor = d->anchor = keep;
        }
        *moved = 1;
    }
    if (in->keys_repeat[A3_KEY_TAB]) {
        undo_checkpoint(d, EK_OTHER, now);
        if (has_sel(d) || shift) {
            /* indent / unindent every selected line */
            u32 l0 = line_of(d, sel_min(d)), l1 = line_of(d, a3_maxi(sel_max(d) - 1, sel_min(d)));
            for (u32 l = l1 + 1; l-- > l0;) {
                i32 ls = line_start(d, l);
                if (shift) { i32 k = 0; while (k < 4 && d->text[ls + k] == ' ') ++k; doc_erase(d, ls, ls + k); }
                else doc_insert(d, ls, "    ", 4);
            }
            d->anchor = line_start(d, l0);
            d->cursor = line_end(d, l1);
        } else {
            i32 c = column_of(d, d->cursor);
            replace_selection(d, "    ", (u32)(4 - c % 4));
        }
        *moved = 1;
    }
    if (in->keys_repeat[A3_KEY_BACKSPACE]) {
        undo_checkpoint(d, EK_DELETE, now);
        if (has_sel(d)) delete_selection(d);
        else if (d->cursor > 0) {
            i32 p = prev_char(d, d->cursor);
            /* remove a whole indent step in leading whitespace */
            i32 ls = line_start(d, line_of(d, d->cursor));
            b32 lead = 1;
            for (i32 i = ls; i < d->cursor; ++i) if (d->text[i] != ' ') { lead = 0; break; }
            if (lead && d->cursor - ls >= 4 && !ctrl) p = d->cursor - ((d->cursor - ls) % 4 ? (d->cursor - ls) % 4 : 4);
            if (ctrl) { while (p > 0 && !a3_is_ident(d->text[p - 1]) && d->text[p - 1] != '\n') --p; while (p > 0 && a3_is_ident(d->text[p - 1])) --p; }
            doc_erase(d, p, d->cursor);
            d->cursor = d->anchor = p;
        }
        *moved = 1;
    }
    if (in->keys_repeat[A3_KEY_DELETE]) {
        undo_checkpoint(d, EK_DELETE, now);
        if (has_sel(d)) delete_selection(d);
        else doc_erase(d, d->cursor, next_char(d, d->cursor));
        *moved = 1;
    }
    /* navigation */
    u32 line = line_of(d, d->cursor);
    if (in->keys_repeat[A3_KEY_LEFT]) {
        if (has_sel(d) && !shift) MOVE_TO(sel_min(d));
        else if (ctrl) { i32 p = d->cursor; while (p > 0 && !a3_is_ident(d->text[p - 1])) --p; while (p > 0 && a3_is_ident(d->text[p - 1])) --p; MOVE_TO(p); }
        else MOVE_TO(prev_char(d, d->cursor));
        d->desired_x = -1;
    }
    if (in->keys_repeat[A3_KEY_RIGHT]) {
        if (has_sel(d) && !shift) MOVE_TO(sel_max(d));
        else if (ctrl) { i32 p = d->cursor; while (p < (i32)d->len && !a3_is_ident(d->text[p])) ++p; while (p < (i32)d->len && a3_is_ident(d->text[p])) ++p; MOVE_TO(p); }
        else MOVE_TO(next_char(d, d->cursor));
        d->desired_x = -1;
    }
    if (in->keys_repeat[A3_KEY_UP] || in->keys_repeat[A3_KEY_DOWN] || in->keys_repeat[A3_KEY_PAGE_UP] || in->keys_repeat[A3_KEY_PAGE_DOWN]) {
        if (d->desired_x < 0) d->desired_x = (f32)column_of(d, d->cursor);
        i32 dl = in->keys_repeat[A3_KEY_UP] ? -1 : in->keys_repeat[A3_KEY_DOWN] ? 1 : in->keys_repeat[A3_KEY_PAGE_UP] ? -page : page;
        i32 nl = a3_clampi((i32)line + dl, 0, (i32)d->line_count - 1);
        f32 dx = d->desired_x;
        MOVE_TO(pos_at_column(d, (u32)nl, (i32)dx));
        d->desired_x = dx;
    }
    if (in->keys_repeat[A3_KEY_HOME]) {
        if (ctrl) MOVE_TO(0);
        else {
            /* smart home: first non-blank, then column 0 */
            i32 ls = line_start(d, line), p = ls;
            while (p < line_end(d, line) && (d->text[p] == ' ' || d->text[p] == '\t')) ++p;
            MOVE_TO(d->cursor == p ? ls : p);
        }
        d->desired_x = -1;
    }
    if (in->keys_repeat[A3_KEY_END]) { MOVE_TO(ctrl ? (i32)d->len : line_end(d, line)); d->desired_x = -1; }
    if (in->keys_pressed[A3_KEY_ESCAPE]) { d->anchor = d->cursor; ed->show_find = 0; }
    #undef MOVE_TO
}

static void find_bar(A3Editor *ed, A3Ui *ui, EdDocument *d) {
    a3_ui_input_text(ui, "find", ed->find_text, sizeof(ed->find_text), A3_INPUT_SEARCH, "Find...");
    a3_ui_same_line(ui);
    b32 next = a3_ui_button_ex(ui, "Next", 60, A3_BUTTON_SMALL), prev = 0;
    a3_ui_same_line(ui);
    prev = a3_ui_button_ex(ui, "Prev", 60, A3_BUTTON_SMALL);
    a3_ui_same_line(ui);
    a3_ui_input_text(ui, "replace", ed->replace_text, sizeof(ed->replace_text), 0, "Replace with...");
    a3_ui_same_line(ui);
    b32 rep = a3_ui_button_ex(ui, "Replace", 80, A3_BUTTON_SMALL);
    a3_ui_same_line(ui);
    b32 rep_all = a3_ui_button_ex(ui, "All", 50, A3_BUTTON_SMALL);
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_CROSS, "Close (Esc)", A3_BUTTON_FLAT | A3_BUTTON_SMALL)) ed->show_find = 0;
    i32 n = (i32)a3_strlen(ed->find_text);
    if (!n) return;
    f64 now = a3_ui_time(ui);
    if (rep && has_sel(d) && sel_max(d) - sel_min(d) == n && find_from(d, ed->find_text, sel_min(d), 0) == sel_min(d)) {
        undo_checkpoint(d, EK_OTHER, now);
        replace_selection(d, ed->replace_text, (u32)a3_strlen(ed->replace_text));
        next = 1;
    }
    if (rep_all) {
        undo_checkpoint(d, EK_OTHER, now);
        u32 count = 0;
        i32 p = 0, rn = (i32)a3_strlen(ed->replace_text);
        for (;;) {
            i32 f = -1;
            for (i32 i = p; i + n <= (i32)d->len; ++i) {
                i32 j = 0;
                while (j < n && a3_to_lower(d->text[i + j]) == a3_to_lower(ed->find_text[j])) ++j;
                if (j == n) { f = i; break; }
            }
            if (f < 0 || count > 100000) break;
            doc_erase(d, f, f + n);
            doc_insert(d, f, ed->replace_text, (u32)rn);
            p = f + rn;
            ++count;
        }
        d->cursor = d->anchor = a3_mini(d->cursor, (i32)d->len);
        a3_ui_notify(ui, 0, "Replaced %u occurrence%s", count, count == 1 ? "" : "s");
    }
    if (next || prev) {
        i32 f = find_from(d, ed->find_text, prev ? sel_min(d) : sel_max(d), prev);
        if (f >= 0) { d->anchor = f; d->cursor = f + n; }
        else a3_ui_notify(ui, colr(ui, A3_UIC_WARNING), "'%s' not found", ed->find_text);
    }
}

void ed_code_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    if (!ed->has_project) return;
    /* tabs */
    i32 close_idx = -1;
    for (u32 i = 0; i < ed->doc_count; ++i) {
        char label[160];
        a3_snprintf(label, sizeof(label), "%s%s", a3_path_filename(ed->docs[i].path), ed->docs[i].dirty ? " *" : "");
        a3_ui_push_id_int(ui, (i64)i);
        b32 close = 0;
        if (a3_ui_tab_bar_item(ui, "tab", label, ed->doc_active == (i32)i, &close)) ed->doc_active = (i32)i;
        if (close) close_idx = (i32)i;
        a3_ui_tooltip(ui, ed->docs[i].path);
        a3_ui_pop_id(ui);
        a3_ui_same_line(ui);
    }
    if (a3_ui_icon_button(ui, A3_ICON_PLUS, "New script", A3_BUTTON_FLAT | A3_BUTTON_SMALL)) {
        char rel[ED_PATH], abs[ED_PATH];
        for (int k = 1; k < 100; ++k) {
            a3_snprintf(rel, sizeof(rel), "Assets/Scripts/Script%d.a3script", k);
            ed_project_path(ed, rel, abs, sizeof(abs));
            if (!a3_file_exists(abs)) break;
        }
        i32 di = ed_code_open(ed, rel);
        if (di >= 0) doc_set_text(&ed->docs[di], "// ASM3D script\nfn on_start() {\n}\n\nfn on_update(dt) {\n}\n");
    }
    if (close_idx >= 0) {
        if (ed->docs[close_idx].dirty) ed_code_save(ed, close_idx); /* never lose typed code */
        close_doc(ed, close_idx);
    }
    if (ed->doc_active < 0 || ed->doc_active >= (i32)ed->doc_count) {
        a3_ui_spacing(ui, 20);
        a3_ui_label_colored(ui, colr(ui, A3_UIC_TEXT_DIM), "No file open.");
        a3_ui_label_wrapped(ui, "Double-click a script, shader or text file in the Assets panel, or press + to create a new script.");
        return;
    }
    i32 di = ed->doc_active;
    EdDocument *d = &ed->docs[di];
    doc_lines(d);
    /* toolbar */
    if (a3_ui_button_ex(ui, "Save", 60, A3_BUTTON_SMALL | (d->dirty ? A3_BUTTON_PRIMARY : 0))) ed_code_save(ed, di);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Find", 60, A3_BUTTON_SMALL | (ed->show_find ? A3_BUTTON_TOGGLED : 0))) ed->show_find = !ed->show_find;
    a3_ui_same_line(ui);
    static const char *const langs[] = { "Plain Text", "ASM3D Script", "GLSL", "JSON" };
    a3_ui_label_colored(ui, colr(ui, A3_UIC_TEXT_DIM), "%s   Ln %u, Col %d   %u lines%s", langs[d->language], line_of(d, d->cursor) + 1, column_of(d, d->cursor) + 1,
                        d->line_count, d->error_count ? "   (errors)" : "");
    if (ed->show_find) find_bar(ed, ui, d);
    A3Vec2 cp = a3_ui_cursor_pos(ui);
    A3Rect area = a3_rect(r.x, cp.y, r.w, r.y + r.h - cp.y);
    if (area.h < 20) return;
    A3UiTheme *th = a3_ui_theme(ui);
    const A3InputState *in = a3_ui_input(ui);
    f32 lh = a3_font_line_height(A3_FONT_MONO), cw = a3_font_text_width(A3_FONT_MONO, "M", 1);
    A3Rect text_r = a3_rect(area.x + GUTTER_W, area.y, area.w - GUTTER_W, area.h);
    /* interaction */
    char idbuf[64];
    a3_snprintf(idbuf, sizeof(idbuf), "code_area_%s", d->path);
    b32 clicked = a3_ui_invisible_button(ui, idbuf, area);
    A3UiId id = a3_ui_last_id(ui);
    b32 hovered = a3_ui_item_hovered(ui);
    if (hovered) {
        /* I-beam cursor + wheel scrolling */
        if (in->scroll.y != 0) d->scroll_y -= in->scroll.y * lh * 3;
        if (in->scroll.x != 0) d->scroll_x -= in->scroll.x * cw * 6;
    }
    b32 moved = 0;
    if (clicked || (hovered && in->mouse_pressed[A3_MOUSE_LEFT])) {
        a3_ui_claim_keyboard(ui, id);
        d->mouse_selecting = 1;
    }
    if (!in->mouse[A3_MOUSE_LEFT]) d->mouse_selecting = 0;
    if (d->mouse_selecting) {
        f32 my = in->mouse_pos.y - text_r.y + d->scroll_y, mx = in->mouse_pos.x - text_r.x - 6 + d->scroll_x;
        i32 ln = a3_clampi((i32)(my / lh), 0, (i32)d->line_count - 1);
        i32 p = pos_at_column(d, (u32)ln, (i32)(mx / cw + 0.5f));
        d->cursor = p;
        if (in->mouse_pressed[A3_MOUSE_LEFT] && !(in->mods & A3_MOD_SHIFT)) d->anchor = p;
        if (in->mouse_double_click[A3_MOUSE_LEFT]) {
            i32 a = p, b = p;
            while (a > 0 && a3_is_ident(d->text[a - 1])) --a;
            while (b < (i32)d->len && a3_is_ident(d->text[b])) ++b;
            d->anchor = a;
            d->cursor = b;
            d->mouse_selecting = 0;
        }
        d->desired_x = -1;
        /* auto scroll while dragging outside */
        if (in->mouse_pos.y < text_r.y) d->scroll_y -= lh;
        if (in->mouse_pos.y > text_r.y + text_r.h) d->scroll_y += lh;
    }
    b32 focused = a3_ui_has_keyboard(ui, id);
    if (focused) handle_keys(ed, ui, d, di, text_r.h, &moved);
    doc_lines(d);
    if (moved) ensure_cursor_visible(d, text_r.h, text_r.w - 12, lh, cw);
    f32 max_scroll = a3_maxf((f32)d->line_count * lh - text_r.h + lh * 3, 0);
    d->scroll_y = a3_clampf(d->scroll_y, 0, max_scroll);
    d->scroll_x = a3_maxf(d->scroll_x, 0);
    /* draw */
    a3_ui_rect(ui, area, th->colors[A3_UIC_BG], 0);
    a3_ui_rect(ui, a3_rect(area.x, area.y, GUTTER_W, area.h), th->colors[A3_UIC_PANEL_ALT], 0);
    a3_ui_push_clip(ui, area);
    u32 first = (u32)(d->scroll_y / lh), last = a3_minu(d->line_count, first + (u32)(area.h / lh) + 2);
    /* block comment state at the first visible line */
    b32 in_block = 0;
    if (d->language == LANG_SCRIPT || d->language == LANG_GLSL) {
        i32 end = line_start(d, first);
        for (i32 i = 0; i + 1 < end; ++i) {
            if (!in_block && d->text[i] == '/' && d->text[i + 1] == '/') { while (i < end && d->text[i] != '\n') ++i; continue; }
            if (!in_block && d->text[i] == '"') { ++i; while (i < end && d->text[i] != '"' && d->text[i] != '\n') ++i; continue; }
            if (!in_block && d->text[i] == '/' && d->text[i + 1] == '*') { in_block = 1; ++i; continue; }
            if (in_block && d->text[i] == '*' && d->text[i + 1] == '/') { in_block = 0; ++i; }
        }
    }
    u32 cur_line = line_of(d, d->cursor);
    i32 s0 = sel_min(d), s1 = sel_max(d);
    i32 find_n = ed->show_find ? (i32)a3_strlen(ed->find_text) : 0;
    Token toks[256];
    for (u32 l = first; l < last; ++l) {
        f32 y = area.y + (f32)l * lh - d->scroll_y;
        i32 ls = line_start(d, l), le = line_end(d, l);
        if (l == cur_line && focused) a3_ui_rect(ui, a3_rect(text_r.x, y, text_r.w, lh), a3_color_alpha(th->colors[A3_UIC_WIDGET], 0.6f), 0);
        /* selection */
        if (s0 != s1 && s1 >= ls && s0 <= le) {
            i32 a = a3_maxi(s0, ls), b = a3_mini(s1, le);
            f32 xa = text_r.x + 6 + (f32)column_of(d, a) * cw - d->scroll_x, xb = text_r.x + 6 + (f32)column_of(d, b) * cw - d->scroll_x;
            if (s1 > le) xb += cw * 0.5f;
            a3_ui_rect(ui, a3_rect(xa, y, a3_maxf(xb - xa, 2), lh), a3_color_alpha(th->colors[A3_UIC_ACCENT], 0.35f), 2);
        }
        /* find matches */
        if (find_n > 0) {
            for (i32 i = ls; i + find_n <= le; ++i) {
                i32 j = 0;
                while (j < find_n && a3_to_lower(d->text[i + j]) == a3_to_lower(ed->find_text[j])) ++j;
                if (j == find_n) {
                    f32 xa = text_r.x + 6 + (f32)column_of(d, i) * cw - d->scroll_x;
                    a3_ui_rect_outline(ui, a3_rect(xa, y, (f32)find_n * cw, lh), th->colors[A3_UIC_WARNING], 2, 1);
                    i += find_n - 1;
                }
            }
        }
        /* gutter */
        char num[16];
        a3_snprintf(num, sizeof(num), "%u", l + 1);
        b32 err = 0;
        const char *msg = 0;
        for (u32 k = 0; k < d->error_count; ++k) if (d->error_lines[k] == (i32)l + 1) { err = 1; msg = d->error_msgs[k]; }
        a3_ui_text_in_rect(ui, A3_FONT_MONO, a3_rect(area.x, y, GUTTER_W - 10, lh), A3_ALIGN_RIGHT, l == cur_line ? th->colors[A3_UIC_TEXT] : th->colors[A3_UIC_TEXT_DISABLED], num);
        if (err) {
            a3_ui_circle(ui, a3_v2(area.x + 8, y + lh * 0.5f), 4, th->colors[A3_UIC_ERROR]);
            a3_ui_rect(ui, a3_rect(text_r.x, y + lh - 2, text_r.w, 2), a3_color_alpha(th->colors[A3_UIC_ERROR], 0.6f), 0);
            if (a3_rect_contains(a3_rect(area.x, y, area.w, lh), in->mouse_pos) && msg) a3_ui_tooltip_now(ui, msg);
        }
        /* text */
        u32 nt = tokenize(ui, d->language, d->text + ls, le - ls, &in_block, toks, A3_ARRAY_COUNT(toks));
        a3_ui_push_clip(ui, text_r);
        for (u32 t = 0; t < nt; ++t) {
            i32 a = ls + toks[t].start;
            f32 x = text_r.x + 6 + (f32)column_of(d, a) * cw - d->scroll_x;
            if (x > text_r.x + text_r.w) break;
            /* tabs are drawn as spaces */
            const char *seg = d->text + a;
            i32 n = toks[t].len, st = 0;
            for (i32 k = 0; k <= n; ++k) {
                if (k == n || seg[k] == '\t') {
                    if (k > st) a3_ui_text_n(ui, A3_FONT_MONO, a3_v2(text_r.x + 6 + (f32)column_of(d, a + st) * cw - d->scroll_x, y), toks[t].color, seg + st, k - st);
                    st = k + 1;
                }
            }
        }
        a3_ui_pop_clip(ui);
    }
    /* caret */
    if (focused && a3_fmodf((f32)a3_ui_time(ui), 1.0f) < 0.6f) {
        f32 x = text_r.x + 6 + (f32)column_of(d, d->cursor) * cw - d->scroll_x, y = area.y + (f32)cur_line * lh - d->scroll_y;
        a3_ui_rect(ui, a3_rect(x - 1, y + 1, 2, lh - 2), th->colors[A3_UIC_ACCENT], 0);
    }
    a3_ui_pop_clip(ui);
    if (!focused) a3_ui_rect_outline(ui, area, th->colors[A3_UIC_BORDER], 0, 1);
    else a3_ui_rect_outline(ui, area, a3_color_alpha(th->colors[A3_UIC_ACCENT], 0.6f), 0, 1);
}
