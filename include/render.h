/*
 * purpwm+ — GLES2 renderer (src/render.c).
 *
 * Server-side header (includes purpwm.h). Responsibilities:
 *   - shaders for RGBA / RGBX / external-OES textures and solid colours, premultiplied blending;
 *   - client buffer import: wl_shm copies (glTexSubImage2D with damage, GL_BGRA_EXT when
 *     available), EGL_LINUX_DMA_BUF_EXT EGLImages (modifiers when supported) and single-pixel
 *     buffers as solid quads;
 *   - a texture cache for UI purp_pixel_buf_t rasters keyed by (id, generation), evicting entries
 *     unused for PURP_RENDER_PIXEL_CACHE_MAX_AGE frames;
 *   - full monitor composition (see purp_render_monitor_frame for the exact order);
 *   - offscreen FBO targets, top-down glReadPixels readback and libpng encoding.
 *
 * Coordinates: all draw calls take PHYSICAL pixel rectangles with the origin at the top-left of
 * the bound target. `y_flip` selects how that maps to GL: window-system framebuffers (EGL window
 * surfaces, GBM scanout surfaces) need y_flip = true because GL row 0 is their bottom row;
 * offscreen FBO textures use y_flip = false, so row 0 is the logical top and glReadPixels
 * returns rows top-down without reversal.
 *
 * The renderer requires its EGL context to be current on the compositor thread whenever these
 * functions run (backends keep it current; offscreen work calls backend make_current first).
 */
#ifndef PURPWM_RENDER_H
#define PURPWM_RENDER_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "purpwm.h"

#define PURP_RENDER_PIXEL_CACHE_SIZE    64
#define PURP_RENDER_PIXEL_CACHE_MAX_AGE 120   /* frames */
/* Cursor elements are drawn when the pointer is within this many logical px of the monitor. */
#define PURP_RENDER_CURSOR_MARGIN       64

/* A GPU texture holding a surface's or buffer's contents. */
typedef struct purp_texture {
    GLuint id;                     /* 0 for solid (single-pixel) textures */
    GLenum target;                 /* GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES */
    int32_t width;                 /* px */
    int32_t height;
    bool has_alpha;                /* false: sample with the RGBX program */
    bool is_solid;                 /* single-pixel buffer: draw `solid` as a quad */
    float solid[4];                /* straight RGBA */
    EGLImageKHR image;             /* dmabuf import, EGL_NO_IMAGE_KHR otherwise */
    uint32_t shm_format;           /* wl_shm format of the last upload */
} purp_texture_t;

typedef struct purp_gl_program {
    GLuint program;
    GLint a_pos;                   /* vec2 quad corner 0..1 */
    GLint a_texcoord;
    GLint u_proj;                  /* mat3: physical px -> clip space (handles y_flip) */
    GLint u_tex_matrix;            /* mat3: quad corner -> texture coords (crop, transform) */
    GLint u_tex;
    GLint u_alpha;
    GLint u_color;                 /* solid program: premultiplied colour */
} purp_gl_program_t;

/* Cached upload of a purp_pixel_buf_t. */
typedef struct purp_pixel_texture {
    uint64_t buf_id;               /* 0 = free slot */
    uint64_t generation;
    GLuint texture;
    int32_t width;
    int32_t height;
    uint64_t last_used_frame;
} purp_pixel_texture_t;

struct purp_renderer {
    purp_server_t *server;
    EGLDisplay egl_display;
    EGLContext egl_context;

    /* Extensions. */
    PFNEGLCREATEIMAGEKHRPROC egl_create_image;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image;
    PFNEGLQUERYDMABUFFORMATSEXTPROC egl_query_dmabuf_formats;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC egl_query_dmabuf_modifiers;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_image_target_texture;
    bool has_dmabuf_import;            /* EGL_EXT_image_dma_buf_import */
    bool has_dmabuf_modifiers;         /* EGL_EXT_image_dma_buf_import_modifiers */
    bool has_external_texture;         /* GL_OES_EGL_image_external */
    bool has_bgra;                     /* GL_EXT_texture_format_BGRA8888 */
    bool has_unpack_subimage;          /* GL_EXT_unpack_subimage (partial shm uploads) */

    purp_gl_program_t prog_rgba;
    purp_gl_program_t prog_rgbx;
    purp_gl_program_t prog_external;
    purp_gl_program_t prog_solid;
    GLuint quad_vbo;

    /* Current target (between purp_render_begin and purp_render_end). */
    int32_t target_width;
    int32_t target_height;
    bool y_flip;
    float proj[9];

    purp_pixel_texture_t pixel_cache[PURP_RENDER_PIXEL_CACHE_SIZE];
    uint64_t frame_seq;
};

/* ---- lifecycle ----------------------------------------------------------------------------- */

/* Compile shaders, resolve extensions; NULL on failure (logged). */
purp_renderer_t *purp_renderer_create(purp_server_t *server, EGLDisplay display,
                                      EGLContext context);
void purp_renderer_destroy(purp_renderer_t *renderer);
/* Importable (format, modifier) pairs for linux-dmabuf (DRM_FORMAT_MOD_INVALID listed for
 * implicit modifiers). `*out` is malloc'd; returns the count. */
size_t purp_renderer_dmabuf_formats(purp_renderer_t *renderer, purp_dmabuf_format_t **out);
/* Whether `attrs` can be imported (zwp_linux_buffer_params_v1.create validation). */
bool purp_renderer_test_dmabuf(purp_renderer_t *renderer, const purp_dmabuf_attributes_t *attrs);

