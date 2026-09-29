/*
 * ASM3D - a3_gl.h
 * Self-contained OpenGL 3.3 core loader (no system GL headers required).
 * Function pointers are resolved at runtime through the window layer, so the
 * engine binary does not hard-link against a GL driver.
 */
#ifndef A3_GL_H
#define A3_GL_H

#include "../core/a3_base.h"

A3_EXTERN_C_BEGIN

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef uint64_t GLuint64;
typedef int64_t GLint64;

#if defined(_WIN32)
#  define A3_GLAPI __stdcall
#else
#  define A3_GLAPI
#endif

/* ---- constants ---- */
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_ZERO 0
#define GL_ONE 1
#define GL_NONE 0
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_ALWAYS 0x0207
#define GL_SRC_COLOR 0x0300
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_RGBA8 0x8058
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32 0x81A7
#define GL_R8 0x8229
#define GL_RG8 0x822B
#define GL_R16F 0x822D
#define GL_R32F 0x822E
#define GL_RG16F 0x822F
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#define GL_RGBA32F 0x8814
#define GL_RGBA16F 0x881A
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_TIME_ELAPSED 0x88BF
#define GL_TIMESTAMP 0x8E28
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_NUM_EXTENSIONS 0x821D
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE

/* ---- function list: X(return_type, name, (params)) ---- */
#define A3_GL_FUNCTIONS(X) \
    X(GLenum, glGetError, (void)) \
    X(const GLubyte *, glGetString, (GLenum name)) \
    X(const GLubyte *, glGetStringi, (GLenum name, GLuint index)) \
    X(void, glGetIntegerv, (GLenum pname, GLint *data)) \
    X(void, glEnable, (GLenum cap)) \
    X(void, glDisable, (GLenum cap)) \
    X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor)) \
    X(void, glBlendFuncSeparate, (GLenum sr, GLenum dr, GLenum sa, GLenum da)) \
    X(void, glDepthFunc, (GLenum func)) \
    X(void, glDepthMask, (GLboolean flag)) \
    X(void, glCullFace, (GLenum mode)) \
    X(void, glFrontFace, (GLenum mode)) \
    X(void, glPolygonMode, (GLenum face, GLenum mode)) \
    X(void, glPolygonOffset, (GLfloat factor, GLfloat units)) \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a)) \
    X(void, glClear, (GLbitfield mask)) \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, glClearDepth, (GLdouble depth)) \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, glPixelStorei, (GLenum pname, GLint param)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void)) \
    X(void, glLineWidth, (GLfloat width)) \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers)) \
    X(void, glBindBuffer, (GLenum target, GLuint buffer)) \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data)) \
    X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint buffer)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays)) \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays)) \
    X(void, glBindVertexArray, (GLuint array)) \
    X(void, glEnableVertexAttribArray, (GLuint index)) \
    X(void, glDisableVertexAttribArray, (GLuint index)) \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
    X(void, glVertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer)) \
    X(void, glVertexAttribDivisor, (GLuint index, GLuint divisor)) \
    X(void, glGenTextures, (GLsizei n, GLuint *textures)) \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *textures)) \
    X(void, glBindTexture, (GLenum target, GLuint texture)) \
    X(void, glActiveTexture, (GLenum texture)) \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, glTexParameterf, (GLenum target, GLenum pname, GLfloat param)) \
    X(void, glTexParameterfv, (GLenum target, GLenum pname, const GLfloat *params)) \
    X(void, glGenerateMipmap, (GLenum target)) \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glDeleteShader, (GLuint shader)) \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)) \
    X(void, glCompileShader, (GLuint shader)) \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params)) \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glDeleteProgram, (GLuint program)) \
    X(void, glAttachShader, (GLuint program, GLuint shader)) \
    X(void, glDetachShader, (GLuint program, GLuint shader)) \
    X(void, glLinkProgram, (GLuint program)) \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params)) \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
    X(void, glUseProgram, (GLuint program)) \
    X(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar *name)) \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint program, const GLchar *name)) \
    X(void, glUniformBlockBinding, (GLuint program, GLuint index, GLuint binding)) \
    X(void, glUniform1i, (GLint location, GLint v0)) \
    X(void, glUniform1f, (GLint location, GLfloat v0)) \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1)) \
    X(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2)) \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
    X(void, glUniform1fv, (GLint location, GLsizei count, const GLfloat *value)) \
    X(void, glUniform2fv, (GLint location, GLsizei count, const GLfloat *value)) \
    X(void, glUniform3fv, (GLint location, GLsizei count, const GLfloat *value)) \
    X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat *value)) \
    X(void, glUniform1iv, (GLint location, GLsizei count, const GLint *value)) \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer)) \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
    X(void, glDrawBuffers, (GLsizei n, const GLenum *bufs)) \
    X(void, glReadBuffer, (GLenum src)) \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)) \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices)) \
    X(void, glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instancecount)) \
    X(void, glDrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount)) \
    X(void, glGenQueries, (GLsizei n, GLuint *ids)) \
    X(void, glDeleteQueries, (GLsizei n, const GLuint *ids)) \
    X(void, glQueryCounter, (GLuint id, GLenum target)) \
    X(void, glBeginQuery, (GLenum target, GLuint id)) \
    X(void, glEndQuery, (GLenum target)) \
    X(void, glGetQueryObjectiv, (GLuint id, GLenum pname, GLint *params)) \
    X(void, glGetQueryObjectui64v, (GLuint id, GLenum pname, GLuint64 *params))

