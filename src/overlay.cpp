// In-game overlay for mods: Dear ImGui drawn over the game at window resolution, after the game's
// frame is scaled to the window and before the buffer swap. The renderer is a small OpenGL ES 2
// backend on the client's GL loader (lookup_gl), saving and restoring every piece of GL state it
// touches so the game never notices. Input arrives from the client's SDL pump (s3e_input.c).
// Lua draws through the `ui` module; widgets that contain others take a function, so a Lua error
// can never leave a window open.
#include "mod_runtime.h"

#include "imgui.h"
#include "imgui_internal.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
void *lookup_gl(const char *symbol);
}

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define BOZ_GL_APIENTRY __stdcall
#else
#define BOZ_GL_APIENTRY
#endif

namespace {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef intptr_t GLsizeiptr;
typedef unsigned int GLbitfield;

enum : GLenum {
    GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE0 = 0x84C0,
    GL_ACTIVE_TEXTURE = 0x84E0,
    GL_TEXTURE_BINDING_2D = 0x8069,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_LINEAR = 0x2601,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_RGBA = 0x1908,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_SHORT = 0x1403,
    GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406,
    GL_TRIANGLES = 0x0004,
    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_ARRAY_BUFFER_BINDING = 0x8894,
    GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895,
    GL_STREAM_DRAW = 0x88E0,
    GL_CURRENT_PROGRAM = 0x8B8D,
    GL_VERTEX_SHADER = 0x8B31,
    GL_FRAGMENT_SHADER = 0x8B30,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_BLEND = 0x0BE2,
    GL_CULL_FACE = 0x0B44,
    GL_DEPTH_TEST = 0x0B71,
    GL_STENCIL_TEST = 0x0B90,
    GL_SCISSOR_TEST = 0x0C11,
    GL_VIEWPORT = 0x0BA2,
    GL_SCISSOR_BOX = 0x0C10,
    GL_COLOR_WRITEMASK = 0x0C23,
    GL_DEPTH_WRITEMASK = 0x0B72,
    GL_BLEND_SRC_RGB = 0x80C9,
    GL_BLEND_DST_RGB = 0x80C8,
    GL_BLEND_SRC_ALPHA = 0x80CB,
    GL_BLEND_DST_ALPHA = 0x80CA,
    GL_BLEND_EQUATION_RGB = 0x8009,
    GL_BLEND_EQUATION_ALPHA = 0x883D,
    GL_FUNC_ADD = 0x8006,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_ONE = 1,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_VERTEX_ATTRIB_ARRAY_ENABLED = 0x8622,
    GL_VERTEX_ATTRIB_ARRAY_SIZE = 0x8623,
    GL_VERTEX_ATTRIB_ARRAY_STRIDE = 0x8624,
    GL_VERTEX_ATTRIB_ARRAY_TYPE = 0x8625,
    GL_VERTEX_ATTRIB_ARRAY_NORMALIZED = 0x886A,
    GL_VERTEX_ATTRIB_ARRAY_POINTER = 0x8645,
    GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING = 0x889F,
    GL_VERTEX_ARRAY_BINDING = 0x85B5,
};

struct Gl {
    void(BOZ_GL_APIENTRY *ActiveTexture)(GLenum);
    void(BOZ_GL_APIENTRY *AttachShader)(GLuint, GLuint);
    void(BOZ_GL_APIENTRY *BindAttribLocation)(GLuint, GLuint, const GLchar *);
    void(BOZ_GL_APIENTRY *BindBuffer)(GLenum, GLuint);
    void(BOZ_GL_APIENTRY *BindTexture)(GLenum, GLuint);
    void(BOZ_GL_APIENTRY *BindVertexArray)(GLuint);  // optional (ES 3)
    void(BOZ_GL_APIENTRY *BlendEquationSeparate)(GLenum, GLenum);
    void(BOZ_GL_APIENTRY *BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void(BOZ_GL_APIENTRY *BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void(BOZ_GL_APIENTRY *ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void(BOZ_GL_APIENTRY *CompileShader)(GLuint);
    GLuint(BOZ_GL_APIENTRY *CreateProgram)(void);
    GLuint(BOZ_GL_APIENTRY *CreateShader)(GLenum);
    void(BOZ_GL_APIENTRY *DeleteTextures)(GLsizei, const GLuint *);
    void(BOZ_GL_APIENTRY *DepthMask)(GLboolean);
    void(BOZ_GL_APIENTRY *Disable)(GLenum);
    void(BOZ_GL_APIENTRY *DisableVertexAttribArray)(GLuint);
    void(BOZ_GL_APIENTRY *DrawElements)(GLenum, GLsizei, GLenum, const void *);
    void(BOZ_GL_APIENTRY *Enable)(GLenum);
    void(BOZ_GL_APIENTRY *EnableVertexAttribArray)(GLuint);
    void(BOZ_GL_APIENTRY *GenBuffers)(GLsizei, GLuint *);
    void(BOZ_GL_APIENTRY *GenTextures)(GLsizei, GLuint *);
    void(BOZ_GL_APIENTRY *GetBooleanv)(GLenum, GLboolean *);
    void(BOZ_GL_APIENTRY *GetIntegerv)(GLenum, GLint *);
    void(BOZ_GL_APIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
    void(BOZ_GL_APIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void(BOZ_GL_APIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
    GLint(BOZ_GL_APIENTRY *GetUniformLocation)(GLuint, const GLchar *);
    void(BOZ_GL_APIENTRY *GetVertexAttribiv)(GLuint, GLenum, GLint *);
    void(BOZ_GL_APIENTRY *GetVertexAttribPointerv)(GLuint, GLenum, void **);
    GLboolean(BOZ_GL_APIENTRY *IsEnabled)(GLenum);
    void(BOZ_GL_APIENTRY *LinkProgram)(GLuint);
    void(BOZ_GL_APIENTRY *PixelStorei)(GLenum, GLint);
    void(BOZ_GL_APIENTRY *Scissor)(GLint, GLint, GLsizei, GLsizei);
    void(BOZ_GL_APIENTRY *ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void(BOZ_GL_APIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                      const void *);
    void(BOZ_GL_APIENTRY *TexParameteri)(GLenum, GLenum, GLint);
    void(BOZ_GL_APIENTRY *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                                         GLenum, const void *);
    void(BOZ_GL_APIENTRY *Uniform1i)(GLint, GLint);
    void(BOZ_GL_APIENTRY *UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
    void(BOZ_GL_APIENTRY *UseProgram)(GLuint);
    void(BOZ_GL_APIENTRY *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void(BOZ_GL_APIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei);
};

Gl gl;
bool g_gl_ready, g_gl_failed;
GLuint g_program, g_vbo, g_ibo;
GLint g_loc_texture, g_loc_proj;
enum { ATTRIB_POSITION = 0, ATTRIB_UV = 1, ATTRIB_COLOR = 2, ATTRIB_COUNT = 3 };

template <typename T> bool load(T &slot, const char *name, bool required = true) {
    slot = reinterpret_cast<T>(lookup_gl(name));
    if (!slot && required) {
        std::fprintf(stderr, "[overlay] missing GL function %s\n", name);
        return false;
    }
    return true;
}

bool load_gl() {
    bool ok = true;
#define L(name) ok &= load(gl.name, "gl" #name)
    L(ActiveTexture); L(AttachShader); L(BindAttribLocation); L(BindBuffer); L(BindTexture);
    L(BlendEquationSeparate); L(BlendFuncSeparate); L(BufferData); L(ColorMask); L(CompileShader);
    L(CreateProgram); L(CreateShader); L(DeleteTextures); L(DepthMask); L(Disable);
    L(DisableVertexAttribArray); L(DrawElements); L(Enable); L(EnableVertexAttribArray);
    L(GenBuffers); L(GenTextures); L(GetBooleanv); L(GetIntegerv); L(GetProgramiv);
    L(GetShaderInfoLog); L(GetShaderiv); L(GetUniformLocation); L(GetVertexAttribiv);
    L(GetVertexAttribPointerv); L(IsEnabled); L(LinkProgram); L(PixelStorei); L(Scissor);
    L(ShaderSource); L(TexImage2D); L(TexParameteri); L(TexSubImage2D); L(Uniform1i);
    L(UniformMatrix4fv); L(UseProgram); L(VertexAttribPointer); L(Viewport);
#undef L
    load(gl.BindVertexArray, "glBindVertexArray", false);
    return ok;
}

GLuint compile(GLenum type, const char *source) {
    GLuint shader = gl.CreateShader(type);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint status = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512] = "";
        gl.GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[overlay] shader: %s\n", log);
        return 0;
    }
    return shader;
}

bool init_gl() {
    if (g_gl_ready || g_gl_failed) {
        return g_gl_ready;
    }
    g_gl_failed = true;
    if (!load_gl()) {
        return false;
    }
    static const char *vertex =
        "uniform mat4 ProjMtx;\n"
        "attribute vec2 Position;\n"
        "attribute vec2 UV;\n"
        "attribute vec4 Color;\n"
        "varying vec2 Frag_UV;\n"
        "varying vec4 Frag_Color;\n"
        "void main() {\n"
        "    Frag_UV = UV;\n"
        "    Frag_Color = Color;\n"
        "    gl_Position = ProjMtx * vec4(Position.xy, 0.0, 1.0);\n"
        "}\n";
    static const char *fragment =
        "precision mediump float;\n"
        "uniform sampler2D Texture;\n"
        "varying vec2 Frag_UV;\n"
        "varying vec4 Frag_Color;\n"
        "void main() {\n"
        "    gl_FragColor = Frag_Color * texture2D(Texture, Frag_UV);\n"
        "}\n";
    GLuint vs = compile(GL_VERTEX_SHADER, vertex);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        return false;
    }
    g_program = gl.CreateProgram();
    gl.AttachShader(g_program, vs);
    gl.AttachShader(g_program, fs);
    gl.BindAttribLocation(g_program, ATTRIB_POSITION, "Position");
    gl.BindAttribLocation(g_program, ATTRIB_UV, "UV");
    gl.BindAttribLocation(g_program, ATTRIB_COLOR, "Color");
    gl.LinkProgram(g_program);
    GLint linked = 0;
    gl.GetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        std::fprintf(stderr, "[overlay] shader program did not link\n");
        return false;
    }
    g_loc_texture = gl.GetUniformLocation(g_program, "Texture");
    g_loc_proj = gl.GetUniformLocation(g_program, "ProjMtx");
    gl.GenBuffers(1, &g_vbo);
    gl.GenBuffers(1, &g_ibo);
    g_gl_failed = false;
    g_gl_ready = true;
    return true;
}

struct SavedState {
    GLint program, texture, active_texture, array_buffer, element_buffer, vertex_array;
    GLint viewport[4], scissor_box[4];
    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha, blend_eq_rgb, blend_eq_alpha;
    GLboolean blend, cull, depth, stencil, scissor, color_mask[4], depth_mask;
    GLint unpack_alignment;
    struct Attrib {
        GLint enabled, size, type, normalized, stride, buffer;
        void *pointer;
    } attribs[ATTRIB_COUNT];
};

void save_state(SavedState &s) {
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s.active_texture);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.array_buffer);
    s.vertex_array = 0;
    if (gl.BindVertexArray) {
        gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vertex_array);
        gl.BindVertexArray(0);
    }
    gl.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.element_buffer);
    gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    gl.GetIntegerv(GL_SCISSOR_BOX, s.scissor_box);
    gl.GetIntegerv(GL_BLEND_SRC_RGB, &s.blend_src_rgb);
    gl.GetIntegerv(GL_BLEND_DST_RGB, &s.blend_dst_rgb);
    gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &s.blend_src_alpha);
    gl.GetIntegerv(GL_BLEND_DST_ALPHA, &s.blend_dst_alpha);
    gl.GetIntegerv(GL_BLEND_EQUATION_RGB, &s.blend_eq_rgb);
    gl.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blend_eq_alpha);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpack_alignment);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, s.color_mask);
    gl.GetBooleanv(GL_DEPTH_WRITEMASK, &s.depth_mask);
    s.blend = gl.IsEnabled(GL_BLEND);
    s.cull = gl.IsEnabled(GL_CULL_FACE);
    s.depth = gl.IsEnabled(GL_DEPTH_TEST);
    s.stencil = gl.IsEnabled(GL_STENCIL_TEST);
    s.scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    for (GLuint i = 0; i < ATTRIB_COUNT; ++i) {
        SavedState::Attrib &a = s.attribs[i];
        gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
        gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
        gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
        gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
        gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
        gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
        gl.GetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
    }
}

