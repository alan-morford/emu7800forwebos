/*
 * OpenGL ES 1.1 stub header for cross-compilation
 * Real implementation provided by device libraries at runtime
 */

#ifndef __gles_gl_h_
#define __gles_gl_h_

#include <stdint.h>

/* OpenGL ES types */
typedef unsigned int    GLenum;
typedef unsigned char   GLboolean;
typedef unsigned int    GLbitfield;
typedef signed char     GLbyte;
typedef short           GLshort;
typedef int             GLint;
typedef int             GLsizei;
typedef unsigned char   GLubyte;
typedef unsigned short  GLushort;
typedef unsigned int    GLuint;
typedef float           GLfloat;
typedef float           GLclampf;
typedef void            GLvoid;
typedef int32_t         GLfixed;
typedef int32_t         GLclampx;

/* OpenGL ES constants */
#define GL_FALSE                    0
#define GL_TRUE                     1

#define GL_DEPTH_BUFFER_BIT         0x00000100
#define GL_STENCIL_BUFFER_BIT       0x00000400
#define GL_COLOR_BUFFER_BIT         0x00004000

#define GL_POINTS                   0x0000
#define GL_LINES                    0x0001
#define GL_LINE_LOOP                0x0002
#define GL_LINE_STRIP               0x0003
#define GL_TRIANGLES                0x0004
#define GL_TRIANGLE_STRIP           0x0005
#define GL_TRIANGLE_FAN             0x0006

#define GL_NEVER                    0x0200
#define GL_LESS                     0x0201
#define GL_EQUAL                    0x0202
#define GL_LEQUAL                   0x0203
#define GL_GREATER                  0x0204
#define GL_NOTEQUAL                 0x0205
#define GL_GEQUAL                   0x0206
#define GL_ALWAYS                   0x0207

#define GL_SRC_COLOR                0x0300
#define GL_ONE_MINUS_SRC_COLOR      0x0301
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DST_ALPHA                0x0304
#define GL_ONE_MINUS_DST_ALPHA      0x0305

#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_DST_COLOR      0x0307
#define GL_SRC_ALPHA_SATURATE       0x0308

#define GL_FRONT                    0x0404
#define GL_BACK                     0x0405
#define GL_FRONT_AND_BACK           0x0408

#define GL_CULL_FACE                0x0B44
#define GL_DEPTH_TEST               0x0B71
#define GL_LIGHTING                 0x0B50

#define GL_TEXTURE_2D               0x0DE1

#define GL_BLEND                    0x0BE2

#define GL_BYTE                     0x1400
#define GL_UNSIGNED_BYTE            0x1401
#define GL_SHORT                    0x1402
#define GL_UNSIGNED_SHORT           0x1403
#define GL_FLOAT                    0x1406
#define GL_FIXED                    0x140C

#define GL_MODELVIEW                0x1700
#define GL_PROJECTION               0x1701
#define GL_TEXTURE                  0x1702

#define GL_ALPHA                    0x1906
#define GL_RGB                      0x1907
#define GL_RGBA                     0x1908
#define GL_LUMINANCE                0x1909
#define GL_LUMINANCE_ALPHA          0x190A

#define GL_NEAREST                  0x2600
#define GL_LINEAR                   0x2601

#define GL_NEAREST_MIPMAP_NEAREST   0x2700
#define GL_LINEAR_MIPMAP_NEAREST    0x2701
#define GL_NEAREST_MIPMAP_LINEAR    0x2702
#define GL_LINEAR_MIPMAP_LINEAR     0x2703

#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803

#define GL_CLAMP_TO_EDGE            0x812F


#define GL_VERTEX_ARRAY             0x8074
#define GL_NORMAL_ARRAY             0x8075
#define GL_COLOR_ARRAY              0x8076
#define GL_TEXTURE_COORD_ARRAY      0x8078

/* Error codes */
#define GL_NO_ERROR                 0
#define GL_INVALID_ENUM             0x0500
#define GL_INVALID_VALUE            0x0501
#define GL_INVALID_OPERATION        0x0502
#define GL_STACK_OVERFLOW           0x0503
#define GL_STACK_UNDERFLOW          0x0504
#define GL_OUT_OF_MEMORY            0x0505

/* Function declarations (stubs for cross-compilation) */
extern void glEnable(GLenum cap);
extern void glDisable(GLenum cap);
extern void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
extern void glClear(GLbitfield mask);
extern void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
extern void glMatrixMode(GLenum mode);
extern void glLoadIdentity(void);
extern void glOrthof(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat zNear, GLfloat zFar);
extern void glGenTextures(GLsizei n, GLuint *textures);
extern void glDeleteTextures(GLsizei n, const GLuint *textures);
extern void glBindTexture(GLenum target, GLuint texture);
extern void glTexParameteri(GLenum target, GLenum pname, GLint param);
extern void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
extern void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
extern void glEnableClientState(GLenum array);
extern void glDisableClientState(GLenum array);
extern void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
extern void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
extern void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
extern void glDrawArrays(GLenum mode, GLint first, GLsizei count);
extern void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
extern void glBlendFunc(GLenum sfactor, GLenum dfactor);
extern void glFinish(void);
extern void glFlush(void);
extern GLenum glGetError(void);

#endif /* __gles_gl_h_ */
