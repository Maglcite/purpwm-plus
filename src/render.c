/* GLES2 rendering, client buffer import and backend rendering bridge. */
#include "render.h"

#include <drm_fourcc.h>
#include <png.h>
#include <setjmp.h>

static int32_t render_clamp_i32(int64_t value) {
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

/* Buffer caches can outlive their last surface; keep them reachable for renderer teardown. */
struct cached_texture {
    purp_texture_t texture;
    purp_buffer_t *buffer;
    struct cached_texture *next;
    struct cached_texture **previous;
};

/* Public state is first; reusable private staging avoids allocations in steady-state frames. */
struct renderer_storage {
    purp_renderer_t renderer;
    uint8_t *composition_scratch;
    size_t composition_capacity;
    uint8_t *upload_row;
    size_t upload_capacity;
    struct cached_texture *buffer_textures;
};

static bool reserve_bytes(uint8_t **buffer, size_t *capacity, size_t needed) {
    if (needed <= *capacity) return true;
    size_t next = *capacity ? *capacity : 4096;
    while (next < needed) {
        if (next > SIZE_MAX / 2) { next = needed; break; }
        next *= 2;
    }
    uint8_t *grown = realloc(*buffer, next);
    if (!grown) return false;
    *buffer = grown;
    *capacity = next;
    return true;
}

static bool has_extension(const char *extensions, const char *name) {
    if (!extensions || !name || !*name || strchr(name, ' ')) return false;
    size_t len = strlen(name);
    const char *at = extensions;
    while ((at = strstr(at, name))) {
        if ((at == extensions || at[-1] == ' ') && (at[len] == '\0' || at[len] == ' '))
            return true;
        at += len;
    }
    return false;
}

static GLuint compile_shader(GLenum kind, const char *source) {
    GLuint shader = glCreateShader(kind);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint okay = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
    if (!okay) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        PURP_ERROR("GL shader compilation failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool create_program(purp_gl_program_t *program, const char *fragment) {
    static const char vertex[] =
        "attribute vec2 a_pos; attribute vec2 a_texcoord;"
        "uniform mat3 u_proj; uniform mat3 u_tex_matrix; varying vec2 v_uv;"
        "void main() { vec3 p = u_proj * vec3(a_pos, 1.0);"
        "gl_Position = vec4(p.xy, 0.0, 1.0);"
        "v_uv = (u_tex_matrix * vec3(a_texcoord, 1.0)).xy; }";
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    program->program = glCreateProgram();
    glAttachShader(program->program, vs);
    glAttachShader(program->program, fs);
    glLinkProgram(program->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint okay = GL_FALSE;
    glGetProgramiv(program->program, GL_LINK_STATUS, &okay);
    if (!okay) {
        char log[1024];
        glGetProgramInfoLog(program->program, sizeof log, NULL, log);
        PURP_ERROR("GL program linking failed: %s", log);
        glDeleteProgram(program->program);
        program->program = 0;
        return false;
    }
    program->a_pos = glGetAttribLocation(program->program, "a_pos");
    program->a_texcoord = glGetAttribLocation(program->program, "a_texcoord");
    program->u_proj = glGetUniformLocation(program->program, "u_proj");
    program->u_tex_matrix = glGetUniformLocation(program->program, "u_tex_matrix");
    program->u_tex = glGetUniformLocation(program->program, "u_tex");
    program->u_alpha = glGetUniformLocation(program->program, "u_alpha");
    program->u_color = glGetUniformLocation(program->program, "u_color");
    return true;
}

purp_renderer_t *purp_renderer_create(purp_server_t *server, EGLDisplay display,
                                       EGLContext context) {
    struct renderer_storage *storage = calloc(1, sizeof *storage);
    if (!storage) return NULL;
    purp_renderer_t *renderer = &storage->renderer;
    renderer->server = server;
    renderer->egl_display = display;
    renderer->egl_context = context;
    const char *egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
    const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    renderer->egl_create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    renderer->egl_destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    renderer->egl_query_dmabuf_formats =
        (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    renderer->egl_query_dmabuf_modifiers =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    renderer->gl_image_target_texture =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    renderer->has_dmabuf_import = has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import") &&
        renderer->egl_create_image && renderer->egl_destroy_image && renderer->gl_image_target_texture;
    renderer->has_dmabuf_modifiers = renderer->has_dmabuf_import &&
        has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    renderer->has_external_texture = has_extension(gl_extensions, "GL_OES_EGL_image_external");
    renderer->has_bgra = has_extension(gl_extensions, "GL_EXT_texture_format_BGRA8888");
    renderer->has_unpack_subimage = has_extension(gl_extensions, "GL_EXT_unpack_subimage");
    static const char rgba_fragment[] =
        "precision mediump float; varying vec2 v_uv; uniform sampler2D u_tex; uniform float u_alpha;"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_alpha; }";
    static const char rgbx_fragment[] =
        "precision mediump float; varying vec2 v_uv; uniform sampler2D u_tex; uniform float u_alpha;"
        "void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0) * u_alpha; }";
    static const char external_fragment[] =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float; varying vec2 v_uv; uniform samplerExternalOES u_tex;"
        "uniform float u_alpha; uniform vec4 u_color;"
        "void main() { vec4 c = texture2D(u_tex, v_uv);"
        "gl_FragColor = vec4(c.rgb, mix(1.0, c.a, u_color.a)) * u_alpha; }";
    static const char solid_fragment[] =
        "precision mediump float; uniform vec4 u_color;"
        "void main() { gl_FragColor = u_color; }";
    if (!create_program(&renderer->prog_rgba, rgba_fragment) ||
        !create_program(&renderer->prog_rgbx, rgbx_fragment) ||
        !create_program(&renderer->prog_solid, solid_fragment) ||
        (renderer->has_external_texture &&
         !create_program(&renderer->prog_external, external_fragment))) {
        purp_renderer_destroy(renderer);
        return NULL;
    }
    static const GLfloat quad[] = { 0, 0, 0, 0, 1, 0, 1, 0,
                                    0, 1, 0, 1, 1, 1, 1, 1 };
    glGenBuffers(1, &renderer->quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (!renderer->quad_vbo || glGetError() != GL_NO_ERROR) {
        purp_renderer_destroy(renderer);
        return NULL;
    }
    return renderer;
}

void purp_texture_destroy(purp_renderer_t *renderer, purp_texture_t *texture) {
    if (!texture) return;
    if (texture->is_solid || texture->image != EGL_NO_IMAGE_KHR) {
        struct cached_texture *cached = (struct cached_texture *)texture;
        if (cached->previous) {
            *cached->previous = cached->next;
            if (cached->next) cached->next->previous = cached->previous;
        }
        if (cached->buffer) cached->buffer->texture = NULL;
    }
    if (texture->id) glDeleteTextures(1, &texture->id);
    if (texture->image != EGL_NO_IMAGE_KHR && renderer && renderer->egl_destroy_image)
        renderer->egl_destroy_image(renderer->egl_display, texture->image);
    free(texture);
}

void purp_renderer_destroy(purp_renderer_t *renderer) {
    if (!renderer) return;
    if (renderer->server) {
        purp_surface_t *surface;
        wl_list_for_each(surface, &renderer->server->surfaces, link) {
            purp_render_surface_destroyed(renderer, surface);
            if (surface->current.buffer)
                purp_render_buffer_destroyed(renderer, surface->current.buffer);
        }
    }
    for (size_t i = 0; i < PURP_RENDER_PIXEL_CACHE_SIZE; ++i)
        if (renderer->pixel_cache[i].texture)
            glDeleteTextures(1, &renderer->pixel_cache[i].texture);
    if (renderer->quad_vbo) glDeleteBuffers(1, &renderer->quad_vbo);
    if (renderer->prog_rgba.program) glDeleteProgram(renderer->prog_rgba.program);
    if (renderer->prog_rgbx.program) glDeleteProgram(renderer->prog_rgbx.program);
    if (renderer->prog_external.program) glDeleteProgram(renderer->prog_external.program);
    if (renderer->prog_solid.program) glDeleteProgram(renderer->prog_solid.program);
    struct renderer_storage *storage = (struct renderer_storage *)renderer;
    while (storage->buffer_textures)
        purp_texture_destroy(renderer, &storage->buffer_textures->texture);
    free(storage->composition_scratch);
    free(storage->upload_row);
    free(storage);
}

static bool append_format(purp_dmabuf_format_t **formats, size_t *count, size_t *capacity,
                          uint32_t format, uint64_t modifier, bool external_only) {
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 32;
        if (next > SIZE_MAX / sizeof **formats) return false;
        purp_dmabuf_format_t *grown = realloc(*formats, next * sizeof **formats);
        if (!grown) return false;
        *formats = grown;
        *capacity = next;
    }
    (*formats)[(*count)++] = (purp_dmabuf_format_t){ format, modifier, external_only };
    return true;
}

size_t purp_renderer_dmabuf_formats(purp_renderer_t *renderer, purp_dmabuf_format_t **out) {
    *out = NULL;
    if (!renderer || !renderer->has_dmabuf_import) return 0;
    const EGLint fallback[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
                                DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888 };
    EGLint *queried = NULL;
    const EGLint *formats = fallback;
    EGLint num_formats = (EGLint)PURP_ARRAY_LEN(fallback);
    if (renderer->has_dmabuf_modifiers && renderer->egl_query_dmabuf_formats) {
        if (!renderer->egl_query_dmabuf_formats(renderer->egl_display, 0, NULL, &num_formats) ||
            num_formats <= 0) return 0;
        queried = malloc((size_t)num_formats * sizeof *queried);
        if (!queried) return 0;
        if (!renderer->egl_query_dmabuf_formats(renderer->egl_display, num_formats, queried,
                                                &num_formats)) {
            free(queried);
            return 0;
        }
        formats = queried;
    }
    size_t count = 0, capacity = 0;
    for (EGLint i = 0; i < num_formats; ++i) {
        bool implicit_external = false;
        EGLint num_modifiers = 0;
        if (renderer->has_dmabuf_modifiers && renderer->egl_query_dmabuf_modifiers &&
            renderer->egl_query_dmabuf_modifiers(renderer->egl_display, formats[i], 0, NULL,
                                                  NULL, &num_modifiers) && num_modifiers > 0) {
            EGLuint64KHR *modifiers = malloc((size_t)num_modifiers * sizeof *modifiers);
            EGLBoolean *external = malloc((size_t)num_modifiers * sizeof *external);
            if (!modifiers || !external) {
                free(modifiers);
                free(external);
                goto fail;
            }
            if (renderer->egl_query_dmabuf_modifiers(renderer->egl_display, formats[i],
                                                      num_modifiers, modifiers, external,
                                                      &num_modifiers)) {
                implicit_external = true;
                for (EGLint j = 0; j < num_modifiers; ++j) {
                    if (!external[j]) implicit_external = false;
                    if (external[j] && !renderer->has_external_texture) continue;
                    if (!append_format(out, &count, &capacity, (uint32_t)formats[i], modifiers[j],
                                       external[j] != EGL_FALSE)) {
                        free(modifiers);
                        free(external);
                        goto fail;
                    }
                }
            }
            free(modifiers);
            free(external);
        }
        if ((!implicit_external || renderer->has_external_texture) &&
            !append_format(out, &count, &capacity, (uint32_t)formats[i], DRM_FORMAT_MOD_INVALID,
                           implicit_external)) goto fail;
    }
    free(queried);
    return count;
fail:
    free(queried);
    free(*out);
    *out = NULL;
    return 0;
}

static EGLImageKHR import_image(purp_renderer_t *renderer, const purp_dmabuf_attributes_t *attrs) {
    if (!renderer->has_dmabuf_import || attrs->width <= 0 || attrs->height <= 0 ||
        attrs->n_planes < 1 || attrs->n_planes > PURP_DMABUF_MAX_PLANES ||
        (attrs->modifier != DRM_FORMAT_MOD_INVALID && !renderer->has_dmabuf_modifiers))
        return EGL_NO_IMAGE_KHR;
    static const EGLint fd_keys[] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                      EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
    static const EGLint offset_keys[] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                          EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
    static const EGLint pitch_keys[] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                         EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
    static const EGLint mod_lo[] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                     EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
    static const EGLint mod_hi[] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                     EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };
    EGLint list[7 + PURP_DMABUF_MAX_PLANES * 10];
    size_t n = 0;
    list[n++] = EGL_WIDTH; list[n++] = attrs->width;
    list[n++] = EGL_HEIGHT; list[n++] = attrs->height;
    list[n++] = EGL_LINUX_DRM_FOURCC_EXT; list[n++] = (EGLint)attrs->format;
    for (int i = 0; i < attrs->n_planes; ++i) {
        if (attrs->fd[i] < 0) return EGL_NO_IMAGE_KHR;
        list[n++] = fd_keys[i]; list[n++] = attrs->fd[i];
        list[n++] = offset_keys[i]; list[n++] = (EGLint)attrs->offset[i];
        list[n++] = pitch_keys[i]; list[n++] = (EGLint)attrs->stride[i];
        if (attrs->modifier != DRM_FORMAT_MOD_INVALID) {
            list[n++] = mod_lo[i]; list[n++] = (EGLint)(attrs->modifier & UINT32_MAX);
            list[n++] = mod_hi[i]; list[n++] = (EGLint)(attrs->modifier >> 32);
        }
    }
    list[n] = EGL_NONE;
    return renderer->egl_create_image(renderer->egl_display, EGL_NO_CONTEXT,
                                       EGL_LINUX_DMA_BUF_EXT, NULL, list);
}

static void configure_texture(GLenum target) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static GLenum dmabuf_target(purp_renderer_t *renderer, const purp_dmabuf_attributes_t *attrs) {
    if (renderer->server) {
        for (size_t i = 0; i < renderer->server->dmabuf_format_count; ++i) {
            const purp_dmabuf_format_t *format = &renderer->server->dmabuf_formats[i];
            if (format->format == attrs->format && format->modifier == attrs->modifier)
                return format->external_only ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
        }
    }
    return GL_TEXTURE_2D;
}

static purp_texture_t *import_dmabuf(purp_renderer_t *renderer,
                                    const purp_dmabuf_attributes_t *attrs, bool has_alpha) {
    EGLImageKHR image = import_image(renderer, attrs);
    if (image == EGL_NO_IMAGE_KHR) return NULL;
    struct cached_texture *cached = calloc(1, sizeof *cached);
    if (!cached) {
        renderer->egl_destroy_image(renderer->egl_display, image);
        return NULL;
    }
    purp_texture_t *texture = &cached->texture;
    texture->image = image;
    texture->width = attrs->width;
    texture->height = attrs->height;
    texture->has_alpha = has_alpha;
    texture->target = dmabuf_target(renderer, attrs);
    if (texture->target == GL_TEXTURE_EXTERNAL_OES && !renderer->has_external_texture) {
        purp_texture_destroy(renderer, texture);
        return NULL;
    }
    glGenTextures(1, &texture->id);
    glBindTexture(texture->target, texture->id);
    configure_texture(texture->target);
    renderer->gl_image_target_texture(texture->target, image);
    bool okay = texture->id && glGetError() == GL_NO_ERROR;
    glBindTexture(texture->target, 0);
    if (!okay) {
        purp_texture_destroy(renderer, texture);
        return NULL;
    }
    return texture;
}

bool purp_renderer_test_dmabuf(purp_renderer_t *renderer, const purp_dmabuf_attributes_t *attrs) {
    if (!renderer || !attrs) return false;
    purp_texture_t *texture = import_dmabuf(renderer, attrs, true);
    if (!texture) return false;
    purp_texture_destroy(renderer, texture);
    return true;
}

/* Upload a rectangle without copying contiguous rows unless channel swizzling is necessary. */
static bool upload_pixels(purp_renderer_t *renderer, const uint8_t *data, int32_t stride,
                          purp_rect_t rect, bool bgra) {
    if (purp_rect_is_empty(rect)) return true;
    if (!data || stride < 0 || rect.w > stride / 4) return false;
    const uint8_t *first = data + (size_t)rect.y * (size_t)stride + (size_t)rect.x * 4;
    GLenum format = bgra && renderer->has_bgra ? GL_BGRA_EXT : GL_RGBA;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (bgra && !renderer->has_bgra) {
        struct renderer_storage *storage = (struct renderer_storage *)renderer;
        if (!reserve_bytes(&storage->upload_row, &storage->upload_capacity, (size_t)rect.w * 4))
            return false;
        uint8_t *row = storage->upload_row;
        for (int32_t y = 0; y < rect.h; ++y) {
            const uint8_t *source = first + (size_t)y * (size_t)stride;
            for (int32_t x = 0; x < rect.w; ++x) {
                row[x * 4] = source[x * 4 + 2];
                row[x * 4 + 1] = source[x * 4 + 1];
                row[x * 4 + 2] = source[x * 4];
                row[x * 4 + 3] = source[x * 4 + 3];
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y + y, rect.w, 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, row);
        }
    } else if (stride == rect.w * 4 && rect.x == 0) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h, format,
                        GL_UNSIGNED_BYTE, first);
    } else if (renderer->has_unpack_subimage && stride % 4 == 0) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h, format,
                        GL_UNSIGNED_BYTE, first);
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    } else {
        for (int32_t y = 0; y < rect.h; ++y)
            glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y + y, rect.w, 1, format,
                            GL_UNSIGNED_BYTE, first + (size_t)y * (size_t)stride);
    }
    return glGetError() == GL_NO_ERROR;
}

