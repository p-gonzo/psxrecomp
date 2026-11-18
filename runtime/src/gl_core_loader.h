/* gl_core_loader.h - GL 1.1 entry points loaded through SDL, not the linker.
 *
 * gpu_gl_renderer.c already loads every GL 1.2+ entry point with
 * SDL_GL_GetProcAddress (load_modern_gl). The GL 1.1 functions it calls were
 * the only reason the runtime linked a system GL library (opengl32 /
 * OpenGL.framework / libGL). Loading them the same way removes that link-time
 * dependency, so a host without OpenGL (Nintendo 3DS) still links the GL
 * backend: there, SDL cannot create a GL context, gl_renderer_init_context
 * returns 0 before any GL call, and the runtime stays on the software
 * renderer, exactly as it already does when GL init fails on a desktop.
 *
 * Include once, from gpu_gl_renderer.c only, right after SDL_opengl.h. Every
 * glXxx(...) call in that translation unit then goes through psx_gl_core.
 * psx_gl_core_load() must succeed (with the context current) before the first
 * GL 1.1 call; it is all-or-nothing like load_modern_gl.
 */
#ifndef PSX_GL_CORE_LOADER_H
#define PSX_GL_CORE_LOADER_H

#ifndef GLAPIENTRY
#define GLAPIENTRY APIENTRY
#endif

/* X(return type, name without the gl prefix, parameter list) */
#define PSX_GL_CORE_FUNCS(X)                                                              \
    X(void,           BindTexture,           (GLenum, GLuint))                            \
    X(void,           Clear,                 (GLbitfield))                                \
    X(void,           ClearColor,            (GLclampf, GLclampf, GLclampf, GLclampf))    \
    X(void,           ClearDepth,            (GLclampd))                                  \
    X(void,           ClearStencil,          (GLint))                                     \
    X(void,           ColorMask,             (GLboolean, GLboolean, GLboolean, GLboolean))\
    X(void,           CopyTexImage2D,        (GLenum, GLint, GLenum, GLint, GLint,        \
                                              GLsizei, GLsizei, GLint))                   \
    X(void,           CopyTexSubImage2D,     (GLenum, GLint, GLint, GLint, GLint, GLint,  \
                                              GLsizei, GLsizei))                          \
    X(void,           DeleteTextures,        (GLsizei, const GLuint *))                   \
    X(void,           DepthFunc,             (GLenum))                                    \
    X(void,           DepthMask,             (GLboolean))                                 \
    X(void,           Disable,               (GLenum))                                    \
    X(void,           DrawArrays,            (GLenum, GLint, GLsizei))                    \
    X(void,           Enable,                (GLenum))                                    \
    X(void,           Finish,                (void))                                      \
    X(void,           Flush,                 (void))                                      \
    X(void,           GenTextures,           (GLsizei, GLuint *))                         \
    X(void,           GetBooleanv,           (GLenum, GLboolean *))                       \
    X(GLenum,         GetError,              (void))                                      \
    X(void,           GetFloatv,             (GLenum, GLfloat *))                         \
    X(void,           GetIntegerv,           (GLenum, GLint *))                           \
    X(const GLubyte *,GetString,             (GLenum))                                    \
    X(void,           GetTexImage,           (GLenum, GLint, GLenum, GLenum, GLvoid *))   \
    X(void,           GetTexLevelParameteriv,(GLenum, GLint, GLenum, GLint *))            \
    X(void,           GetTexParameteriv,     (GLenum, GLenum, GLint *))                   \
    X(GLboolean,      IsEnabled,             (GLenum))                                    \
    X(void,           LineWidth,             (GLfloat))                                   \
    X(void,           PixelStorei,           (GLenum, GLint))                             \
    X(void,           ReadBuffer,            (GLenum))                                    \
    X(void,           ReadPixels,            (GLint, GLint, GLsizei, GLsizei, GLenum,     \
                                              GLenum, GLvoid *))                          \
    X(void,           Scissor,               (GLint, GLint, GLsizei, GLsizei))            \
    X(void,           StencilFunc,           (GLenum, GLint, GLuint))                     \
    X(void,           StencilMask,           (GLuint))                                    \
    X(void,           StencilOp,             (GLenum, GLenum, GLenum))                    \
    X(void,           TexImage2D,            (GLenum, GLint, GLint, GLsizei, GLsizei,     \
                                              GLint, GLenum, GLenum, const GLvoid *))     \
    X(void,           TexParameterf,         (GLenum, GLenum, GLfloat))                   \
    X(void,           TexParameteri,         (GLenum, GLenum, GLint))                     \
    X(void,           TexSubImage2D,         (GLenum, GLint, GLint, GLint, GLsizei,       \
                                              GLsizei, GLenum, GLenum, const GLvoid *))   \
    X(void,           Viewport,              (GLint, GLint, GLsizei, GLsizei))

