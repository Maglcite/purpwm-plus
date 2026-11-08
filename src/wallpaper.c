/*
 * purpwm+ — static wallpaper (see ui.h).
 *
 * $PURPWM_WALLPAPER points at any image gdk-pixbuf can read. For each requested physical output
 * size the source is cover-cropped (centered, aspect-preserving), bilinear-scaled and composited
 * over the opaque #11081F background into a premultiplied ARGB8888 purp_pixel_buf_t, cached for
 * up to PURP_WALLPAPER_CACHE_ENTRIES resolutions.
 */
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "ui.h"

purp_rect_t purp_wallpaper_cover_crop(int32_t src_w, int32_t src_h, int32_t dst_w, int32_t dst_h) {
    src_w = purp_max_i32(src_w, 1);
    src_h = purp_max_i32(src_h, 1);
    dst_w = purp_max_i32(dst_w, 1);
    dst_h = purp_max_i32(dst_h, 1);
    int64_t crop_w, crop_h;
    if ((int64_t)src_w * dst_h > (int64_t)src_h * dst_w) {
        crop_w = (int64_t)src_h * dst_w / dst_h;
        crop_w = crop_w < 1 ? 1 : (crop_w > src_w ? src_w : crop_w);
        crop_h = src_h;
    } else {
        crop_h = (int64_t)src_w * dst_h / dst_w;
        crop_h = crop_h < 1 ? 1 : (crop_h > src_h ? src_h : crop_h);
        crop_w = src_w;
    }
    return purp_rect_make((src_w - (int32_t)crop_w) / 2, (src_h - (int32_t)crop_h) / 2,
                          (int32_t)crop_w, (int32_t)crop_h);
}

static void purp_wallpaper_clear_cache(purp_wallpaper_t *wp) {
    for (size_t i = 0; i < PURP_WALLPAPER_CACHE_ENTRIES; ++i) {
        purp_pixel_buf_free(&wp->cache[i].buffer);
        wp->cache[i] = (purp_wallpaper_entry_t){ 0 };
    }
}

bool purp_wallpaper_load(purp_wallpaper_t *wp, const char *path) {
    GError *error = NULL;
    GdkPixbuf *loaded = path ? gdk_pixbuf_new_from_file(path, &error) : NULL;
    if (!loaded) {
        PURP_WARN("Unable to load wallpaper %s: %s", path ? path : "(null)",
                  error ? error->message : "no path");
        if (error) g_error_free(error);
        return false;
    }
    /* Normalise to 8-bit RGBA straight alpha so the compositor can use one code path. */
    GdkPixbuf *rgba = loaded;
    if (!gdk_pixbuf_get_has_alpha(loaded) || gdk_pixbuf_get_n_channels(loaded) != 4) {
        rgba = gdk_pixbuf_add_alpha(loaded, FALSE, 0, 0, 0);
        g_object_unref(loaded);
        if (!rgba) {
            PURP_WARN("Unable to load wallpaper %s: cannot add alpha channel", path);
            return false;
        }
    }
    if (wp->source) g_object_unref((GdkPixbuf *)wp->source);
    wp->source = rgba;
    wp->source_width = gdk_pixbuf_get_width(rgba);
    wp->source_height = gdk_pixbuf_get_height(rgba);
    wp->loaded = true;
    purp_wallpaper_clear_cache(wp);
    return true;
}

void purp_wallpaper_init_from_env(purp_wallpaper_t *wp) {
    *wp = (purp_wallpaper_t){ 0 };
    const char *path = getenv(PURP_ENV_WALLPAPER);
    if (path && path[0]) (void)purp_wallpaper_load(wp, path);
}

void purp_wallpaper_finish(purp_wallpaper_t *wp) {
    if (!wp) return;
    if (wp->source) g_object_unref((GdkPixbuf *)wp->source);
    purp_wallpaper_clear_cache(wp);
    *wp = (purp_wallpaper_t){ 0 };
}