/* ---- client content ------------------------------------------------------------------------ */

/* Commit hook (surface.c): bring surface->texture up to date with the current buffer — copy shm
 * (only the damaged rows when possible, then the buffer may be released), reuse/import the dmabuf
 * EGLImage cached on the buffer, or describe a single-pixel solid. */
bool purp_render_surface_commit(purp_renderer_t *renderer, purp_surface_t *surface);
void purp_render_surface_destroyed(purp_renderer_t *renderer, purp_surface_t *surface);
void purp_render_buffer_destroyed(purp_renderer_t *renderer, purp_buffer_t *buffer);
void purp_texture_destroy(purp_renderer_t *renderer, purp_texture_t *texture);

/* ---- frame primitives (physical px, top-left origin) --------------------------------------- */

void purp_render_begin(purp_renderer_t *renderer, int32_t width, int32_t height, bool y_flip);
void purp_render_clear(purp_renderer_t *renderer, purp_color_t color);
/* Solid rectangle, straight colour (premultiplied in the shader). */
void purp_render_rect(purp_renderer_t *renderer, purp_rect_t rect, purp_color_t color);
/* Textured quad: `src` in texture px (crop / viewport source), `dst` target px, optional
 * scissor `clip` (target px), `transform` = enum wl_output_transform of the buffer. */
void purp_render_texture(purp_renderer_t *renderer, const purp_texture_t *texture,
                         purp_rectf_t src, purp_rect_t dst, float alpha, const purp_rect_t *clip,
                         int32_t transform);
/* Upload-if-needed and draw a UI raster at `dst` (usually its own size). */
void purp_render_pixel_buf(purp_renderer_t *renderer, const purp_pixel_buf_t *buf,
                           purp_rect_t dst, float alpha);
/* Draw a raw GL texture (nested host composition of offscreen tiles). */
void purp_render_gl_texture(purp_renderer_t *renderer, GLuint texture, int32_t tex_width,
                            int32_t tex_height, purp_rect_t src, purp_rect_t dst, float alpha);
/* Finish the frame (scissor reset, pixel-cache aging). */
void purp_render_end(purp_renderer_t *renderer);

/* ---- composition --------------------------------------------------------------------------- */

/* Draw a surface tree (subsurfaces, viewport crop, buffer scale/transform) whose root's buffer
 * origin is at global logical `origin`, on a monitor whose logical origin is `monitor_origin` and
 * scale is `scale`. `clip` (physical, may be NULL) crops windows to their content box. */
void purp_render_surface_tree(purp_renderer_t *renderer, purp_surface_t *root,
                              purp_pointf_t origin, purp_point_t monitor_origin, double scale,
                              float alpha, const purp_rect_t *clip);
/* Four solid border rectangles (top, bottom, left, right) of `width` logical px inside `geo`
 * (global logical), drawn on the monitor at `monitor_origin`/`scale`. */
void purp_render_border(purp_renderer_t *renderer, purp_rect_t geo, int32_t width,
                        purp_color_t color, purp_point_t monitor_origin, double scale);
/*
 * Compose monitor `mon` into the bound target (fb_width x fb_height physical px), back to front:
 *   locked: only the lock screen raster (nothing else, not even the cursor);
 *   otherwise: clear PURP_COLOR_BG_ROOT; wallpaper; Background/Bottom layers; windows in reverse
 *   purp_wm_stacking_order (frames overlapping the monitor grown by BORDER_WIDTH + 4): fullscreen
 *   windows over an opaque black backdrop at alpha 1, others with 2px borders
 *   (active/inactive colour) plus a 1px #C084FC glow just outside the focused frame, content
 *   cropped to the content box at opacity override / OPACITY_ACTIVE / OPACITY_INACTIVE;
 *   the status bar (when visible); Top/Overlay layers (keepalive skipped, alpha 0.96); popups
 *   (alpha 0.96, above layers); notification toasts and the help overlay (focused monitor only);
 *   the DnD icon (alpha 0.92); the cursor (client surface, theme shape or built-in arrow) unless
 *   `hide_cursor` or the pointer is farther than PURP_RENDER_CURSOR_MARGIN px away.
 */
bool purp_render_monitor_frame(purp_server_t *server, size_t mon, int32_t fb_width,
                               int32_t fb_height, bool y_flip, bool hide_cursor);

/* ---- offscreen targets, readback, PNG ------------------------------------------------------ */

bool purp_render_offscreen_ensure(purp_renderer_t *renderer, purp_offscreen_t *target,
                                  int32_t width, int32_t height);
void purp_render_offscreen_release(purp_renderer_t *renderer, purp_offscreen_t *target);
/*
 * Render monitor `mon` into a temporary FBO (y_flip = false) at its physical size and read back
 * `region` (global logical; NULL = whole monitor) converted to physical px and clipped to the
 * monitor, top-down, as RGBA8 with alpha forced to 255. `*rgba` is malloc'd (w*h*4).
 */
bool purp_render_capture_rgba(purp_server_t *server, size_t mon, const purp_rect_t *region,
                              bool hide_cursor, uint8_t **rgba, uint32_t *width,
                              uint32_t *height);
/* Encode RGBA8 (8-bit, no interlace) with libpng into a new blob. */
bool purp_png_encode_rgba(const uint8_t *rgba, uint32_t width, uint32_t height,
                          purp_blob_t **out);

#endif /* PURPWM_RENDER_H */
