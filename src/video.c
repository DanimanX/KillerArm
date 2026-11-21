// Display output (see video.h). Supports Desktop OpenGL 2.1 / GLSL 1.20 and
// OpenGL ES 2.0 / GLSL ES 1.00 (for ARM handhelds such as the Anbernic RG34XX),
// falling back to plain SDL rendering. All GL entry points are fetched through
// SDL, so nothing links against libGL / libGLESv2 directly.
#include "video.h"
#include "config.h"
#include "gl_funcs.h"
#include "shaders.h"
#include <stdio.h>
#include <stdlib.h>

static SDL_Window *win;
static bool use_gl, use_gles;

/* ---------------------------------------------------------------- */
/* SDL_Renderer fallback                                            */
/* ---------------------------------------------------------------- */

static SDL_Renderer *ren;
static SDL_Texture *ren_game, *ren_game565, *ren_over;

static bool sdl_init(void)
{
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) return false;
    ren_game = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    ren_game565 = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    ren_over = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    SDL_SetTextureBlendMode(ren_over, SDL_BLENDMODE_BLEND);
    return ren_game && ren_over;
}

static void sdl_present_tex(SDL_Texture *tex, const void *pixels, int pitch, const uint32_t *overlay, int filter, int scale_mode)
{
    if (scale_mode == SCALE_STRETCH) SDL_RenderSetLogicalSize(ren, 0, 0);
    else SDL_RenderSetLogicalSize(ren, 320, 240);
    SDL_RenderSetIntegerScale(ren, scale_mode == SCALE_INTEGER);
    SDL_SetTextureScaleMode(tex, filter == FILTER_SMOOTH ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_UpdateTexture(tex, NULL, pixels, pitch);
    if (overlay) SDL_UpdateTexture(ren_over, NULL, overlay, 320 * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    if (overlay) SDL_RenderCopy(ren, ren_over, NULL, NULL);
    SDL_RenderPresent(ren);
}

/* ---------------------------------------------------------------- */
/* OpenGL / OpenGL ES 2.0                                           */
/* ---------------------------------------------------------------- */

#define GL_DEFINE(ret, name, args) ret (APIENTRY *p_##name) args;
GL_FUNCS(GL_DEFINE)

static SDL_GLContext ctx;
static GLuint tex_game, tex_game565, tex_over;
static GLuint prog[NFILTERS], prog_over;
static GLint u_src[NFILTERS], u_dst[NFILTERS];
static int last_filter = -1;

// Desktop GLSL 1.20 shaders
static const char *vs_src_gl =
    "#version 120\n"
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5); gl_Position = vec4(pos, 0.0, 1.0); }\n";

// OpenGL ES 2.0 / GLSL ES 1.00 vertex shader
static const char *vs_src_gles =
    "#version 100\n"
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5); gl_Position = vec4(pos, 0.0, 1.0); }\n";

// Shared header: tex = source image, src = its size, dst = viewport size in pixels.
#define FS_HEAD_GL \
    "#version 120\n" \
    "uniform sampler2D tex;\n" \
    "uniform vec2 src, dst;\n" \
    "varying vec2 uv;\n"

#define FS_HEAD_GLES \
    "#version 100\n" \
    "precision highp float;\n" \
    "uniform sampler2D tex;\n" \
    "uniform vec2 src, dst;\n" \
    "varying vec2 uv;\n"

// Crisp pixels without uneven widths at non-integer scales: nearest
// sampling with a one-output-pixel blend at each texel edge.
#define FS_SHARP_BODY \
    "void main() {\n" \
    "    vec2 t = uv * src;\n" \
    "    vec2 scale = max(floor(dst / src), vec2(1.0));\n" \
    "    vec2 f = fract(t) - 0.5;\n" \
    "    vec2 region = 0.5 - 0.5 / scale;\n" \
    "    vec2 g = (f - clamp(f, -region, region)) * scale + 0.5;\n" \
    "    gl_FragColor = texture2D(tex, (floor(t) + g) / src);\n" \
    "}\n"

