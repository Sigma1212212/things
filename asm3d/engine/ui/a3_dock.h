/*
 * ASM3D - a3_dock.h
 * Docking layout: a binary tree of splits whose leaves hold tab stacks of
 * panels. Tabs can be dragged onto other leaves (as a tab) or onto an edge
 * of a leaf (to split it). Layouts serialize to JSON so users can save and
 * switch workspaces (Beginner, Programming, Level Design, ...).
 */
#ifndef A3_DOCK_H
#define A3_DOCK_H

#include "a3_ui.h"
#include "../core/a3_strbuf.h"

A3_EXTERN_C_BEGIN

#define A3_DOCK_MAX_PANELS 48
#define A3_DOCK_MAX_NODES 96
#define A3_DOCK_MAX_TABS 12

typedef void (*A3DockDrawFn)(void *user, A3Ui *ui, A3Rect content);

typedef struct A3DockPanel {
    char name[48];
    u32 icon;
    A3DockDrawFn draw;
    void *user;
    b32 open;
    u32 flags;          /* A3PanelFlags for the content panel */
} A3DockPanel;

typedef struct A3DockNode {
    b32 used;
    i32 parent;
    i32 child[2];       /* -1 for leaves */
    b32 vertical;       /* true: children side by side (vertical divider) */
    f32 ratio;          /* size of child[0] relative to the node */
    i32 tabs[A3_DOCK_MAX_TABS];
    u32 tab_count;
    i32 active;         /* index into tabs */
    A3Rect rect;        /* last layout rect */
} A3DockNode;

typedef struct A3Dock {
    A3DockPanel panels[A3_DOCK_MAX_PANELS];
    u32 panel_count;
    A3DockNode nodes[A3_DOCK_MAX_NODES];
    i32 root;
    i32 drag_panel;     /* panel being dragged (-1 none) */
    i32 focused_panel;
} A3Dock;

void a3_dock_init(A3Dock *d);
i32  a3_dock_add_panel(A3Dock *d, const char *name, u32 icon, A3DockDrawFn draw, void *user, u32 content_flags);
i32  a3_dock_find_panel(A3Dock *d, const char *name);
/* Layout building */
void a3_dock_reset(A3Dock *d);                 /* single empty root leaf */
i32  a3_dock_root(A3Dock *d);
/* Splits leaf `node`; returns the new leaf on the given side (right/bottom when second = true). */
i32  a3_dock_split(A3Dock *d, i32 node, b32 vertical, f32 ratio, b32 second);
void a3_dock_add_tab(A3Dock *d, i32 node, i32 panel);
void a3_dock_close_panel(A3Dock *d, i32 panel);
/* Opens (if closed) and activates a panel. */
void a3_dock_show(A3Dock *d, const char *name);
b32  a3_dock_is_visible(A3Dock *d, const char *name);

void a3_dock_draw(A3Dock *d, A3Ui *ui, A3Rect area);

void a3_dock_save(A3Dock *d, A3StrBuf *out);
b32  a3_dock_load(A3Dock *d, const char *json, usize len);

A3_EXTERN_C_END

#endif