void set_enabled(GLenum cap, GLboolean on) {
    if (on) {
        gl.Enable(cap);
    } else {
        gl.Disable(cap);
    }
}

void restore_state(const SavedState &s) {
    for (GLuint i = 0; i < ATTRIB_COUNT; ++i) {
        const SavedState::Attrib &a = s.attribs[i];
        gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)a.buffer);
        gl.VertexAttribPointer(i, a.size, (GLenum)a.type, (GLboolean)a.normalized, a.stride, a.pointer);
        if (a.enabled) {
            gl.EnableVertexAttribArray(i);
        } else {
            gl.DisableVertexAttribArray(i);
        }
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)s.array_buffer);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s.element_buffer);
    if (gl.BindVertexArray) {
        gl.BindVertexArray((GLuint)s.vertex_array);
    }
    gl.UseProgram((GLuint)s.program);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)s.texture);
    gl.ActiveTexture((GLenum)s.active_texture);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, s.unpack_alignment);
    gl.BlendEquationSeparate((GLenum)s.blend_eq_rgb, (GLenum)s.blend_eq_alpha);
    gl.BlendFuncSeparate((GLenum)s.blend_src_rgb, (GLenum)s.blend_dst_rgb, (GLenum)s.blend_src_alpha,
                         (GLenum)s.blend_dst_alpha);
    set_enabled(GL_BLEND, s.blend);
    set_enabled(GL_CULL_FACE, s.cull);
    set_enabled(GL_DEPTH_TEST, s.depth);
    set_enabled(GL_STENCIL_TEST, s.stencil);
    set_enabled(GL_SCISSOR_TEST, s.scissor);
    gl.ColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
    gl.DepthMask(s.depth_mask);
    gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    gl.Scissor(s.scissor_box[0], s.scissor_box[1], s.scissor_box[2], s.scissor_box[3]);
}