/* Composite one straight-alpha RGBA pixel over opaque PURP_WALLPAPER_BG_RGBA and write it as
 * premultiplied bytes B, G, R, A=255 (channel division matching the Rust wallpaper). */
static void purp_wallpaper_put_pixel(uint8_t *dst, const uint8_t *src) {
    uint32_t a = src[3];
    uint32_t inv = 255u - a;
    dst[0] = (uint8_t)((src[2] * a + PURP_WALLPAPER_BG_RGBA.b * inv) / 255u);
    dst[1] = (uint8_t)((src[1] * a + PURP_WALLPAPER_BG_RGBA.g * inv) / 255u);
    dst[2] = (uint8_t)((src[0] * a + PURP_WALLPAPER_BG_RGBA.r * inv) / 255u);
    dst[3] = 255;
}

static bool purp_wallpaper_render(purp_wallpaper_t *wp, int32_t width, int32_t height,
                                  purp_pixel_buf_t *out) {
    const GdkPixbuf *source = (const GdkPixbuf *)wp->source;
    purp_rect_t crop = purp_wallpaper_cover_crop(wp->source_width, wp->source_height, width,
                                                 height);
    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf((GdkPixbuf *)source, crop.x, crop.y, crop.w, crop.h);
    if (!sub) return false;
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(sub, width, height, GDK_INTERP_BILINEAR);
    g_object_unref(sub);
    if (!scaled) return false;
    if (!purp_pixel_buf_ensure(out, width, height)) {
        g_object_unref(scaled);
        return false;
    }
    const uint8_t *src = gdk_pixbuf_get_pixels(scaled);
    int src_stride = gdk_pixbuf_get_rowstride(scaled);
    bool has_alpha = gdk_pixbuf_get_has_alpha(scaled);
    int channels = gdk_pixbuf_get_n_channels(scaled);
    uint8_t opaque[4];
    for (int32_t y = 0; y < height; ++y) {
        const uint8_t *src_row = src + (size_t)y * (size_t)src_stride;
        uint8_t *dst_row = out->data + (size_t)y * (size_t)out->stride;
        for (int32_t x = 0; x < width; ++x) {
            const uint8_t *px = src_row + (size_t)x * (size_t)channels;
            if (has_alpha) {
                purp_wallpaper_put_pixel(dst_row + (size_t)x * 4u, px);
            } else {
                opaque[0] = px[0];
                opaque[1] = px[1];
                opaque[2] = px[2];
                opaque[3] = 255;
                purp_wallpaper_put_pixel(dst_row + (size_t)x * 4u, opaque);
            }
        }
    }
    purp_pixel_buf_touch(out);
    g_object_unref(scaled);
    return true;
}

const purp_pixel_buf_t *purp_wallpaper_get(purp_wallpaper_t *wp, int32_t width, int32_t height) {
    if (!wp->loaded || !wp->source) return NULL;
    width = purp_max_i32(width, 1);
    height = purp_max_i32(height, 1);
    size_t free_slot = PURP_WALLPAPER_CACHE_ENTRIES;
    for (size_t i = 0; i < PURP_WALLPAPER_CACHE_ENTRIES; ++i) {
        purp_wallpaper_entry_t *entry = &wp->cache[i];
        if (entry->in_use && entry->width == width && entry->height == height)
            return &entry->buffer;
        if (!entry->in_use && free_slot == PURP_WALLPAPER_CACHE_ENTRIES) free_slot = i;
    }
    if (free_slot == PURP_WALLPAPER_CACHE_ENTRIES) {
        /* Cache full: drop every raster (Rust clears the whole map) and reuse slot 0. */
        purp_wallpaper_clear_cache(wp);
        free_slot = 0;
    }
    purp_wallpaper_entry_t *entry = &wp->cache[free_slot];
    if (!purp_wallpaper_render(wp, width, height, &entry->buffer)) return NULL;
    entry->in_use = true;
    entry->width = width;
    entry->height = height;
    return &entry->buffer;
}
