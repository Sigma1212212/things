/*
 * ASM3D Editor - undo / redo.
 *
 * Every change is recorded as before/after snapshots of the affected entity
 * subtree in the scene JSON format (GUIDs preserved), so undo works for any
 * component - including custom and plugin components - without per-field
 * code. Continuous edits (dragging a value or a gizmo) are merged into a
 * single step that is committed when the mouse is released.
 */
#include "editor.h"
#include "../../engine/scene/a3_scene_io.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_json.h"

#define UNDO_LIMIT 200

void ed_undo_init(EdUndo *u) { a3_zero_struct(u); }

static void free_entry(EdUndoEntry *e) { a3_free(e->before); a3_free(e->after); e->before = e->after = 0; }

void ed_undo_clear(EdUndo *u) {
    for (u32 i = 0; i < u->entries.count; ++i) free_entry(&u->entries.data[i]);
    a3_array_clear(u->entries);
    u->cursor = 0;
    a3_free(u->pending_before);
    u->pending_before = 0;
    u->pending_guid = 0;
    u->pending_changed = 0;
}

void ed_undo_free(EdUndo *u) {
    ed_undo_clear(u);
    a3_array_free(u->entries);
}

char *ed_snapshot(A3World *w, u64 guid) {
    A3Entity e = a3_entity_find_by_guid(w, guid);
    if (!a3_entity_valid(w, e)) return 0;
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_EDITOR);
    if (a3_scene_save_subtree_json(w, e, &sb) != A3_OK) { a3_strbuf_free(&sb); return 0; }
    return sb.data; /* ownership transferred */
}

static void destroy_by_guid(A3World *w, u64 guid) {
    A3Entity e = a3_entity_find_by_guid(w, guid);
    if (a3_entity_valid(w, e)) a3_entity_destroy(w, e);
}

b32 ed_restore_snapshot(A3World *w, u64 guid, const char *json) {
    destroy_by_guid(w, guid);
    if (!json) return 1;
    A3SceneLoadReport rep;
    a3_log_set_console(0);
    A3Result r = a3_scene_load_json(w, json, a3_strlen(json), &rep);
    a3_log_set_console(1);
    if (r != A3_OK) { A3_ERROR("undo", "could not restore state: %s", rep.error); return 0; }
    /* restore sibling order */
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, 1024);
    A3Json *root = a3_json_parse(json, a3_strlen(json), &ar, 0);
    u32 index = (u32)a3_json_get_number(root, "siblingIndex", 0);
    a3_arena_release(&ar);
    A3Entity e = a3_entity_find_by_guid(w, guid);
    if (a3_entity_valid(w, e)) a3_entity_set_sibling_index(w, e, index);
    return 1;
}

void ed_undo_push(A3Editor *ed, EdUndoKind kind, const char *label, u64 guid, char *before, char *after) {
    EdUndo *u = &ed->undo;
    /* drop redo history */
    for (u32 i = u->cursor; i < u->entries.count; ++i) free_entry(&u->entries.data[i]);
    u->entries.count = u->cursor;
    if (u->entries.count >= UNDO_LIMIT) {
        free_entry(&u->entries.data[0]);
        a3_memmove(u->entries.data, u->entries.data + 1, sizeof(EdUndoEntry) * (u->entries.count - 1));
        u->entries.count--;
    }
    EdUndoEntry e;
    a3_zero_struct(&e);
    e.kind = kind;
    a3_strcpy(e.label, sizeof(e.label), label ? label : "Edit");
    e.guid = guid;
    e.before = before;
    e.after = after;
    if (!a3_array_push(u->entries, e, A3_MEM_EDITOR)) { free_entry(&e); return; }
    u->cursor = u->entries.count;
    ed_mark_dirty(ed);
}

/* ---- continuous edit capture ---- */

void ed_undo_begin_frame(A3Editor *ed) {
    EdUndo *u = &ed->undo;
    if (ed->mode != ED_EDIT || !ed->world) return;
    if (u->pending_changed) return; /* keep the "before" of the edit in progress */
    if (u->pending_guid != ed->selected || !u->pending_before || (ed->frame % 2) == 0) {
        a3_free(u->pending_before);
        u->pending_before = ed->selected ? ed_snapshot(ed->world, ed->selected) : 0;
        u->pending_guid = ed->selected;
    }
}

void ed_undo_mark_changed(A3Editor *ed, const char *label) {
    EdUndo *u = &ed->undo;
    if (ed->mode != ED_EDIT) return;
    if (!u->pending_changed) a3_strcpy(u->pending_label, sizeof(u->pending_label), label ? label : "Edit");
    u->pending_changed = 1;
    ed->dirty = 1;
}

void ed_undo_end_frame(A3Editor *ed) {
    EdUndo *u = &ed->undo;
    if (ed->pending_create) {
        /* recorded at frame end so setup done right after creation is part of the step */
        char *after = ed->mode == ED_EDIT ? ed_snapshot(ed->world, ed->pending_create) : 0;
        if (after) ed_undo_push(ed, ED_UNDO_CREATE, "Create", ed->pending_create, 0, after);
        ed->pending_create = 0;
        a3_free(u->pending_before);
        u->pending_before = 0;
    }
    if (!u->pending_changed) return;
    const A3InputState *in = a3_ui_input(ed->ui);
    b32 still_editing = in->mouse[A3_MOUSE_LEFT] || a3_ui_wants_keyboard(ed->ui);
    if (still_editing) return;
    char *after = ed_snapshot(ed->world, u->pending_guid);
    if (u->pending_before && after && a3_strcmp(u->pending_before, after) != 0) {
        ed_undo_push(ed, ED_UNDO_MODIFY, u->pending_label, u->pending_guid, u->pending_before, after);
        u->pending_before = 0;
    } else {
        a3_free(after);
    }
    u->pending_changed = 0;
}

b32 ed_undo(A3Editor *ed) {
    EdUndo *u = &ed->undo;
    if (ed->mode != ED_EDIT || u->cursor == 0) return 0;
    EdUndoEntry *e = &u->entries.data[--u->cursor];
    switch (e->kind) {
    case ED_UNDO_MODIFY: ed_restore_snapshot(ed->world, e->guid, e->before); break;
    case ED_UNDO_CREATE: destroy_by_guid(ed->world, e->guid); if (ed->selected == e->guid) ed->selected = 0; break;
    case ED_UNDO_DELETE: ed_restore_snapshot(ed->world, e->guid, e->before); ed->selected = e->guid; break;
    }
    a3_free(u->pending_before);
    u->pending_before = 0;
    ed->dirty = 1;
    a3_ui_notify(ed->ui, 0, "Undo: %s", e->label);
    return 1;
}

b32 ed_redo(A3Editor *ed) {
    EdUndo *u = &ed->undo;
    if (ed->mode != ED_EDIT || u->cursor >= u->entries.count) return 0;
    EdUndoEntry *e = &u->entries.data[u->cursor++];
    switch (e->kind) {
    case ED_UNDO_MODIFY: ed_restore_snapshot(ed->world, e->guid, e->after); break;
    case ED_UNDO_CREATE: ed_restore_snapshot(ed->world, e->guid, e->after); ed->selected = e->guid; break;
    case ED_UNDO_DELETE: destroy_by_guid(ed->world, e->guid); if (ed->selected == e->guid) ed->selected = 0; break;
    }
    a3_free(u->pending_before);
    u->pending_before = 0;
    ed->dirty = 1;
    a3_ui_notify(ed->ui, 0, "Redo: %s", e->label);
    return 1;
}