#define A3_GL_DECLARE(ret, name, params) typedef ret (A3_GLAPI *PFN_##name) params; extern PFN_##name a3_##name;
A3_GL_FUNCTIONS(A3_GL_DECLARE)
#undef A3_GL_DECLARE

/* Engine code calls GL through these macros so the names read naturally. */
#define glGetError a3_glGetError
#define glGetString a3_glGetString
#define glGetStringi a3_glGetStringi
#define glGetIntegerv a3_glGetIntegerv
#define glEnable a3_glEnable
#define glDisable a3_glDisable
#define glBlendFunc a3_glBlendFunc
#define glBlendFuncSeparate a3_glBlendFuncSeparate
#define glDepthFunc a3_glDepthFunc
#define glDepthMask a3_glDepthMask
#define glCullFace a3_glCullFace
#define glFrontFace a3_glFrontFace
#define glPolygonMode a3_glPolygonMode
#define glPolygonOffset a3_glPolygonOffset
#define glColorMask a3_glColorMask
#define glClear a3_glClear
#define glClearColor a3_glClearColor
#define glClearDepth a3_glClearDepth
#define glViewport a3_glViewport
#define glScissor a3_glScissor
#define glPixelStorei a3_glPixelStorei
#define glReadPixels a3_glReadPixels
#define glFinish a3_glFinish
#define glFlush a3_glFlush
#define glLineWidth a3_glLineWidth
#define glGenBuffers a3_glGenBuffers
#define glDeleteBuffers a3_glDeleteBuffers
#define glBindBuffer a3_glBindBuffer
#define glBufferData a3_glBufferData
#define glBufferSubData a3_glBufferSubData
#define glBindBufferBase a3_glBindBufferBase
#define glGenVertexArrays a3_glGenVertexArrays
#define glDeleteVertexArrays a3_glDeleteVertexArrays
#define glBindVertexArray a3_glBindVertexArray
#define glEnableVertexAttribArray a3_glEnableVertexAttribArray
#define glDisableVertexAttribArray a3_glDisableVertexAttribArray
#define glVertexAttribPointer a3_glVertexAttribPointer
#define glVertexAttribIPointer a3_glVertexAttribIPointer
#define glVertexAttribDivisor a3_glVertexAttribDivisor
#define glGenTextures a3_glGenTextures
#define glDeleteTextures a3_glDeleteTextures
#define glBindTexture a3_glBindTexture
#define glActiveTexture a3_glActiveTexture
#define glTexImage2D a3_glTexImage2D
#define glTexSubImage2D a3_glTexSubImage2D
#define glTexParameteri a3_glTexParameteri
#define glTexParameterf a3_glTexParameterf
#define glTexParameterfv a3_glTexParameterfv
#define glGenerateMipmap a3_glGenerateMipmap
#define glCreateShader a3_glCreateShader
#define glDeleteShader a3_glDeleteShader
#define glShaderSource a3_glShaderSource
#define glCompileShader a3_glCompileShader
#define glGetShaderiv a3_glGetShaderiv
#define glGetShaderInfoLog a3_glGetShaderInfoLog
#define glCreateProgram a3_glCreateProgram
#define glDeleteProgram a3_glDeleteProgram
#define glAttachShader a3_glAttachShader
#define glDetachShader a3_glDetachShader
#define glLinkProgram a3_glLinkProgram
#define glGetProgramiv a3_glGetProgramiv
#define glGetProgramInfoLog a3_glGetProgramInfoLog
#define glUseProgram a3_glUseProgram
#define glBindAttribLocation a3_glBindAttribLocation
#define glGetUniformLocation a3_glGetUniformLocation
#define glGetUniformBlockIndex a3_glGetUniformBlockIndex
#define glUniformBlockBinding a3_glUniformBlockBinding
#define glUniform1i a3_glUniform1i
#define glUniform1f a3_glUniform1f
#define glUniform2f a3_glUniform2f
#define glUniform3f a3_glUniform3f
#define glUniform4f a3_glUniform4f
#define glUniform1fv a3_glUniform1fv
#define glUniform2fv a3_glUniform2fv
#define glUniform3fv a3_glUniform3fv
#define glUniform4fv a3_glUniform4fv
#define glUniform1iv a3_glUniform1iv
#define glUniformMatrix4fv a3_glUniformMatrix4fv
#define glGenFramebuffers a3_glGenFramebuffers
#define glDeleteFramebuffers a3_glDeleteFramebuffers
#define glBindFramebuffer a3_glBindFramebuffer
#define glFramebufferTexture2D a3_glFramebufferTexture2D
#define glCheckFramebufferStatus a3_glCheckFramebufferStatus
#define glDrawBuffers a3_glDrawBuffers
#define glReadBuffer a3_glReadBuffer
#define glBlitFramebuffer a3_glBlitFramebuffer
#define glDrawArrays a3_glDrawArrays
#define glDrawElements a3_glDrawElements
#define glDrawArraysInstanced a3_glDrawArraysInstanced
#define glDrawElementsInstanced a3_glDrawElementsInstanced
#define glGenQueries a3_glGenQueries
#define glDeleteQueries a3_glDeleteQueries
#define glQueryCounter a3_glQueryCounter
#define glBeginQuery a3_glBeginQuery
#define glEndQuery a3_glEndQuery
#define glGetQueryObjectiv a3_glGetQueryObjectiv
#define glGetQueryObjectui64v a3_glGetQueryObjectui64v

typedef void *(*A3GLGetProcFn)(const char *name);
/* Resolves all functions. Returns number of missing functions (0 = success). */
int a3_gl_load(A3GLGetProcFn get_proc);

A3_EXTERN_C_END

#endif