#define PSX_GL_CORE_MEMBER(ret, name, args) ret (GLAPIENTRY *name) args;
static struct { PSX_GL_CORE_FUNCS(PSX_GL_CORE_MEMBER) } psx_gl_core;
#undef PSX_GL_CORE_MEMBER

/* Returns 1 when every entry point resolved, 0 otherwise (and names the first
 * missing one in *missing). Call with the GL context current. */
static int psx_gl_core_load(const char **missing) {
    *missing = NULL;
#define PSX_GL_CORE_LOAD(ret, name, args)                                         \
    psx_gl_core.name = (ret (GLAPIENTRY *) args)SDL_GL_GetProcAddress("gl" #name); \
    if (!psx_gl_core.name && !*missing) *missing = "gl" #name;
    PSX_GL_CORE_FUNCS(PSX_GL_CORE_LOAD)
#undef PSX_GL_CORE_LOAD
    return *missing == NULL;
}

/* Object-like, so glClear(mask) becomes (psx_gl_core.Clear)(mask). */
#define glBindTexture            (psx_gl_core.BindTexture)
#define glClear                  (psx_gl_core.Clear)
#define glClearColor             (psx_gl_core.ClearColor)
#define glClearDepth             (psx_gl_core.ClearDepth)
#define glClearStencil           (psx_gl_core.ClearStencil)
#define glColorMask              (psx_gl_core.ColorMask)
#define glCopyTexImage2D         (psx_gl_core.CopyTexImage2D)
#define glCopyTexSubImage2D      (psx_gl_core.CopyTexSubImage2D)
#define glDeleteTextures         (psx_gl_core.DeleteTextures)
#define glDepthFunc              (psx_gl_core.DepthFunc)
#define glDepthMask              (psx_gl_core.DepthMask)
#define glDisable                (psx_gl_core.Disable)
#define glDrawArrays             (psx_gl_core.DrawArrays)
#define glEnable                 (psx_gl_core.Enable)
#define glFinish                 (psx_gl_core.Finish)
#define glFlush                  (psx_gl_core.Flush)
#define glGenTextures            (psx_gl_core.GenTextures)
#define glGetBooleanv            (psx_gl_core.GetBooleanv)
#define glGetError               (psx_gl_core.GetError)
#define glGetFloatv              (psx_gl_core.GetFloatv)
#define glGetIntegerv            (psx_gl_core.GetIntegerv)
#define glGetString              (psx_gl_core.GetString)
#define glGetTexImage            (psx_gl_core.GetTexImage)
#define glGetTexLevelParameteriv (psx_gl_core.GetTexLevelParameteriv)
#define glGetTexParameteriv      (psx_gl_core.GetTexParameteriv)
#define glIsEnabled              (psx_gl_core.IsEnabled)
#define glLineWidth              (psx_gl_core.LineWidth)
#define glPixelStorei            (psx_gl_core.PixelStorei)
#define glReadBuffer             (psx_gl_core.ReadBuffer)
#define glReadPixels             (psx_gl_core.ReadPixels)
#define glScissor                (psx_gl_core.Scissor)
#define glStencilFunc            (psx_gl_core.StencilFunc)
#define glStencilMask            (psx_gl_core.StencilMask)
#define glStencilOp              (psx_gl_core.StencilOp)
#define glTexImage2D             (psx_gl_core.TexImage2D)
#define glTexParameterf          (psx_gl_core.TexParameterf)
#define glTexParameteri          (psx_gl_core.TexParameteri)
#define glTexSubImage2D          (psx_gl_core.TexSubImage2D)
#define glViewport               (psx_gl_core.Viewport)

#endif /* PSX_GL_CORE_LOADER_H */