// Catmull-Rom bicubic with a light unsharp mask: smooth edges, keeps the
// detail of the pre-rendered art. Unrolled rows avoid dynamic vec4 indexing
// so strict GLSL ES 1.00 drivers (Mali, Lima, PowerVR) compile it fast.
#define FS_SMOOTH_BODY \
    "vec3 tap(vec2 p) { return texture2D(tex, (p + 0.5) / src).rgb; }\n" \
    "vec4 w(float x) {\n" \
    "    float x2 = x * x, x3 = x2 * x;\n" \
    "    return vec4(-0.5 * x3 + x2 - 0.5 * x, 1.5 * x3 - 2.5 * x2 + 1.0,\n" \
    "                -1.5 * x3 + 2.0 * x2 + 0.5 * x, 0.5 * x3 - 0.5 * x2);\n" \
    "}\n" \
    "vec3 row4(vec2 p, vec4 wx) {\n" \
    "    return tap(p + vec2(-1.0, 0.0)) * wx.x + tap(p) * wx.y +\n" \
    "           tap(p + vec2(1.0, 0.0)) * wx.z + tap(p + vec2(2.0, 0.0)) * wx.w;\n" \
    "}\n" \
    "void main() {\n" \
    "    vec2 t = uv * src - 0.5;\n" \
    "    vec2 i = floor(t), f = t - i;\n" \
    "    vec4 wx = w(f.x), wy = w(f.y);\n" \
    "    vec3 c = row4(i + vec2(0.0, -1.0), wx) * wy.x +\n" \
    "             row4(i, wx) * wy.y +\n" \
    "             row4(i + vec2(0.0, 1.0), wx) * wy.z +\n" \
    "             row4(i + vec2(0.0, 2.0), wx) * wy.w;\n" \
    "    vec3 blur = (tap(i) + tap(i + vec2(1.0, 0.0)) + tap(i + vec2(0.0, 1.0)) + tap(i + vec2(1.0))) * 0.25;\n" \
    "    gl_FragColor = vec4(clamp(c + (c - blur) * 0.25, 0.0, 1.0), 1.0);\n" \
    "}\n"

// Gaussian beam per source line; wider (brighter) beams for bright pixels,
// as on a real tube.
#define SCANLINE_FN \
    "vec3 lin(vec3 c) { return c * c; }\n" \
    "vec3 sample_line(vec2 p) {\n" \
    "    float x = p.x * src.x;\n" \
    "    float fx = fract(x) - 0.5;\n" \
    "    float sx = (floor(x) + 0.5 + clamp(fx * 3.0, -0.5, 0.5)) / src.x;\n" \
    "    float y = p.y * src.y - 0.5;\n" \
    "    float fy = fract(y);\n" \
    "    float y0 = (floor(y) + 0.5) / src.y, y1 = y0 + 1.0 / src.y;\n" \
    "    vec3 a = lin(texture2D(tex, vec2(sx, y0)).rgb), b = lin(texture2D(tex, vec2(sx, y1)).rgb);\n" \
    "    vec3 wa = exp(-fy * fy / (2.0 * mix(vec3(0.09), vec3(0.17), a)));\n" \
    "    vec3 wb = exp(-(1.0 - fy) * (1.0 - fy) / (2.0 * mix(vec3(0.09), vec3(0.17), b)));\n" \
    "    return a * wa + b * wb;\n" \
    "}\n"

#define FS_SCANLINES_BODY SCANLINE_FN \
    "void main() { gl_FragColor = vec4(sqrt(sample_line(uv) * 1.45), 1.0); }\n"

// Scanlines plus an aperture-grille mask, slight curvature and vignette.
#define FS_CRT_BODY SCANLINE_FN \
    "void main() {\n" \
    "    vec2 c = uv * 2.0 - 1.0;\n" \
    "    c *= 1.0 + vec2(0.035, 0.05) * (c.yx * c.yx);\n" \
    "    vec2 p = c * 0.5 + 0.5;\n" \
    "    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n" \
    "    vec3 col = sample_line(p);\n" \
    "    float m = mod(gl_FragCoord.x, 3.0);\n" \
    "    vec3 mask = vec3(0.72);\n" \
    "    if (m < 1.0) mask.r = 1.12; else if (m < 2.0) mask.g = 1.12; else mask.b = 1.12;\n" \
    "    if (dst.x < 960.0) mask = vec3(1.0);\n" \
    "    col *= mask * 1.6;\n" \
    "    float v = smoothstep(1.35, 0.6, length(c * vec2(0.9, 1.0)));\n" \
    "    gl_FragColor = vec4(sqrt(col) * mix(0.75, 1.0, v), 1.0);\n" \
    "}\n"

static const char *const fs_gl[NFILTERS] = {
    FS_HEAD_GL FS_SHARP_BODY,
    FS_HEAD_GL FS_SMOOTH_BODY,
    FS_HEAD_GL FS_SCANLINES_BODY,
    FS_HEAD_GL FS_CRT_BODY,
};