void update_texture(ImTextureData *tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        GLuint id = 0;
        gl.GenTextures(1, &id);
        gl.BindTexture(GL_TEXTURE_2D, id);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->Width, tex->Height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      tex->GetPixels());
        tex->SetTexID((ImTextureID)(intptr_t)id);
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        // ES 2 has no GL_UNPACK_ROW_LENGTH: upload each changed rectangle row by row.
        gl.BindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)tex->TexID);
        for (ImTextureRect &r : tex->Updates) {
            for (int y = 0; y < r.h; ++y) {
                gl.TexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y + y, r.w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                 tex->GetPixelsAt(r.x, r.y + y));
            }
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        GLuint id = (GLuint)(intptr_t)tex->TexID;
        gl.DeleteTextures(1, &id);
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void render(ImDrawData *draw) {
    int fb_width = (int)(draw->DisplaySize.x * draw->FramebufferScale.x);
    int fb_height = (int)(draw->DisplaySize.y * draw->FramebufferScale.y);
    if (fb_width <= 0 || fb_height <= 0 || !init_gl()) {
        return;
    }
    SavedState saved;
    save_state(saved);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (draw->Textures) {
        for (ImTextureData *tex : *draw->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                update_texture(tex);
            }
        }
    }

    gl.Enable(GL_BLEND);
    gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.Enable(GL_SCISSOR_TEST);
    gl.ColorMask(1, 1, 1, 1);
    gl.Viewport(0, 0, fb_width, fb_height);
    float l = draw->DisplayPos.x, r = l + draw->DisplaySize.x;
    float t = draw->DisplayPos.y, b = t + draw->DisplaySize.y;
    const float projection[16] = {
        2.0f / (r - l), 0, 0, 0, 0, 2.0f / (t - b), 0, 0, 0, 0, -1.0f, 0,
        (r + l) / (l - r), (t + b) / (b - t), 0, 1.0f,
    };
    gl.UseProgram(g_program);
    gl.Uniform1i(g_loc_texture, 0);
    gl.UniformMatrix4fv(g_loc_proj, 1, 0, projection);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    gl.EnableVertexAttribArray(ATTRIB_POSITION);
    gl.EnableVertexAttribArray(ATTRIB_UV);
    gl.EnableVertexAttribArray(ATTRIB_COLOR);
    gl.VertexAttribPointer(ATTRIB_POSITION, 2, GL_FLOAT, 0, sizeof(ImDrawVert),
                           (void *)offsetof(ImDrawVert, pos));
    gl.VertexAttribPointer(ATTRIB_UV, 2, GL_FLOAT, 0, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, uv));
    gl.VertexAttribPointer(ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE, 1, sizeof(ImDrawVert),
                           (void *)offsetof(ImDrawVert, col));

    ImVec2 clip_off = draw->DisplayPos, clip_scale = draw->FramebufferScale;
    for (ImDrawList *list : draw->CmdLists) {
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)list->VtxBuffer.Size * (GLsizeiptr)sizeof(ImDrawVert),
                      list->VtxBuffer.Data, GL_STREAM_DRAW);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)list->IdxBuffer.Size * (GLsizeiptr)sizeof(ImDrawIdx),
                      list->IdxBuffer.Data, GL_STREAM_DRAW);
        for (const ImDrawCmd &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                continue;
            }
            ImVec2 min((cmd.ClipRect.x - clip_off.x) * clip_scale.x, (cmd.ClipRect.y - clip_off.y) * clip_scale.y);
            ImVec2 max((cmd.ClipRect.z - clip_off.x) * clip_scale.x, (cmd.ClipRect.w - clip_off.y) * clip_scale.y);
            if (max.x <= min.x || max.y <= min.y) {
                continue;
            }
            gl.Scissor((GLint)min.x, (GLint)((float)fb_height - max.y), (GLsizei)(max.x - min.x),
                       (GLsizei)(max.y - min.y));
            gl.BindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd.GetTexID());
            gl.DrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount,
                            sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                            (void *)(intptr_t)(cmd.IdxOffset * sizeof(ImDrawIdx)));
        }
    }
    restore_state(saved);
}