static bool surface_owns_texture(const purp_surface_t *surface) {
    return surface->texture && !surface->texture->is_solid &&
        surface->texture->image == EGL_NO_IMAGE_KHR;
}

void purp_render_surface_destroyed(purp_renderer_t *renderer, purp_surface_t *surface) {
    if (!surface) return;
    if (surface_owns_texture(surface)) purp_texture_destroy(renderer, surface->texture);
    surface->texture = NULL;
}

void purp_render_buffer_destroyed(purp_renderer_t *renderer, purp_buffer_t *buffer) {
    if (!buffer || !buffer->texture) return;
    if (renderer && renderer->server) {
        purp_surface_t *surface;
        wl_list_for_each(surface, &renderer->server->surfaces, link)
            if (surface->texture == buffer->texture) surface->texture = NULL;
    }
    purp_texture_destroy(renderer, buffer->texture);
    buffer->texture = NULL;
}

bool purp_render_surface_commit(purp_renderer_t *renderer, purp_surface_t *surface) {
    if (!renderer || !surface) return false;
    purp_buffer_t *buffer = surface->current.buffer;
    if (!buffer) {
        /* A state-only commit retains a copied shm texture; an explicit detach clears it. */
        if (surface->current.committed & PURP_SURFACE_STATE_BUFFER)
            purp_render_surface_destroyed(renderer, surface);
        return true;
    }
    if (buffer->kind != PURP_BUFFER_SHM) {
        purp_render_surface_destroyed(renderer, surface);
        if (!buffer->texture && buffer->kind == PURP_BUFFER_DMABUF)
            buffer->texture = import_dmabuf(renderer, &buffer->dmabuf, buffer->has_alpha);
        else if (!buffer->texture && buffer->kind == PURP_BUFFER_SINGLE_PIXEL) {
            struct cached_texture *cached = calloc(1, sizeof *cached);
            buffer->texture = cached ? &cached->texture : NULL;
            if (buffer->texture) {
                buffer->texture->is_solid = true;
                buffer->texture->has_alpha = buffer->has_alpha;
                buffer->texture->width = buffer->width;
                buffer->texture->height = buffer->height;
                memcpy(buffer->texture->solid, buffer->single_pixel, sizeof buffer->single_pixel);
            }
        }
        if (buffer->texture) {
            struct cached_texture *cached = (struct cached_texture *)buffer->texture;
            if (!cached->previous) {
                struct renderer_storage *storage = (struct renderer_storage *)renderer;
                cached->buffer = buffer;
                cached->next = storage->buffer_textures;
                cached->previous = &storage->buffer_textures;
                if (cached->next) cached->next->previous = &cached->next;
                storage->buffer_textures = cached;
            }
        }
        surface->texture = buffer->texture;
        return surface->texture != NULL;
    }
    struct wl_shm_buffer *shm = buffer->resource ? wl_shm_buffer_get(buffer->resource) : NULL;
    if (!shm) return false;
    uint32_t format = wl_shm_buffer_get_format(shm);
    bool bgra = format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888;
    if (!bgra && format != WL_SHM_FORMAT_ABGR8888 && format != WL_SHM_FORMAT_XBGR8888)
        return false;
    int32_t width = wl_shm_buffer_get_width(shm), height = wl_shm_buffer_get_height(shm);
    int32_t stride = wl_shm_buffer_get_stride(shm);
    if (width <= 0 || height <= 0 || width > INT32_MAX / 4 || stride < width * 4) return false;
    bool fresh = !surface_owns_texture(surface) || surface->texture->width != width ||
        surface->texture->height != height || surface->texture->shm_format != format;
    if (fresh) {
        purp_render_surface_destroyed(renderer, surface);
        surface->texture = calloc(1, sizeof *surface->texture);
        if (!surface->texture) return false;
        surface->texture->target = GL_TEXTURE_2D;
        surface->texture->width = width;
        surface->texture->height = height;
        surface->texture->shm_format = format;
        glGenTextures(1, &surface->texture->id);
        glBindTexture(GL_TEXTURE_2D, surface->texture->id);
        configure_texture(GL_TEXTURE_2D);
        GLenum upload_format = bgra && renderer->has_bgra ? GL_BGRA_EXT : GL_RGBA;
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)upload_format, width, height, 0, upload_format,
                     GL_UNSIGNED_BYTE, NULL);
    } else glBindTexture(GL_TEXTURE_2D, surface->texture->id);
    surface->texture->has_alpha = format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_ABGR8888;
    wl_shm_buffer_begin_access(shm);
    const uint8_t *data = wl_shm_buffer_get_data(shm);
    bool okay = true;
    purp_rect_t bounds = purp_rect_make(0, 0, width, height);
    if (!fresh && purp_region_is_empty(&surface->current.surface_damage) &&
        !purp_region_is_empty(&surface->current.buffer_damage)) {
        for (size_t i = 0; i < surface->current.buffer_damage.count && okay; ++i) {
            purp_rect_t damage;
            if (purp_rect_intersect(bounds, surface->current.buffer_damage.rects[i], &damage))
                okay = upload_pixels(renderer, data, stride, damage, bgra);
        }
    } else okay = upload_pixels(renderer, data, stride, bounds, bgra);
    wl_shm_buffer_end_access(shm);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!okay) purp_render_surface_destroyed(renderer, surface);
    /* The GPU owns a copy; clients may immediately reuse the shared-memory storage. */
    surface->current.buffer = NULL;
    purp_buffer_unlock(buffer);
    return okay;
}