static const char *const fs_gles[NFILTERS] = {
    FS_HEAD_GLES FS_SHARP_BODY,
    FS_HEAD_GLES FS_SMOOTH_BODY,
    FS_HEAD_GLES FS_SCANLINES_BODY,
    FS_HEAD_GLES FS_CRT_BODY,
};

static const char *fs_overlay_gl = FS_HEAD_GL
    "void main() { gl_FragColor = texture2D(tex, uv); }\n";

// On GLES 2.0 core, GL_BGRA is not guaranteed; the 0xAARRGGBB overlay is
// uploaded as GL_RGBA and swizzled back in the fragment shader for free.
static const char *fs_overlay_gles = FS_HEAD_GLES
    "void main() { gl_FragColor = texture2D(tex, uv).bgra; }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "video: shader error:\n%s\n", log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
        return 0;
    }
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glBindAttribLocation(p, 0, "pos");
    p_glLinkProgram(p);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "video: link error:\n%s\n", log);
        p_glDeleteProgram(p);
        return 0;
    }
    p_glUseProgram(p);
    p_glUniform1i(p_glGetUniformLocation(p, "tex"), 0);
    return p;
}

static GLuint make_texture(GLint intfmt, GLenum fmt, GLenum type)
{
    GLuint t;
    p_glGenTextures(1, &t);
    p_glBindTexture(GL_TEXTURE_2D, t);
    p_glTexImage2D(GL_TEXTURE_2D, 0, intfmt, 320, 240, 0, fmt, type, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

static bool gl_init(bool gles)
{
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "video: no %s context: %s\n", gles ? "OpenGL ES" : "OpenGL", SDL_GetError()); return false; }
#define LOAD(ret, name, args) \
    p_##name = (ret (APIENTRY *) args)SDL_GL_GetProcAddress(#name); \
    if (!p_##name) { fprintf(stderr, "video: missing %s\n", #name); return false; }
    GL_FUNCS(LOAD)
#undef LOAD
    const char *vs = gles ? vs_src_gles : vs_src_gl;
    const char *const *fs = gles ? fs_gles : fs_gl;
    for (int i = 0; i < NFILTERS; i++) {
        if (!(prog[i] = link(vs, fs[i]))) return false;
        u_src[i] = p_glGetUniformLocation(prog[i], "src");
        u_dst[i] = p_glGetUniformLocation(prog[i], "dst");
    }
    if (!(prog_over = link(vs, gles ? fs_overlay_gles : fs_overlay_gl))) return false;
    tex_game = make_texture(GL_RGBA, gles ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE);
    tex_game565 = make_texture(GL_RGB, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
    tex_over = make_texture(GL_RGBA, gles ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    last_filter = -1;
    use_gles = gles;
    return true;
}

// Largest rectangle for the chosen scaling mode, centred.
static SDL_Rect viewport(int w, int h, int scale_mode)
{
    SDL_Rect r = { 0, 0, w, h };
    if (scale_mode == SCALE_STRETCH) return r;
    int vw = w, vh = w * 3 / 4;
    if (vh > h) { vh = h; vw = h * 4 / 3; }
    if (scale_mode == SCALE_INTEGER && vw >= 320 && vh >= 240) {
        int k = vw / 320 < vh / 240 ? vw / 320 : vh / 240;
        vw = 320 * k; vh = 240 * k;
    }
    r.x = (w - vw) / 2; r.y = (h - vh) / 2; r.w = vw; r.h = vh;
    return r;
}

// Convert 0x00RRGGBB -> 0xFFBBGGRR for core GLES GL_RGBA uploads (rare path:
// built-in live gameplay uses the RGB565 fast path instead).
static void xrgb_to_rgba(uint32_t *dst, const uint32_t *src)
{
    for (int i = 0; i < 320 * 240; i++) {
        uint32_t p = src[i];
        dst[i] = 0xff000000u | ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu);
    }
}

static void gl_present_common(const uint32_t *game, const uint16_t *game565, const uint32_t *overlay, int filter, int scale_mode)
{
    int w, h;
    SDL_GL_GetDrawableSize(win, &w, &h);
    SDL_Rect v = viewport(w, h, scale_mode);
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };

    p_glViewport(0, 0, w, h);
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glViewport(v.x, h - v.y - v.h, v.w, v.h);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    p_glEnableVertexAttribArray(0);
    p_glActiveTexture(GL_TEXTURE0);

    if (shader_active() && game) {
        shader_render(game, v, h);
        p_glViewport(v.x, h - v.y - v.h, v.w, v.h);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        p_glEnableVertexAttribArray(0);
        p_glDisableVertexAttribArray(1);
        goto overlay_pass;
    }
    if (filter < 0 || filter >= NFILTERS) filter = FILTER_SHARP;
    if (game565) {
        p_glBindTexture(GL_TEXTURE_2D, tex_game565);
        if (last_filter < 0) {
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 240, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, game565);
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    } else {
        p_glBindTexture(GL_TEXTURE_2D, tex_game);
        if (last_filter < 0) {      // every filter does its own interpolation from bilinear taps
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        if (use_gles) {
            static uint32_t rgba[320 * 240];
            xrgb_to_rgba(rgba, game);
            p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 240, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        } else {
            p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 240, GL_BGRA, GL_UNSIGNED_BYTE, game);
        }
    }
    if (last_filter < 0) {
        // ensure both game textures stay linear-sampled once configured
        p_glBindTexture(GL_TEXTURE_2D, game565 ? tex_game : tex_game565);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        p_glBindTexture(GL_TEXTURE_2D, game565 ? tex_game565 : tex_game);
        last_filter = filter;
    }
    p_glUseProgram(prog[filter]);
    p_glUniform2f(u_src[filter], 320.0f, 240.0f);
    p_glUniform2f(u_dst[filter], (GLfloat)v.w, (GLfloat)v.h);
    p_glDisable(GL_BLEND);
    p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

overlay_pass:
    if (overlay) {
        // overlay: on top, unfiltered, alpha-blended
        p_glActiveTexture(GL_TEXTURE0);
        p_glBindTexture(GL_TEXTURE_2D, tex_over);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 240, use_gles ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE, overlay);
        p_glUseProgram(prog_over);
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    SDL_GL_SwapWindow(win);
}

/* ---------------------------------------------------------------- */

static bool open_gl_window(const char *title, int w, int h, Uint32 base_flags, bool gles)
{
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (gles) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    }
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, base_flags | SDL_WINDOW_OPENGL);
    if (!win) return false;
    if (gl_init(gles)) { use_gl = true; return true; }
    if (ctx) { SDL_GL_DeleteContext(ctx); ctx = NULL; }
    SDL_DestroyWindow(win);
    win = NULL;
    return false;
}