// --- ImGui context, input ----------------------------------------------------------------------

bool g_context;
bool g_in_frame;
bool g_capturing;
float g_scale = 1.0f;
ImGuiErrorRecoveryState g_recovery;

ImGuiKey imgui_key(int scancode) {
    if (scancode >= 4 && scancode <= 29) {
        return (ImGuiKey)(ImGuiKey_A + (scancode - 4));
    }
    if (scancode >= 30 && scancode <= 38) {
        return (ImGuiKey)(ImGuiKey_1 + (scancode - 30));
    }
    if (scancode >= 58 && scancode <= 69) {
        return (ImGuiKey)(ImGuiKey_F1 + (scancode - 58));
    }
    switch (scancode) {
    case 39: return ImGuiKey_0;
    case 40: return ImGuiKey_Enter;
    case 41: return ImGuiKey_Escape;
    case 42: return ImGuiKey_Backspace;
    case 43: return ImGuiKey_Tab;
    case 44: return ImGuiKey_Space;
    case 45: return ImGuiKey_Minus;
    case 46: return ImGuiKey_Equal;
    case 53: return ImGuiKey_GraveAccent;
    case 73: return ImGuiKey_Insert;
    case 74: return ImGuiKey_Home;
    case 75: return ImGuiKey_PageUp;
    case 76: return ImGuiKey_Delete;
    case 77: return ImGuiKey_End;
    case 78: return ImGuiKey_PageDown;
    case 79: return ImGuiKey_RightArrow;
    case 80: return ImGuiKey_LeftArrow;
    case 81: return ImGuiKey_DownArrow;
    case 82: return ImGuiKey_UpArrow;
    case 88: return ImGuiKey_KeypadEnter;
    case 224: return ImGuiKey_LeftCtrl;
    case 225: return ImGuiKey_LeftShift;
    case 226: return ImGuiKey_LeftAlt;
    case 227: return ImGuiKey_LeftSuper;
    case 228: return ImGuiKey_RightCtrl;
    case 229: return ImGuiKey_RightShift;
    case 230: return ImGuiKey_RightAlt;
    case 231: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

void ensure_context() {
    if (g_context) {
        return;
    }
    g_context = true;
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendRendererName = "boz_gles2";
    io.BackendPlatformName = "boz_client";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.Fonts->AddFontDefaultVector();
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
}

}  // namespace

extern "C" void overlay_event(const struct overlay_event *e) {
    if (!g_context) {
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    switch (e->kind) {
    case OVERLAY_KEY: {
        io.AddKeyEvent(ImGuiMod_Ctrl, (e->modifiers & 1) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (e->modifiers & 2) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (e->modifiers & 4) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (e->modifiers & 8) != 0);
        ImGuiKey key = imgui_key(e->scancode);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, e->down);
        }
        break;
    }
    case OVERLAY_TEXT:
        if (g_capturing && e->text) {
            io.AddInputCharactersUTF8(e->text);
        }
        break;
    case OVERLAY_MOUSE_MOVE:
        io.AddMousePosEvent(e->x, e->y);
        break;
    case OVERLAY_MOUSE_BUTTON:
        if (e->button >= 0 && e->button < 3) {
            io.AddMouseButtonEvent(e->button, e->down);
        }
        break;
    case OVERLAY_MOUSE_WHEEL:
        io.AddMouseWheelEvent(e->wheel_x, e->wheel_y);
        break;
    }
}