void purp_render_begin(purp_renderer_t *renderer, int32_t width, int32_t height, bool y_flip) {
    if (!renderer) return;
    renderer->target_width = purp_max_i32(width, 1);
    renderer->target_height = purp_max_i32(height, 1);
    renderer->y_flip = y_flip;
    memset(renderer->proj, 0, sizeof renderer->proj);
    renderer->proj[0] = 2.0f / (float)renderer->target_width;
    renderer->proj[4] = (y_flip ? -2.0f : 2.0f) / (float)renderer->target_height;
    renderer->proj[6] = -1.0f;
    renderer->proj[7] = y_flip ? 1.0f : -1.0f;
    renderer->proj[8] = 1.0f;
    ++renderer->frame_seq;
    glViewport(0, 0, renderer->target_width, renderer->target_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
}

void purp_render_clear(purp_renderer_t *renderer, purp_color_t color) {
    if (!renderer) return;
    glDisable(GL_SCISSOR_TEST);
    glClearColor(color.r * color.a, color.g * color.a, color.b * color.a, color.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void draw_quad(purp_renderer_t *renderer, const purp_gl_program_t *program,
                      purp_rect_t rect, const float *tex_matrix) {
    float proj[9];
    memcpy(proj, renderer->proj, sizeof proj);
    proj[0] *= (float)rect.w;
    proj[4] *= (float)rect.h;
    proj[6] += renderer->proj[0] * (float)rect.x;
    proj[7] += renderer->proj[4] * (float)rect.y;
    glUniformMatrix3fv(program->u_proj, 1, GL_FALSE, proj);
    if (program->u_tex_matrix >= 0 && tex_matrix)
        glUniformMatrix3fv(program->u_tex_matrix, 1, GL_FALSE, tex_matrix);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->quad_vbo);
    glEnableVertexAttribArray((GLuint)program->a_pos);
    glVertexAttribPointer((GLuint)program->a_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
    if (program->a_texcoord >= 0) {
        glEnableVertexAttribArray((GLuint)program->a_texcoord);
        glVertexAttribPointer((GLuint)program->a_texcoord, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)program->a_pos);
    if (program->a_texcoord >= 0) glDisableVertexAttribArray((GLuint)program->a_texcoord);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void purp_render_rect(purp_renderer_t *renderer, purp_rect_t rect, purp_color_t color) {
    if (!renderer || purp_rect_is_empty(rect)) return;
    glUseProgram(renderer->prog_solid.program);
    glUniform4f(renderer->prog_solid.u_color, color.r * color.a, color.g * color.a,
                color.b * color.a, color.a);
    draw_quad(renderer, &renderer->prog_solid, rect, NULL);
}

static void transform_uv(int32_t transform, double x, double y, double *u, double *v) {
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_90: *u = y; *v = 1.0 - x; break;
    case WL_OUTPUT_TRANSFORM_180: *u = 1.0 - x; *v = 1.0 - y; break;
    case WL_OUTPUT_TRANSFORM_270: *u = 1.0 - y; *v = x; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED: *u = 1.0 - x; *v = y; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90: *u = y; *v = x; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: *u = x; *v = 1.0 - y; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: *u = 1.0 - y; *v = 1.0 - x; break;
    default: *u = x; *v = y; break;
    }
}

void purp_render_texture(purp_renderer_t *renderer, const purp_texture_t *texture,
                         purp_rectf_t src, purp_rect_t dst, float alpha, const purp_rect_t *clip,
                         int32_t transform) {
    if (!renderer || !texture || texture->width <= 0 || texture->height <= 0 ||
        purp_rect_is_empty(dst) || src.w <= 0 || src.h <= 0 || alpha <= 0) return;
    if (clip) {
        purp_rect_t clipped;
        if (!purp_rect_intersect(*clip, purp_rect_make(0, 0, renderer->target_width,
                                                     renderer->target_height), &clipped)) return;
        glEnable(GL_SCISSOR_TEST);
        glScissor(clipped.x, renderer->y_flip ? renderer->target_height - clipped.y - clipped.h :
                  clipped.y, clipped.w, clipped.h);
    }
    if (texture->is_solid) {
        purp_color_t color = { texture->solid[0], texture->solid[1], texture->solid[2],
                               texture->solid[3] * alpha };
        purp_render_rect(renderer, dst, color);
    } else if (texture->id) {
        const purp_gl_program_t *program = texture->target == GL_TEXTURE_EXTERNAL_OES ?
            &renderer->prog_external : (texture->has_alpha ? &renderer->prog_rgba : &renderer->prog_rgbx);
        if (!program->program) { glDisable(GL_SCISSOR_TEST); return; }
        double x0 = src.x / texture->width, y0 = src.y / texture->height;
        double x1 = (src.x + src.w) / texture->width, y1 = (src.y + src.h) / texture->height;
        double u0, v0, ux, vx, uy, vy;
        transform_uv(transform, x0, y0, &u0, &v0);
        transform_uv(transform, x1, y0, &ux, &vx);
        transform_uv(transform, x0, y1, &uy, &vy);
        float matrix[] = { (float)(ux - u0), (float)(vx - v0), 0,
                           (float)(uy - u0), (float)(vy - v0), 0, (float)u0, (float)v0, 1 };
        glUseProgram(program->program);
        glUniform1i(program->u_tex, 0);
        glUniform1f(program->u_alpha, purp_clamp_f32(alpha, 0, 1));
        if (program->u_color >= 0) glUniform4f(program->u_color, 0, 0, 0, texture->has_alpha ? 1 : 0);
        glBindTexture(texture->target, texture->id);
        draw_quad(renderer, program, dst, matrix);
        glBindTexture(texture->target, 0);
    }
    glDisable(GL_SCISSOR_TEST);
}

void purp_render_pixel_buf(purp_renderer_t *renderer, const purp_pixel_buf_t *buf,
                           purp_rect_t dst, float alpha) {
    if (!renderer || !buf || !buf->data || !buf->id || buf->width <= 0 || buf->height <= 0 ||
        buf->width > INT32_MAX / 4 || buf->stride < buf->width * 4) return;
    purp_pixel_texture_t *slot = NULL, *oldest = &renderer->pixel_cache[0];
    for (size_t i = 0; i < PURP_RENDER_PIXEL_CACHE_SIZE; ++i) {
        purp_pixel_texture_t *entry = &renderer->pixel_cache[i];
        if (entry->buf_id == buf->id) { slot = entry; break; }
        if (!entry->buf_id) oldest = entry;
        else if (oldest->buf_id && entry->last_used_frame < oldest->last_used_frame) oldest = entry;
    }
    if (!slot) {
        slot = oldest;
        if (slot->texture) glDeleteTextures(1, &slot->texture);
        memset(slot, 0, sizeof *slot);
        slot->buf_id = buf->id;
        glGenTextures(1, &slot->texture);
    }
    glBindTexture(GL_TEXTURE_2D, slot->texture);
    configure_texture(GL_TEXTURE_2D);
    bool resized = slot->width != buf->width || slot->height != buf->height;
    if (resized) {
        GLenum format = renderer->has_bgra ? GL_BGRA_EXT : GL_RGBA;
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, buf->width, buf->height, 0,
                     format, GL_UNSIGNED_BYTE, NULL);
        slot->width = buf->width;
        slot->height = buf->height;
    }
    if (resized || slot->generation != buf->generation) {
        if (!upload_pixels(renderer, buf->data, buf->stride,
                           purp_rect_make(0, 0, buf->width, buf->height), true)) {
            glBindTexture(GL_TEXTURE_2D, 0);
            if (slot->texture) glDeleteTextures(1, &slot->texture);
            memset(slot, 0, sizeof *slot);
            return;
        }
        slot->generation = buf->generation;
    }
    slot->last_used_frame = renderer->frame_seq;
    glBindTexture(GL_TEXTURE_2D, 0);
    purp_texture_t texture = { .id = slot->texture, .target = GL_TEXTURE_2D,
                              .width = slot->width, .height = slot->height, .has_alpha = true };
    purp_render_texture(renderer, &texture, (purp_rectf_t){0, 0, buf->width, buf->height},
                         dst, alpha, NULL, WL_OUTPUT_TRANSFORM_NORMAL);
}

void purp_render_gl_texture(purp_renderer_t *renderer, GLuint id, int32_t tex_width,
                            int32_t tex_height, purp_rect_t src, purp_rect_t dst, float alpha) {
    purp_texture_t texture = { .id = id, .target = GL_TEXTURE_2D, .width = tex_width,
                              .height = tex_height, .has_alpha = true };
    purp_render_texture(renderer, &texture, (purp_rectf_t){src.x, src.y, src.w, src.h},
                         dst, alpha, NULL, WL_OUTPUT_TRANSFORM_NORMAL);
}

void purp_render_end(purp_renderer_t *renderer) {
    if (!renderer) return;
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(0);
    for (size_t i = 0; i < PURP_RENDER_PIXEL_CACHE_SIZE; ++i) {
        purp_pixel_texture_t *slot = &renderer->pixel_cache[i];
        if (slot->buf_id && renderer->frame_seq - slot->last_used_frame > PURP_RENDER_PIXEL_CACHE_MAX_AGE) {
            glDeleteTextures(1, &slot->texture);
            memset(slot, 0, sizeof *slot);
        }
    }
}

struct tree_render_data {
    purp_renderer_t *renderer;
    purp_pointf_t origin;
    purp_point_t monitor_origin;
    double scale;
    float alpha;
    const purp_rect_t *clip;
};

static bool render_tree_surface(purp_surface_t *surface, int32_t x, int32_t y, void *data) {
    struct tree_render_data *tree = data;
    purp_texture_t *texture = surface->texture;
    if (!texture || !surface->mapped) return true;
    double gx = tree->origin.x + x - tree->monitor_origin.x;
    double gy = tree->origin.y + y - tree->monitor_origin.y;
    int32_t left = purp_round_i32(gx * tree->scale), top = purp_round_i32(gy * tree->scale);
    purp_rect_t dst = purp_rect_make(left, top,
        render_clamp_i32((int64_t)purp_round_i32((gx + surface->size.w) * tree->scale) - left),
        render_clamp_i32((int64_t)purp_round_i32((gy + surface->size.h) * tree->scale) - top));
    int32_t transform = surface->current.buffer_transform;
    bool swapped = (transform & 1) != 0;
    double transformed_width = swapped ? texture->height : texture->width;
    double transformed_height = swapped ? texture->width : texture->height;
    purp_rectf_t src = {0, 0, texture->width, texture->height};
    if (surface->current.viewport_has_src) {
        double buffer_scale = surface->current.buffer_scale > 0 ? surface->current.buffer_scale : 1;
        const purp_rectf_t crop = surface->current.viewport_src;
        /* texture() applies the inverse transform after normalisation, so source axes here
         * describe the transformed buffer, re-expressed in its untransformed dimensions. */
        src = (purp_rectf_t){crop.x * buffer_scale / transformed_width * texture->width,
                            crop.y * buffer_scale / transformed_height * texture->height,
                            crop.w * buffer_scale / transformed_width * texture->width,
                            crop.h * buffer_scale / transformed_height * texture->height};
    }
    purp_buffer_t *buffer = surface->current.buffer;
    bool inverted = buffer && buffer->kind == PURP_BUFFER_DMABUF &&
        (buffer->dmabuf.flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT);
    if (inverted) {
        /* Reflect the buffer Y coordinate after the surface transform. */
        static const int32_t flip_y[] = { WL_OUTPUT_TRANSFORM_FLIPPED_180,
            WL_OUTPUT_TRANSFORM_FLIPPED_90, WL_OUTPUT_TRANSFORM_FLIPPED,
            WL_OUTPUT_TRANSFORM_FLIPPED_270, WL_OUTPUT_TRANSFORM_180,
            WL_OUTPUT_TRANSFORM_90, WL_OUTPUT_TRANSFORM_NORMAL, WL_OUTPUT_TRANSFORM_270 };
        if (transform >= 0 && transform < 8) transform = flip_y[transform];
    }
    purp_render_texture(tree->renderer, texture, src, dst, tree->alpha, tree->clip, transform);
    return true;
}

void purp_render_surface_tree(purp_renderer_t *renderer, purp_surface_t *root,
                              purp_pointf_t origin, purp_point_t monitor_origin, double scale,
                              float alpha, const purp_rect_t *clip) {
    if (!renderer || !root || scale <= 0) return;
    struct tree_render_data tree = {renderer, origin, monitor_origin, scale, alpha, clip};
    purp_surface_for_each(root, render_tree_surface, &tree);
}

static purp_rect_t monitor_physical_rect(purp_rect_t global, purp_point_t origin, double scale) {
    return purp_rect_to_physical(purp_rect_translate(global, -origin.x, -origin.y), scale);
}

void purp_render_border(purp_renderer_t *renderer, purp_rect_t geo, int32_t width,
                        purp_color_t color, purp_point_t monitor_origin, double scale) {
    if (!renderer || purp_rect_is_empty(geo) || width <= 0) return;
    int32_t wx = purp_min_i32(width, geo.w), wy = purp_min_i32(width, geo.h);
    purp_render_rect(renderer, monitor_physical_rect(purp_rect_make(geo.x, geo.y, geo.w, wy),
                                                     monitor_origin, scale), color);
    purp_render_rect(renderer, monitor_physical_rect(purp_rect_make(geo.x, geo.y + geo.h - wy, geo.w, wy),
                                                     monitor_origin, scale), color);
    int32_t side_h = purp_max_i32(geo.h - 2 * wy, 0);
    purp_render_rect(renderer, monitor_physical_rect(purp_rect_make(geo.x, geo.y + wy, wx, side_h),
                                                     monitor_origin, scale), color);
    purp_render_rect(renderer, monitor_physical_rect(purp_rect_make(geo.x + geo.w - wx, geo.y + wy, wx, side_h),
                                                     monitor_origin, scale), color);
}

static void render_layers(purp_server_t *server, size_t mon, uint32_t level,
                          purp_point_t origin, double scale, float alpha) {
    purp_layer_surface_t *layer;
    wl_list_for_each(layer, &server->layers, link) {
        if (layer->monitor != (int)mon || layer->current.layer != level ||
            !purp_layer_is_mapped(layer) || purp_layer_is_keepalive(layer)) continue;
        purp_render_surface_tree(server->renderer, layer->surface,
                                  (purp_pointf_t){layer->geo.x, layer->geo.y}, origin,
                                  scale, alpha, NULL);
    }
}

static void render_raster_at(purp_renderer_t *renderer, const purp_pixel_buf_t *raster,
                             int32_t x, int32_t y) {
    if (raster)
        purp_render_pixel_buf(renderer, raster, purp_rect_make(x, y, raster->width, raster->height), 1);
}

bool purp_render_monitor_frame(purp_server_t *server, size_t mon, int32_t fb_width,
                               int32_t fb_height, bool y_flip, bool hide_cursor) {
    if (!server || !server->renderer || mon >= server->wm.monitor_count ||
        !server->wm.monitors[mon].enabled || fb_width <= 0 || fb_height <= 0) return false;
    purp_renderer_t *renderer = server->renderer;
    purp_monitor_t *monitor = &server->wm.monitors[mon];
    purp_point_t origin = monitor->loc;
    double scale = monitor->scale;
    purp_render_begin(renderer, fb_width, fb_height, y_flip);
    if (server->lock_screen) {
        purp_render_clear(renderer, (purp_color_t){0.067f, 0.031f, 0.122f, 1});
        render_raster_at(renderer, purp_lock_render(server->lock_screen, server->font,
                                                    fb_width, fb_height, scale), 0, 0);
        purp_render_end(renderer);
        return glGetError() == GL_NO_ERROR;
    }
    purp_render_clear(renderer, PURP_COLOR_BG_ROOT);
    render_raster_at(renderer, purp_wallpaper_get(&server->wallpaper, fb_width, fb_height), 0, 0);
    render_layers(server, mon, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, origin, scale, 1);
    render_layers(server, mon, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, origin, scale, 1);
    struct renderer_storage *storage = (struct renderer_storage *)renderer;
    size_t order_count = server->wm.window_count;
    size_t popup_capacity = (size_t)wl_list_length(&server->xdg_surfaces);
    if (order_count > SIZE_MAX / sizeof(size_t) ||
        popup_capacity > SIZE_MAX / sizeof(purp_popup_t *)) {
        purp_render_end(renderer);
        return false;
    }
    size_t order_bytes = order_count * sizeof(size_t);
    size_t popup_bytes = popup_capacity * sizeof(purp_popup_t *);
    if (!reserve_bytes(&storage->composition_scratch, &storage->composition_capacity,
                       order_bytes > popup_bytes ? order_bytes : popup_bytes)) {
        purp_render_end(renderer);
        return false;
    }
    size_t *order = (size_t *)storage->composition_scratch;
    size_t count = purp_wm_stacking_order(&server->wm, order, order_count);
    purp_rect_t visible = purp_rect_inflate(purp_monitor_rect(monitor), PURP_BORDER_WIDTH + 4);
    for (size_t n = count; n > 0; --n) {
        size_t index = order[n - 1];
        purp_window_t *window = server->wm.windows[index];
        if (!purp_rect_overlaps(window->geo, visible)) continue;
        purp_surface_t *surface = purp_xdg_window_surface(window);
        if (!surface) continue;
        bool focused = (int)index == server->wm.focused_idx;
        if (window->is_fullscreen)
            purp_render_rect(renderer, monitor_physical_rect(window->geo, origin, scale),
                              PURP_COLOR_FULLSCREEN_BACKDROP);
        else {
            if (focused)
                purp_render_border(renderer, purp_rect_inflate(window->geo, 1), 1,
                                     PURP_COLOR_BORDER_GLOW, origin, scale);
            purp_render_border(renderer, window->geo, PURP_BORDER_WIDTH,
                                 focused ? PURP_COLOR_BORDER_ACTIVE : PURP_COLOR_BORDER_INACTIVE,
                                 origin, scale);
        }
        float alpha = window->is_fullscreen ? 1 : (window->has_opacity_override ?
            window->opacity_override : (focused ? PURP_OPACITY_ACTIVE : PURP_OPACITY_INACTIVE));
        purp_point_t content = purp_window_content_loc(window);
        purp_size_t content_size = purp_window_surface_size(window);
        purp_rect_t clip = monitor_physical_rect(purp_rect_make(content.x, content.y,
            content_size.w, content_size.h), origin, scale);
        purp_point_t surface_loc = purp_window_surface_loc(window);
        purp_render_surface_tree(renderer, surface, (purp_pointf_t){surface_loc.x, surface_loc.y},
                                  origin, scale, alpha, &clip);
    }
    bool bar_visible = purp_wm_bar_visible_on(&server->wm, mon);
    if (bar_visible) {
        purp_bar_inputs_t inputs;
        char title[512];
        purp_wm_bar_inputs(&server->wm, mon, &inputs, title, sizeof title);
        render_raster_at(renderer, purp_bar_render(&server->bar, mon, monitor->logical_size.w,
                                                   scale, &inputs), 0, 0);
    }
    render_layers(server, mon, ZWLR_LAYER_SHELL_V1_LAYER_TOP, origin, scale, PURP_OPACITY_POPUP);
    render_layers(server, mon, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, origin, scale, PURP_OPACITY_POPUP);
    purp_popup_t **popups = (purp_popup_t **)storage->composition_scratch;
    size_t popup_count = purp_xdg_visible_popups(server, (int)mon, popups, popup_capacity);
    for (size_t i = 0; i < popup_count; ++i) {
        purp_popup_t *popup = popups[i];
        purp_point_t loc = purp_xdg_popup_global_loc(popup);
        if (popup->xdg->has_geometry) {
            loc.x = render_clamp_i32((int64_t)loc.x - popup->xdg->geometry.x);
            loc.y = render_clamp_i32((int64_t)loc.y - popup->xdg->geometry.y);
        }
        purp_render_surface_tree(renderer, popup->xdg->surface, (purp_pointf_t){loc.x, loc.y},
                                  origin, scale, PURP_OPACITY_POPUP, NULL);
    }
    if (mon == purp_wm_active_monitor_index(&server->wm)) {
        purp_point_t at = {0, 0};
        const purp_pixel_buf_t *raster = purp_notify_render(&server->notify, server->font,
            monitor->logical_size.w, bar_visible, scale, &at);
        render_raster_at(renderer, raster, at.x, at.y);
        raster = purp_help_render(&server->help, server->font, fb_width, fb_height, scale, &at);
        render_raster_at(renderer, raster, at.x, at.y);
    }
    purp_pointf_t pointer = server->wm.pointer_location;
    if (server->seat.drag.active && server->seat.drag.icon)
        purp_render_surface_tree(renderer, server->seat.drag.icon, pointer, origin, scale,
                                  PURP_OPACITY_DND_ICON, NULL);
    if (!hide_cursor && server->seat.cursor_mode != PURP_CURSOR_MODE_HIDDEN &&
        purp_rect_contains_f(purp_rect_inflate(purp_monitor_rect(monitor),
                                              PURP_RENDER_CURSOR_MARGIN), pointer)) {
        if (server->seat.cursor_mode == PURP_CURSOR_MODE_SURFACE && server->seat.cursor_surface) {
            pointer.x -= server->seat.cursor_hotspot.x;
            pointer.y -= server->seat.cursor_hotspot.y;
            purp_render_surface_tree(renderer, server->seat.cursor_surface, pointer, origin, scale, 1, NULL);
        } else {
            const purp_cursor_image_t *cursor = purp_cursor_theme_get(&server->cursor_theme,
                server->seat.cursor_shape ? server->seat.cursor_shape : PURP_CURSOR_DEFAULT, scale);
            if (cursor)
                render_raster_at(renderer, &cursor->buffer,
                    purp_round_i32((pointer.x - origin.x) * scale) - cursor->hotspot_x,
                    purp_round_i32((pointer.y - origin.y) * scale) - cursor->hotspot_y);
        }
    }
    purp_render_end(renderer);
    return glGetError() == GL_NO_ERROR;
}

bool purp_render_offscreen_ensure(purp_renderer_t *renderer, purp_offscreen_t *target,
                                  int32_t width, int32_t height) {
    if (!renderer || !target || width <= 0 || height <= 0) return false;
    if (target->fbo && target->texture && target->width == width && target->height == height)
        return true;
    GLint old_fbo = 0, old_texture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    purp_offscreen_t next = { .width = width, .height = height };
    glGenTextures(1, &next.texture);
    glBindTexture(GL_TEXTURE_2D, next.texture);
    configure_texture(GL_TEXTURE_2D);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &next.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, next.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, next.texture, 0);
    bool okay = next.fbo && next.texture &&
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
    if (!okay) { purp_render_offscreen_release(renderer, &next); return false; }
    bool was_bound = target->fbo && (GLuint)old_fbo == target->fbo;
    bool texture_was_bound = target->texture && (GLuint)old_texture == target->texture;
    purp_render_offscreen_release(renderer, target);
    *target = next;
    if (was_bound) glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    if (texture_was_bound) glBindTexture(GL_TEXTURE_2D, target->texture);
    return true;
}

void purp_render_offscreen_release(purp_renderer_t *renderer, purp_offscreen_t *target) {
    if (!target) return;
    if (target->fbo) glDeleteFramebuffers(1, &target->fbo);
    if (target->texture) glDeleteTextures(1, &target->texture);
    memset(target, 0, sizeof *target);
}

bool purp_render_capture_rgba(purp_server_t *server, size_t mon, const purp_rect_t *region,
                              bool hide_cursor, uint8_t **rgba, uint32_t *width,
                              uint32_t *height) {
    if (!rgba || !width || !height) return false;
    *rgba = NULL; *width = 0; *height = 0;
    if (!server || !server->renderer || server->lock_screen || mon >= server->wm.monitor_count ||
        !server->wm.monitors[mon].enabled) return false;
    if (server->backend_ops && server->backend_ops->make_current &&
        !server->backend_ops->make_current(server->backend)) return false;
    purp_monitor_t *monitor = &server->wm.monitors[mon];
    int32_t full_width = monitor->physical_size.w, full_height = monitor->physical_size.h;
    purp_rect_t logical = purp_monitor_rect(monitor);
    if (region && !purp_rect_intersect(logical, *region, &logical)) return false;
    purp_rect_t physical = monitor_physical_rect(logical, monitor->loc, monitor->scale);
    if (!purp_rect_intersect(physical, purp_rect_make(0, 0, full_width, full_height), &physical) ||
        (size_t)physical.w > SIZE_MAX / 4 / (size_t)physical.h) return false;
    uint8_t *pixels = malloc((size_t)physical.w * (size_t)physical.h * 4);
    if (!pixels) return false;
    GLint old_fbo = 0, old_viewport[4], old_pack = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    purp_offscreen_t target = {0};
    bool okay = purp_render_offscreen_ensure(server->renderer, &target, full_width, full_height);
    if (okay) {
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        okay = purp_render_monitor_frame(server, mon, full_width, full_height, false, hide_cursor);
        if (okay) {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(physical.x, physical.y, physical.w, physical.h,
                          GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            okay = glGetError() == GL_NO_ERROR;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    purp_render_offscreen_release(server->renderer, &target);
    if (!okay) { free(pixels); return false; }
    size_t count = (size_t)physical.w * (size_t)physical.h;
    for (size_t i = 0; i < count; ++i) pixels[i * 4 + 3] = 255;
    *rgba = pixels; *width = (uint32_t)physical.w; *height = (uint32_t)physical.h;
    return true;
}

struct png_output {
    purp_blob_t *blob;
    size_t length;
    size_t capacity;
};

static void png_write_bytes(png_structp png, png_bytep bytes, png_size_t length) {
    struct png_output *output = png_get_io_ptr(png);
    if (length > SIZE_MAX - output->length) png_error(png, "PNG output overflow");
    size_t needed = output->length + length;
    if (needed > output->capacity) {
        size_t capacity = output->capacity ? output->capacity : 4096;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        if (capacity > SIZE_MAX - sizeof(purp_blob_t))
            png_error(png, "PNG output overflow");
        purp_blob_t *grown = realloc(output->blob, sizeof *grown + capacity);
        if (!grown) png_error(png, "PNG output allocation failed");
        output->blob = grown;
        output->capacity = capacity;
    }
    memcpy(output->blob->data + output->length, bytes, length);
    output->length += length;
}

static void png_flush_bytes(png_structp png) {
    (void)png;
}

bool purp_png_encode_rgba(const uint8_t *rgba, uint32_t width, uint32_t height,
                          purp_blob_t **out) {
    if (!out) return false;
    *out = NULL;
    if (!rgba || !width || !height || width > PNG_UINT_31_MAX || height > PNG_UINT_31_MAX ||
        (size_t)width > SIZE_MAX / 4 / height) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    struct png_output *output = calloc(1, sizeof *output);
    if (!info || !output) {
        png_destroy_write_struct(&png, info ? &info : NULL);
        free(output);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        free(output->blob);
        free(output);
        return false;
    }
    png_set_write_fn(png, output, png_write_bytes, png_flush_bytes);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                  PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (uint32_t row = 0; row < height; ++row)
        png_write_row(png, rgba + (size_t)row * width * 4);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    atomic_init(&output->blob->refs, 1);
    output->blob->len = output->length;
    *out = output->blob;
    free(output);
    return *out != NULL;
}

bool purp_host_renderer_init(purp_server_t *server, EGLDisplay display, EGLContext context,
                             dev_t render_node) {
    if (!server || server->renderer) return false;
    server->renderer = purp_renderer_create(server, display, context);
    if (!server->renderer) return false;
    purp_dmabuf_format_t *formats = NULL;
    size_t count = purp_renderer_dmabuf_formats(server->renderer, &formats);
    bool okay = purp_dmabuf_init(server, formats, count, render_node);
    free(formats);
    if (!okay) {
        purp_renderer_destroy(server->renderer);
        server->renderer = NULL;
    }
    return okay;
}

void purp_host_renderer_finish(purp_server_t *server) {
    if (!server) return;
    purp_dmabuf_finish(server);
    purp_renderer_destroy(server->renderer);
    server->renderer = NULL;
}

bool purp_host_render_monitor(purp_server_t *server, size_t idx, int32_t fb_width,
                              int32_t fb_height, bool y_flip) {
    if (!server || idx >= server->wm.monitor_count) return false;
    /* Consume the submitted damage, not damage that arrives while DRM's flip is pending.
     * Offscreen screenshot composition bypasses this presentation entry point. */
    server->monitor_damaged[idx] = false;
    bool okay = purp_render_monitor_frame(server, idx, fb_width, fb_height, y_flip, false);
    if (!okay) server->monitor_damaged[idx] = true;
    return okay;
}

bool purp_host_offscreen_ensure(purp_server_t *server, purp_offscreen_t *target, int32_t width,
                                int32_t height) {
    return server && purp_render_offscreen_ensure(server->renderer, target, width, height);
}

void purp_host_offscreen_bind(const purp_offscreen_t *target) {
    glBindFramebuffer(GL_FRAMEBUFFER, target ? target->fbo : 0);
}

void purp_host_offscreen_release(purp_server_t *server, purp_offscreen_t *target) {
    purp_render_offscreen_release(server ? server->renderer : NULL, target);
}

void purp_host_render_begin(purp_server_t *server, int32_t fb_width, int32_t fb_height,
                            bool y_flip, purp_color_t clear) {
    if (!server) return;
    purp_render_begin(server->renderer, fb_width, fb_height, y_flip);
    purp_render_clear(server->renderer, clear);
}

void purp_host_render_texture(purp_server_t *server, uint32_t texture, int32_t tex_width,
                              int32_t tex_height, purp_rect_t src, purp_rect_t dst, float alpha) {
    if (server) purp_render_gl_texture(server->renderer, texture, tex_width, tex_height, src, dst, alpha);
}

void purp_host_render_end(purp_server_t *server) {
    if (server) purp_render_end(server->renderer);
}