SDL_Window *video_open(const char *title, int w, int h)
{
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef UC_HANDHELD
    flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    if (!getenv("KI2_NO_GL")) {
        bool prefer_gles = getenv("KI2_GLES") != NULL;
#if defined(UC_GLES) || defined(UC_HANDHELD) || defined(__aarch64__) || defined(__arm__)
        if (!getenv("KI2_NO_GLES")) prefer_gles = true;
#endif
        if (open_gl_window(title, w, h, flags, prefer_gles)) return win;
        if (open_gl_window(title, w, h, flags, !prefer_gles)) return win;
    }
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!win || !sdl_init()) return NULL;
    return win;
}

void video_set_vsync(bool on)
{
    if (use_gl) SDL_GL_SetSwapInterval(on ? 1 : 0);
    else SDL_RenderSetVSync(ren, on);
}

void video_present(const uint32_t *game, const uint32_t *overlay, int filter, int scale_mode)
{
    if (use_gl) gl_present_common(game, NULL, overlay, filter, scale_mode);
    else sdl_present_tex(ren_game, game, 320 * 4, overlay, filter, scale_mode);
}

void video_present_565(const uint16_t *game565, const uint32_t *overlay, int filter, int scale_mode)
{
    if (use_gl) gl_present_common(NULL, game565, overlay, filter, scale_mode);
    else if (ren_game565) sdl_present_tex(ren_game565, game565, 320 * 2, overlay, filter, scale_mode);
    else {
        static uint32_t tmp[320 * 240];
        for (int i = 0; i < 320 * 240; i++) {
            uint16_t p = game565[i];
            uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            tmp[i] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
        sdl_present_tex(ren_game, tmp, 320 * 4, overlay, filter, scale_mode);
    }
}

const char *video_backend(void) { return use_gl ? (use_gles ? "OpenGL ES 2.0" : "OpenGL") : "SDL renderer"; }
bool video_is_gles(void) { return use_gles; }

bool video_set_shader(const char *path, char *err, size_t errlen)
{
    if (!use_gl) { snprintf(err, errlen, "shaders need OpenGL"); return false; }
    if (!path || !*path) { shader_unload(); return true; }
    return shader_load(path, err, errlen);
}

bool video_shaders_supported(void) { return use_gl; }

void video_close(void)
{
    if (ctx) SDL_GL_DeleteContext(ctx);
    if (ren) SDL_DestroyRenderer(ren);
}