extern "C" bool overlay_capturing(void) {
    return g_capturing;
}

extern "C" void overlay_begin_frame(int width, int height, double dt) {
    ensure_context();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);
    io.DeltaTime = dt > 0.0 ? (float)dt : 1.0f / 60.0f;
    float scale = height >= 720 ? (float)height / 720.0f : 1.0f;
    if (scale != g_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / g_scale);
        style.FontScaleDpi = scale;
        g_scale = scale;
    }
    io.MouseDrawCursor = g_capturing;
    if (!g_capturing) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    ImGui::NewFrame();
    ImGui::ErrorRecoveryStoreState(&g_recovery);
    g_in_frame = true;
}

extern "C" void overlay_end_frame(void) {
    if (!g_in_frame) {
        return;
    }
    g_in_frame = false;
    ImGui::ErrorRecoveryTryToRecoverState(&g_recovery);
    ImGui::Render();
    ImDrawData *draw = ImGui::GetDrawData();
    if (draw && (draw->TotalVtxCount > 0 || (draw->Textures && !draw->Textures->empty()))) {
        render(draw);
    }
}

// --- Lua: ui and overlay -----------------------------------------------------------------------

namespace {

int check_frame(lua_State *L) {
    if (!g_in_frame) {
        return luaL_error(L, "ui functions can only be used in a \"frame\" event handler");
    }
    return 0;
}

// Calls the function at index with no arguments; errors propagate after the caller closes its scope.
int call_body(lua_State *L, int index) {
    lua_pushvalue(L, index);
    return lua_pcall(L, 0, 0, 0);
}

int window_flags(lua_State *L, int opts) {
    int flags = 0;
    if (!lua_istable(L, opts)) {
        return flags;
    }
    struct {
        const char *key;
        int flag;
    } names[] = {
        {"no_title", ImGuiWindowFlags_NoTitleBar},     {"no_resize", ImGuiWindowFlags_NoResize},
        {"no_move", ImGuiWindowFlags_NoMove},          {"no_collapse", ImGuiWindowFlags_NoCollapse},
        {"auto_size", ImGuiWindowFlags_AlwaysAutoResize}, {"no_background", ImGuiWindowFlags_NoBackground},
        {"no_inputs", ImGuiWindowFlags_NoInputs},      {"no_scrollbar", ImGuiWindowFlags_NoScrollbar},
    };
    for (auto &n : names) {
        lua_getfield(L, opts, n.key);
        if (lua_toboolean(L, -1)) {
            flags |= n.flag;
        }
        lua_pop(L, 1);
    }
    return flags;
}

float opt_number(lua_State *L, int opts, const char *key, float fallback) {
    if (!lua_istable(L, opts)) {
        return fallback;
    }
    lua_getfield(L, opts, key);
    float value = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

bool opt_bool(lua_State *L, int opts, const char *key) {
    if (!lua_istable(L, opts)) {
        return false;
    }
    lua_getfield(L, opts, key);
    bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

// ui.window(title, opts, fn) -> open. opts: x, y, w, h (first use, in 720p units), closable, flags.
int ui_window(lua_State *L) {
    check_frame(L);
    const char *title = luaL_checkstring(L, 1);
    int body = lua_isfunction(L, 2) ? 2 : 3;
    int opts = body == 3 ? 2 : 0;
    luaL_checktype(L, body, LUA_TFUNCTION);
    float x = opt_number(L, opts, "x", -1), y = opt_number(L, opts, "y", -1);
    float w = opt_number(L, opts, "w", 0), h = opt_number(L, opts, "h", 0);
    if (x >= 0 && y >= 0) {
        ImGui::SetNextWindowPos(ImVec2(x * g_scale, y * g_scale), ImGuiCond_FirstUseEver);
    }
    if (w > 0 && h > 0) {
        ImGui::SetNextWindowSize(ImVec2(w * g_scale, h * g_scale), ImGuiCond_FirstUseEver);
    }
    if (opt_bool(L, opts, "focus")) {
        ImGui::SetNextWindowFocus();
    }
    bool open = true;
    bool visible = ImGui::Begin(title, opt_bool(L, opts, "closable") ? &open : nullptr, window_flags(L, opts));
    int status = visible ? call_body(L, body) : LUA_OK;
    ImGui::End();
    if (status != LUA_OK) {
        return lua_error(L);
    }
    lua_pushboolean(L, open);
    return 1;
}

int ui_child(lua_State *L) {
    check_frame(L);
    const char *id = luaL_checkstring(L, 1);
    float w = (float)luaL_optnumber(L, 2, 0), h = (float)luaL_optnumber(L, 3, 0);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    ImGui::BeginChild(id, ImVec2(w * g_scale, h * g_scale), ImGuiChildFlags_Borders);
    int status = call_body(L, 4);
    ImGui::EndChild();
    return status != LUA_OK ? lua_error(L) : 0;
}

int ui_collapsing(lua_State *L) {
    check_frame(L);
    const char *label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    bool open = ImGui::CollapsingHeader(label, lua_toboolean(L, 3) ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (open && call_body(L, 2) != LUA_OK) {
        return lua_error(L);
    }
    lua_pushboolean(L, open);
    return 1;
}

int ui_tree(lua_State *L) {
    check_frame(L);
    const char *label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    bool open = ImGui::TreeNode(label);
    if (open) {
        int status = call_body(L, 2);
        ImGui::TreePop();
        if (status != LUA_OK) {
            return lua_error(L);
        }
    }
    lua_pushboolean(L, open);
    return 1;
}

int ui_tab_bar(lua_State *L) {
    check_frame(L);
    const char *id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ImGui::BeginTabBar(id)) {
        int status = call_body(L, 2);
        ImGui::EndTabBar();
        if (status != LUA_OK) {
            return lua_error(L);
        }
    }
    return 0;
}

int ui_tab(lua_State *L) {
    check_frame(L);
    const char *label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    bool open = ImGui::BeginTabItem(label);
    if (open) {
        int status = call_body(L, 2);
        ImGui::EndTabItem();
        if (status != LUA_OK) {
            return lua_error(L);
        }
    }
    lua_pushboolean(L, open);
    return 1;
}

// ui.table(id, columns, fn): inside, ui.next_column() moves to the next cell.
int ui_table(lua_State *L) {
    check_frame(L);
    const char *id = luaL_checkstring(L, 1);
    int columns = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (columns < 1 || columns > 16) {
        return luaL_error(L, "ui.table: 1 to 16 columns");
    }
    if (ImGui::BeginTable(id, columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        int status = call_body(L, 3);
        ImGui::EndTable();
        if (status != LUA_OK) {
            return lua_error(L);
        }
    }
    return 0;
}

int ui_next_column(lua_State *L) {
    check_frame(L);
    ImGui::TableNextColumn();
    return 0;
}

int ui_text(lua_State *L) {
    check_frame(L);
    ImGui::TextUnformatted(luaL_checkstring(L, 1));
    return 0;
}

int ui_text_wrapped(lua_State *L) {
    check_frame(L);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(luaL_checkstring(L, 1));
    ImGui::PopTextWrapPos();
    return 0;
}

int ui_text_disabled(lua_State *L) {
    check_frame(L);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(luaL_checkstring(L, 1));
    ImGui::PopStyleColor();
    return 0;
}

// ui.text_colored(text, r, g, b[, a]) with components 0..1.
int ui_text_colored(lua_State *L) {
    check_frame(L);
    const char *text = luaL_checkstring(L, 1);
    ImVec4 color((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4),
                 (float)luaL_optnumber(L, 5, 1.0));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    return 0;
}

int ui_button(lua_State *L) {
    check_frame(L);
    const char *label = luaL_checkstring(L, 1);
    ImVec2 size((float)luaL_optnumber(L, 2, 0) * g_scale, (float)luaL_optnumber(L, 3, 0) * g_scale);
    lua_pushboolean(L, ImGui::Button(label, size));
    return 1;
}

int ui_selectable(lua_State *L) {
    check_frame(L);
    lua_pushboolean(L, ImGui::Selectable(luaL_checkstring(L, 1), lua_toboolean(L, 2)));
    return 1;
}

// ui.checkbox(label, value) -> changed, value
int ui_checkbox(lua_State *L) {
    check_frame(L);
    bool value = lua_toboolean(L, 2);
    bool changed = ImGui::Checkbox(luaL_checkstring(L, 1), &value);
    lua_pushboolean(L, changed);
    lua_pushboolean(L, value);
    return 2;
}

// ui.slider(label, value, min, max[, integer]) -> changed, value
int ui_slider(lua_State *L) {
    check_frame(L);
    const char *label = luaL_checkstring(L, 1);
    bool changed;
    if (lua_toboolean(L, 5)) {
        int value = (int)luaL_checkinteger(L, 2);
        changed = ImGui::SliderInt(label, &value, (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4));
        lua_pushboolean(L, changed);
        lua_pushinteger(L, value);
    } else {
        float value = (float)luaL_checknumber(L, 2);
        changed = ImGui::SliderFloat(label, &value, (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
        lua_pushboolean(L, changed);
        lua_pushnumber(L, value);
    }
    return 2;
}

// ui.combo(label, index, items) -> changed, index (1-based)
int ui_combo(lua_State *L) {
    check_frame(L);
    const char *label = luaL_checkstring(L, 1);
    int index = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    int count = (int)lua_rawlen(L, 3);
    lua_rawgeti(L, 3, index);
    const char *preview = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 3, i);
            if (ImGui::Selectable(luaL_tolstring(L, -1, nullptr), i == index)) {
                index = i;
                changed = true;
            }
            lua_pop(L, 2);
        }
        ImGui::EndCombo();
    }
    lua_pop(L, 1);
    lua_pushboolean(L, changed);
    lua_pushinteger(L, index);
    return 2;
}

struct HistoryState {
    int position = -1;  // -1: editing a new line
};
std::unordered_map<ImGuiID, HistoryState> g_history;

struct InputContext {
    lua_State *L;
    int history;  // stack index of the history table, or 0
    HistoryState *state;
};

int input_callback(ImGuiInputTextCallbackData *data) {
    InputContext *ctx = static_cast<InputContext *>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || !ctx->history) {
        return 0;
    }
    lua_State *L = ctx->L;
    int count = (int)lua_rawlen(L, ctx->history);
    int &pos = ctx->state->position;
    int previous = pos;
    if (data->EventKey == ImGuiKey_UpArrow) {
        pos = pos < 0 ? count - 1 : (pos > 0 ? pos - 1 : 0);
    } else if (data->EventKey == ImGuiKey_DownArrow && pos >= 0) {
        pos = pos + 1 >= count ? -1 : pos + 1;
    }
    if (previous != pos) {
        const char *text = "";
        if (pos >= 0) {
            lua_rawgeti(L, ctx->history, pos + 1);
            text = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        }
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, text);
        if (pos >= 0) {
            lua_pop(L, 1);
        }
    }
    return 0;
}

// ui.input(label, text, opts) -> changed (or submitted with opts.submit), text.
// opts: hint, submit (Enter returns true), history (array of earlier lines for Up/Down), width, focus.
int ui_input(lua_State *L) {
    check_frame(L);
    const char *label = luaL_checkstring(L, 1);
    size_t length = 0;
    const char *text = luaL_optlstring(L, 2, "", &length);
    int opts = lua_istable(L, 3) ? 3 : 0;
    static std::vector<char> buffer;
    buffer.assign(text, text + length);
    buffer.resize(length + 1024, '\0');
    int flags = 0;
    InputContext ctx = {L, 0, nullptr};
    if (opts) {
        if (opt_bool(L, opts, "submit")) {
            flags |= ImGuiInputTextFlags_EnterReturnsTrue;
        }
        lua_getfield(L, opts, "history");
        if (lua_istable(L, -1)) {
            ctx.history = lua_gettop(L);
            flags |= ImGuiInputTextFlags_CallbackHistory;
        } else {
            lua_pop(L, 1);
        }
        if (opt_bool(L, opts, "focus")) {
            ImGui::SetKeyboardFocusHere();
        }
        float width = opt_number(L, opts, "width", 0);
        if (width != 0) {
            ImGui::SetNextItemWidth(width < 0 ? width : width * g_scale);
        }
    }
    ctx.state = &g_history[ImGui::GetID(label)];
    const char *hint = nullptr;
    if (opts) {
        lua_getfield(L, opts, "hint");
        hint = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
    }
    bool result = hint ? ImGui::InputTextWithHint(label, hint, buffer.data(), buffer.size(), flags, input_callback, &ctx)
                       : ImGui::InputText(label, buffer.data(), buffer.size(), flags, input_callback, &ctx);
    if (result && (flags & ImGuiInputTextFlags_EnterReturnsTrue)) {
        ctx.state->position = -1;
    }
    lua_pushboolean(L, result);
    lua_pushstring(L, buffer.data());
    return 2;
}

int ui_same_line(lua_State *L) {
    check_frame(L);
    ImGui::SameLine();
    return 0;
}

int ui_separator(lua_State *L) {
    check_frame(L);
    if (lua_isstring(L, 1)) {
        ImGui::SeparatorText(lua_tostring(L, 1));
    } else {
        ImGui::Separator();
    }
    return 0;
}

int ui_spacing(lua_State *L) {
    check_frame(L);
    ImGui::Spacing();
    return 0;
}

int ui_tooltip(lua_State *L) {
    check_frame(L);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", luaL_checkstring(L, 1));
    }
    return 0;
}

