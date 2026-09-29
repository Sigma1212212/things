/*
 * ASM3D - a3_project.h
 * Project folders: reading project.a3proj, checking a project for problems
 * and packaging a desktop build. Shared by the editor and asm3d_cli.
 */
#ifndef A3_PROJECT_H
#define A3_PROJECT_H

#include "../core/a3_base.h"
#include "../core/a3_memory.h"
#include "../ecs/a3_reflect.h"
#include "../core/a3_strbuf.h"

A3_EXTERN_C_BEGIN

#if A3_PLATFORM_WINDOWS
#  define A3_EXE_SUFFIX ".exe"
#else
#  define A3_EXE_SUFFIX ""
#endif

typedef struct A3ProjectInfo {
    char name[A3_NAME_MAX];
    char startup_scene[A3_PATH_MAX];
    char template_name[A3_NAME_MAX];
    char engine[32];
    i32 width, height;
} A3ProjectInfo;

/* Reads <dir>/project.a3proj. False (with defaults filled) when missing or broken. */
b32 a3_project_read(const char *dir, A3ProjectInfo *out);
/* Writes a fresh project.a3proj (used when creating projects). */
b32 a3_project_write(const char *dir, const A3ProjectInfo *info);

typedef struct A3ProjectIssue {
    b32 error;                 /* false = warning */
    char file[A3_PATH_MAX];    /* project-relative, may be empty */
    i32 line;                  /* scripts: line of the problem, else 0 */
    char text[400];
} A3ProjectIssue;

typedef struct A3ProjectReport {
    A3_ARRAY_TYPE(A3ProjectIssue) issues;
    u32 errors, warnings;
    u32 files, scripts, scenes;
} A3ProjectReport;

/* Checks the startup scene (or `scene_rel` when given), every scene's asset
 * references, and compiles every script. */
void a3_project_validate(const char *dir, const char *scene_rel, A3ProjectReport *r);
void a3_project_report_free(A3ProjectReport *r);
/* "ERROR: ..." / "Warning: ..." lines plus a summary line. */
void a3_project_report_text(const A3ProjectReport *r, A3StrBuf *out);

typedef void (*A3ProjectLogFn)(void *user, const char *line);
/* Packages <out_dir>/<name>[.exe] + data/ (project.a3proj and Assets/) + README.txt.
 * `out_dir` NULL = <dir>/Builds/Desktop-<config>/<name>. The final folder is
 * written to out_path. Refuses to build when validation finds errors. */
b32 a3_project_build(const char *dir, const char *player_exe, const char *out_dir, const char *config,
                     A3ProjectLogFn log, void *user, char *out_path, usize out_cap);
/* asm3d_player next to the running program (or on PATH). */
b32 a3_project_find_player(char *out, usize cap);

A3_EXTERN_C_END

#endif