int ui_scroll_to_bottom(lua_State *L) {
    check_frame(L);
    ImGui::SetScrollHereY(1.0f);
    return 0;
}

int ui_at_bottom(lua_State *L) {
    check_frame(L);
    lua_pushboolean(L, ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f);
    return 1;
}

int ui_focus_next(lua_State *L) {
    check_frame(L);
    ImGui::SetKeyboardFocusHere();
    return 0;
}

int ui_item_width(lua_State *L) {
    check_frame(L);
    float width = (float)luaL_checknumber(L, 1);
    ImGui::SetNextItemWidth(width < 0 ? width : width * g_scale);
    return 0;
}

int ui_scale(lua_State *L) {
    lua_pushnumber(L, g_scale);
    return 1;
}

int ui_wants_keyboard(lua_State *L) {
    lua_pushboolean(L, g_context && ImGui::GetIO().WantCaptureKeyboard);
    return 1;
}

// overlay.capture(on): the overlay takes the mouse and keyboard from the game while on.
int overlay_capture(lua_State *L) {
    g_capturing = lua_toboolean(L, 1);
    return 0;
}

int overlay_is_capturing(lua_State *L) {
    lua_pushboolean(L, g_capturing);
    return 1;
}

}  // namespace

extern "C" void overlay_open_lua(void *state) {
    lua_State *L = static_cast<lua_State *>(state);
    static const luaL_Reg ui[] = {
        {"window", ui_window},       {"child", ui_child},
        {"collapsing", ui_collapsing}, {"tree", ui_tree},
        {"tab_bar", ui_tab_bar},     {"tab", ui_tab},
        {"table", ui_table},         {"next_column", ui_next_column},
        {"text", ui_text},           {"text_wrapped", ui_text_wrapped},
        {"text_disabled", ui_text_disabled}, {"text_colored", ui_text_colored},
        {"button", ui_button},       {"selectable", ui_selectable},
        {"checkbox", ui_checkbox},   {"slider", ui_slider},
        {"combo", ui_combo},         {"input", ui_input},
        {"same_line", ui_same_line}, {"separator", ui_separator},
        {"spacing", ui_spacing},     {"tooltip", ui_tooltip},
        {"scroll_to_bottom", ui_scroll_to_bottom}, {"at_bottom", ui_at_bottom},
        {"focus_next", ui_focus_next}, {"item_width", ui_item_width},
        {"scale", ui_scale},         {"wants_keyboard", ui_wants_keyboard},
        {nullptr, nullptr},
    };
    static const luaL_Reg overlay[] = {
        {"capture", overlay_capture},
        {"capturing", overlay_is_capturing},
        {nullptr, nullptr},
    };
    lua_newtable(L);
    luaL_setfuncs(L, ui, 0);
    lua_setglobal(L, "ui");
    lua_newtable(L);
    luaL_setfuncs(L, overlay, 0);
    lua_setglobal(L, "overlay");
}
