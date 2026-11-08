/*
 * purpwm+ — XCursor theme loader + built-in purple arrow (see ui.h).
 *
 * Shapes come from $XCURSOR_THEME (or "default", e.g. Adwaita) through libXcursor's library path
 * ($XCURSOR_PATH or the compiled-in discover path), at $XCURSOR_SIZE (default 24) times the
 * output scale. Each lookup tries the CSS name then the legacy aliases, picks the theme image
 * closest to the target size (preferring the next larger one, first frame of animated cursors)
 * and nearest-neighbour scales it — bitmap and hotspot alike — into a premultiplied ARGB8888
 * purp_pixel_buf_t. Shapes missing from the theme fall back to the default cursor and finally to
 * the built-in 20x26 purple arrow, so purp_cursor_theme_get() never returns NULL.
 */
#include <X11/Xcursor/Xcursor.h>

#include "ui.h"

/* CSS cursor names for wp_cursor_shape_device_v1.shape values 1..=36 (cursor-icon crate
 * compatible, matching the Rust purpwm Smithay names). */
static const char *const PURP_CURSOR_SHAPE_NAMES[PURP_CURSOR_SHAPE_MAX + 1] = {
    [PURP_CURSOR_DEFAULT] = "default",
    [PURP_CURSOR_CONTEXT_MENU] = "context-menu",
    [PURP_CURSOR_HELP] = "help",
    [PURP_CURSOR_POINTER] = "pointer",
    [PURP_CURSOR_PROGRESS] = "progress",
    [PURP_CURSOR_WAIT] = "wait",
    [PURP_CURSOR_CELL] = "cell",
    [PURP_CURSOR_CROSSHAIR] = "crosshair",
    [PURP_CURSOR_TEXT] = "text",
    [PURP_CURSOR_VERTICAL_TEXT] = "vertical-text",
    [PURP_CURSOR_ALIAS] = "alias",
    [PURP_CURSOR_COPY] = "copy",
    [PURP_CURSOR_MOVE] = "move",
    [PURP_CURSOR_NO_DROP] = "no-drop",
    [PURP_CURSOR_NOT_ALLOWED] = "not-allowed",
    [PURP_CURSOR_GRAB] = "grab",
    [PURP_CURSOR_GRABBING] = "grabbing",
    [PURP_CURSOR_E_RESIZE] = "e-resize",
    [PURP_CURSOR_N_RESIZE] = "n-resize",
    [PURP_CURSOR_NE_RESIZE] = "ne-resize",
    [PURP_CURSOR_NW_RESIZE] = "nw-resize",
    [PURP_CURSOR_S_RESIZE] = "s-resize",
    [PURP_CURSOR_SE_RESIZE] = "se-resize",
    [PURP_CURSOR_SW_RESIZE] = "sw-resize",
    [PURP_CURSOR_W_RESIZE] = "w-resize",
    [PURP_CURSOR_EW_RESIZE] = "ew-resize",
    [PURP_CURSOR_NS_RESIZE] = "ns-resize",
    [PURP_CURSOR_NESW_RESIZE] = "nesw-resize",
    [PURP_CURSOR_NWSE_RESIZE] = "nwse-resize",
    [PURP_CURSOR_COL_RESIZE] = "col-resize",
    [PURP_CURSOR_ROW_RESIZE] = "row-resize",
    [PURP_CURSOR_ALL_SCROLL] = "all-scroll",
    [PURP_CURSOR_ZOOM_IN] = "zoom-in",
    [PURP_CURSOR_ZOOM_OUT] = "zoom-out",
    [PURP_CURSOR_DND_ASK] = "dnd-ask",
    [PURP_CURSOR_ALL_RESIZE] = "all-resize",
};

/* Legacy X cursor aliases, in lookup order (cursor-icon crate alt_names). */
static const char *const PURP_CURSOR_ALT_EMPTY[] = { NULL };
static const char *const PURP_CURSOR_ALT_DEFAULT[] = { "left_ptr", "arrow", "top_left_arrow",
    "left_arrow", NULL };
static const char *const PURP_CURSOR_ALT_HELP[] = { "question_arrow", "whats_this", NULL };
static const char *const PURP_CURSOR_ALT_POINTER[] = { "hand2", "hand1", "hand", "pointing_hand",
    NULL };
static const char *const PURP_CURSOR_ALT_PROGRESS[] = { "left_ptr_watch", "half-busy", NULL };
static const char *const PURP_CURSOR_ALT_WAIT[] = { "watch", NULL };
static const char *const PURP_CURSOR_ALT_CELL[] = { "plus", NULL };
static const char *const PURP_CURSOR_ALT_CROSSHAIR[] = { "cross", NULL };
static const char *const PURP_CURSOR_ALT_TEXT[] = { "xterm", "ibeam", NULL };
static const char *const PURP_CURSOR_ALT_ALIAS[] = { "link", NULL };
static const char *const PURP_CURSOR_ALT_NO_DROP[] = { "circle", NULL };
static const char *const PURP_CURSOR_ALT_NOT_ALLOWED[] = { "crossed_circle", "forbidden", NULL };
static const char *const PURP_CURSOR_ALT_GRAB[] = { "openhand", "fleur", NULL };
static const char *const PURP_CURSOR_ALT_GRABBING[] = { "closedhand", NULL };
static const char *const PURP_CURSOR_ALT_E_RESIZE[] = { "right_side", NULL };
static const char *const PURP_CURSOR_ALT_N_RESIZE[] = { "top_side", NULL };
static const char *const PURP_CURSOR_ALT_NE_RESIZE[] = { "top_right_corner", NULL };
static const char *const PURP_CURSOR_ALT_NW_RESIZE[] = { "top_left_corner", NULL };
static const char *const PURP_CURSOR_ALT_S_RESIZE[] = { "bottom_side", NULL };
static const char *const PURP_CURSOR_ALT_SE_RESIZE[] = { "bottom_right_corner", NULL };
static const char *const PURP_CURSOR_ALT_SW_RESIZE[] = { "bottom_left_corner", NULL };
static const char *const PURP_CURSOR_ALT_W_RESIZE[] = { "left_side", NULL };
static const char *const PURP_CURSOR_ALT_EW_RESIZE[] = { "h_double_arrow", "size_hor", NULL };
static const char *const PURP_CURSOR_ALT_NS_RESIZE[] = { "v_double_arrow", "size_ver", NULL };
static const char *const PURP_CURSOR_ALT_NESW_RESIZE[] = { "fd_double_arrow", "size_bdiag", NULL };
static const char *const PURP_CURSOR_ALT_NWSE_RESIZE[] = { "bd_double_arrow", "size_fdiag", NULL };
static const char *const PURP_CURSOR_ALT_COL_RESIZE[] = { "split_h", "h_double_arrow",
    "sb_h_double_arrow", NULL };
static const char *const PURP_CURSOR_ALT_ROW_RESIZE[] = { "split_v", "v_double_arrow",
    "sb_v_double_arrow", NULL };
static const char *const PURP_CURSOR_ALT_ALL_SCROLL[] = { "size_all", NULL };
static const char *const PURP_CURSOR_ALT_DND_ASK[] = { "copy", NULL };
static const char *const PURP_CURSOR_ALT_ALL_RESIZE[] = { "move", NULL };

static const char *const *const PURP_CURSOR_ALT_NAMES[PURP_CURSOR_SHAPE_MAX + 1] = {
    [PURP_CURSOR_DEFAULT] = PURP_CURSOR_ALT_DEFAULT,
    [PURP_CURSOR_CONTEXT_MENU] = PURP_CURSOR_ALT_EMPTY,
    [PURP_CURSOR_HELP] = PURP_CURSOR_ALT_HELP,
    [PURP_CURSOR_POINTER] = PURP_CURSOR_ALT_POINTER,
    [PURP_CURSOR_PROGRESS] = PURP_CURSOR_ALT_PROGRESS,
    [PURP_CURSOR_WAIT] = PURP_CURSOR_ALT_WAIT,
    [PURP_CURSOR_CELL] = PURP_CURSOR_ALT_CELL,
    [PURP_CURSOR_CROSSHAIR] = PURP_CURSOR_ALT_CROSSHAIR,
    [PURP_CURSOR_TEXT] = PURP_CURSOR_ALT_TEXT,
    [PURP_CURSOR_VERTICAL_TEXT] = PURP_CURSOR_ALT_EMPTY,
    [PURP_CURSOR_ALIAS] = PURP_CURSOR_ALT_ALIAS,
    [PURP_CURSOR_COPY] = PURP_CURSOR_ALT_EMPTY,
    [PURP_CURSOR_MOVE] = PURP_CURSOR_ALT_EMPTY,
    [PURP_CURSOR_NO_DROP] = PURP_CURSOR_ALT_NO_DROP,
    [PURP_CURSOR_NOT_ALLOWED] = PURP_CURSOR_ALT_NOT_ALLOWED,
    [PURP_CURSOR_GRAB] = PURP_CURSOR_ALT_GRAB,
    [PURP_CURSOR_GRABBING] = PURP_CURSOR_ALT_GRABBING,
    [PURP_CURSOR_E_RESIZE] = PURP_CURSOR_ALT_E_RESIZE,
    [PURP_CURSOR_N_RESIZE] = PURP_CURSOR_ALT_N_RESIZE,
    [PURP_CURSOR_NE_RESIZE] = PURP_CURSOR_ALT_NE_RESIZE,
    [PURP_CURSOR_NW_RESIZE] = PURP_CURSOR_ALT_NW_RESIZE,
    [PURP_CURSOR_S_RESIZE] = PURP_CURSOR_ALT_S_RESIZE,
    [PURP_CURSOR_SE_RESIZE] = PURP_CURSOR_ALT_SE_RESIZE,
    [PURP_CURSOR_SW_RESIZE] = PURP_CURSOR_ALT_SW_RESIZE,
    [PURP_CURSOR_W_RESIZE] = PURP_CURSOR_ALT_W_RESIZE,
    [PURP_CURSOR_EW_RESIZE] = PURP_CURSOR_ALT_EW_RESIZE,
    [PURP_CURSOR_NS_RESIZE] = PURP_CURSOR_ALT_NS_RESIZE,
    [PURP_CURSOR_NESW_RESIZE] = PURP_CURSOR_ALT_NESW_RESIZE,
    [PURP_CURSOR_NWSE_RESIZE] = PURP_CURSOR_ALT_NWSE_RESIZE,
    [PURP_CURSOR_COL_RESIZE] = PURP_CURSOR_ALT_COL_RESIZE,
    [PURP_CURSOR_ROW_RESIZE] = PURP_CURSOR_ALT_ROW_RESIZE,
    [PURP_CURSOR_ALL_SCROLL] = PURP_CURSOR_ALT_ALL_SCROLL,
    [PURP_CURSOR_ZOOM_IN] = PURP_CURSOR_ALT_EMPTY,
    [PURP_CURSOR_ZOOM_OUT] = PURP_CURSOR_ALT_EMPTY,
    [PURP_CURSOR_DND_ASK] = PURP_CURSOR_ALT_DND_ASK,
    [PURP_CURSOR_ALL_RESIZE] = PURP_CURSOR_ALT_ALL_RESIZE,
};

/* The built-in arrow's first 23 rows (rows 23..25 of the 20x26 canvas stay transparent):
 * W outline, L highlight, P fill, S shadow, '.' transparent — same bitmap as Rust purpwm. */
static const char *const PURP_CURSOR_ARROW_ROWS[] = {
    "W...................",
    "WW..................",
    "WLW.................",
    "WLLW................",
    "WLPLW...............",
    "WLPPLW..............",
    "WLPPPLW.............",
    "WLPPPPLW............",
    "WLPPPPPLW...........",
    "WLPPPPPPLW..........",
    "WLPPPPPPPLW.........",
    "WLPPPPPPPPLW........",
    "WLPPPPPPPPPLW.......",
    "WLPPPPPPWWWWWW......",
    "WLPPWLPPLWSSSS......",
    "WLPW.WLPPLW.........",
    "WLW..SWLPPLW........",
    "WW....WLPPLW........",
    "W.....SWLPPLW.......",
    ".......WLPPLW.......",
    ".......SWLPLW.......",
    "........WWWW........",
    "........SSSS........",
};

static bool purp_cursor_shape_valid(purp_cursor_shape_t shape) {
    return shape >= PURP_CURSOR_DEFAULT && shape <= PURP_CURSOR_SHAPE_MAX;
}

const char *purp_cursor_shape_name(purp_cursor_shape_t shape) {
    if (!purp_cursor_shape_valid(shape)) return NULL;
    return PURP_CURSOR_SHAPE_NAMES[shape];
}

const char *const *purp_cursor_shape_alt_names(purp_cursor_shape_t shape) {
    if (!purp_cursor_shape_valid(shape)) return NULL;
    return PURP_CURSOR_ALT_NAMES[shape];
}

uint32_t purp_cursor_target_px(uint32_t base_size, double scale) {
    double target = round((double)base_size * scale);
    if (!(target >= (double)PURP_CURSOR_MIN_PX)) return PURP_CURSOR_MIN_PX; /* catches NaN */
    if (target >= (double)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)target;
}

void purp_cursor_theme_init(purp_cursor_theme_t *theme) {
    *theme = (purp_cursor_theme_t){ 0 };
    const char *env = getenv("XCURSOR_THEME");
    if (env && env[0]) {
        snprintf(theme->theme, sizeof theme->theme, "%s", env);
    } else {
        snprintf(theme->theme, sizeof theme->theme, "%s", PURP_CURSOR_DEFAULT_THEME);
    }
    env = getenv("XCURSOR_SIZE");
    if (env) {
        char *end = NULL;
        unsigned long size = strtoul(env, &end, 10);
        if (end != env && *end == '\0' && size > 0 && size <= UINT32_MAX)
            theme->base_size = (uint32_t)size;
    }
    if (theme->base_size == 0) theme->base_size = PURP_CURSOR_DEFAULT_SIZE;
}

void purp_cursor_theme_finish(purp_cursor_theme_t *theme) {
    if (!theme) return;
    for (size_t i = 0; i < PURP_CURSOR_CACHE_ENTRIES; ++i)
        purp_pixel_buf_free(&theme->cache[i].image.buffer);
    purp_pixel_buf_free(&theme->arrow.buffer);
    theme->cache_next = 0;
    theme->arrow_px = 0;
}

/* Write the premultiplied ARGB8888 word `px` (0xAARRGGBB) as bytes B, G, R, A at (x, y). */
static void purp_cursor_put_word(purp_pixel_buf_t *buf, int32_t x, int32_t y, uint32_t px) {
    if (!buf->data || x < 0 || y < 0 || x >= buf->width || y >= buf->height) return;
    uint8_t *dst = buf->data + (size_t)y * (size_t)buf->stride + (size_t)x * 4u;
    dst[0] = (uint8_t)(px & 0xFFu);
    dst[1] = (uint8_t)((px >> 8) & 0xFFu);
    dst[2] = (uint8_t)((px >> 16) & 0xFFu);
    dst[3] = (uint8_t)(px >> 24);
}

static purp_rgba_t purp_cursor_arrow_color(char c) {
    switch (c) {
    case 'W': return PURP_CURSOR_OUTLINE_RGBA;
    case 'L': return PURP_CURSOR_HIGHLIGHT_RGBA;
    case 'P': return PURP_CURSOR_FILL_RGBA;
    case 'S': return PURP_CURSOR_SHADOW_RGBA;
    default: return (purp_rgba_t){ 0, 0, 0, 0 };
    }
}

bool purp_cursor_builtin_arrow(purp_cursor_image_t *out, double scale) {
    if (!(scale > 0.0)) scale = 1.0; /* NaN, 0 and negatives render at scale 1 */
    int32_t width = purp_max_i32(1, purp_round_i32(PURP_CURSOR_ARROW_WIDTH * scale));
    int32_t height = purp_max_i32(1, purp_round_i32(PURP_CURSOR_ARROW_HEIGHT * scale));
    if (!purp_pixel_buf_ensure(&out->buffer, width, height)) return false;
    out->hotspot_x = 0;
    out->hotspot_y = 0;
    for (int32_t y = 0; y < height; ++y) {
        int32_t src_y = (int32_t)((double)y / scale);
        if (src_y > PURP_CURSOR_ARROW_HEIGHT - 1) src_y = PURP_CURSOR_ARROW_HEIGHT - 1;
        for (int32_t x = 0; x < width; ++x) {
            int32_t src_x = (int32_t)((double)x / scale);
            if (src_x > PURP_CURSOR_ARROW_WIDTH - 1) src_x = PURP_CURSOR_ARROW_WIDTH - 1;
            purp_rgba_t rgba = { 0, 0, 0, 0 };
            if (src_y < (int32_t)PURP_ARRAY_LEN(PURP_CURSOR_ARROW_ROWS))
                rgba = purp_cursor_arrow_color(PURP_CURSOR_ARROW_ROWS[src_y][src_x]);
            purp_cursor_put_word(&out->buffer, x, y, purp_rgba_premultiply(rgba));
        }
    }
    purp_pixel_buf_touch(&out->buffer);
    return true;
}

/* First frame of the theme image closest to `target`: the smallest nominal size >= target when
 * one exists, otherwise the largest below (Rust: min_by_key((size < target, |size - target|))). */
static const XcursorImage *purp_cursor_pick_image(const XcursorImages *images, uint32_t target) {
    const XcursorImage *best = NULL;
    uint32_t best_score = UINT32_MAX;
    bool best_below = true;
    for (int i = 0; i < images->nimage; ++i) {
        const XcursorImage *img = images->images[i];
        if (!img || img->width == 0 || img->height == 0) continue;
        bool below = img->size < target;
        uint32_t diff = img->size >= target ? img->size - target : target - img->size;
        if (!best || (int)below < (int)best_below ||
            ((int)below == (int)best_below && diff < best_score)) {
            best = img;
            best_below = below;
            best_score = diff;
        }
    }
    return best;
}

/* Nearest-neighbour scale `img` so its largest side is exactly `target` px, scaling the hotspot
 * by the same factor. Xcursor pixels are already premultiplied little-endian ARGB8888, the same
 * layout as purp_pixel_buf_t. */
static bool purp_cursor_convert_image(purp_cursor_image_t *out, const XcursorImage *img,
                                      uint32_t target) {
    double factor = (double)target /
                    (double)purp_max_i32(purp_max_i32((int32_t)img->width, (int32_t)img->height),
                                         1);
    int32_t width = purp_max_i32(1, purp_round_i32((double)img->width * factor));
    int32_t height = purp_max_i32(1, purp_round_i32((double)img->height * factor));
    if (!purp_pixel_buf_ensure(&out->buffer, width, height)) return false;
    out->hotspot_x = purp_round_i32((double)img->xhot * factor);
    out->hotspot_y = purp_round_i32((double)img->yhot * factor);
    for (int32_t y = 0; y < height; ++y) {
        XcursorDim src_y = (XcursorDim)((double)y / factor);
        if (src_y >= img->height) src_y = img->height - 1;
        const XcursorPixel *row = img->pixels + (size_t)src_y * img->width;
        for (int32_t x = 0; x < width; ++x) {
            XcursorDim src_x = (XcursorDim)((double)x / factor);
            if (src_x >= img->width) src_x = img->width - 1;
            purp_cursor_put_word(&out->buffer, x, y, row[src_x]);
        }
    }
    purp_pixel_buf_touch(&out->buffer);
    return true;
}

/* Try the CSS name and every alias for `shape` in the theme; first that loads wins. */
static bool purp_cursor_load_named(const purp_cursor_theme_t *theme, purp_cursor_shape_t shape,
                                   uint32_t target, purp_cursor_image_t *out) {
    const char *const *aliases = purp_cursor_shape_alt_names(shape);
    for (size_t i = 0;; ++i) {
        const char *try_name = i == 0 ? purp_cursor_shape_name(shape) : NULL;
        if (i > 0) {
            if (!aliases || !aliases[i - 1]) break;
            try_name = aliases[i - 1];
        }
        if (!try_name) continue;
        XcursorImages *images = XcursorLibraryLoadImages(try_name, theme->theme, (int)target);
        if (!images) continue;
        const XcursorImage *img = purp_cursor_pick_image(images, target);
        bool ok = img && purp_cursor_convert_image(out, img, target);
        XcursorImagesDestroy(images);
        if (ok) return true;
    }
    return false;
}

/* Built-in arrow raster for `target` px, cached in theme->arrow at theme->arrow_px. */
static const purp_cursor_image_t *purp_cursor_fallback_arrow(purp_cursor_theme_t *theme,
                                                             uint32_t target) {
    if (theme->arrow_px != target || !theme->arrow.buffer.data) {
        double scale = (double)target / (double)purp_max_i32(PURP_CURSOR_ARROW_WIDTH,
                                                             PURP_CURSOR_ARROW_HEIGHT);
        if (!purp_cursor_builtin_arrow(&theme->arrow, scale)) {
            theme->arrow_px = 0;
            return &theme->arrow;
        }
        theme->arrow_px = target;
    }
    return &theme->arrow;
}

const purp_cursor_image_t *purp_cursor_theme_get(purp_cursor_theme_t *theme,
                                                 purp_cursor_shape_t shape, double scale) {
    uint32_t target = purp_cursor_target_px(theme->base_size, scale);
    for (size_t i = 0; i < PURP_CURSOR_CACHE_ENTRIES; ++i) {
        const purp_cursor_cache_entry_t *entry = &theme->cache[i];
        if (entry->in_use && entry->shape == shape && entry->target_px == target) {
            if (!entry->missing) return &entry->image;
            goto fallback;
        }
    }
    {
        /* Not cached: fill an unused slot, else evict round-robin. */
        purp_cursor_cache_entry_t *entry = NULL;
        for (size_t i = 0; i < PURP_CURSOR_CACHE_ENTRIES; ++i)
            if (!theme->cache[i].in_use) {
                entry = &theme->cache[i];
                break;
            }
        if (!entry) {
            entry = &theme->cache[theme->cache_next % PURP_CURSOR_CACHE_ENTRIES];
            theme->cache_next++;
            purp_pixel_buf_free(&entry->image.buffer);
        }
        *entry = (purp_cursor_cache_entry_t){ .in_use = true, .shape = shape,
                                              .target_px = target };
        entry->missing = !purp_cursor_load_named(theme, shape, target, &entry->image);
        if (!entry->missing) return &entry->image;
    }
fallback:
    /* Theme miss: default shape, then the built-in arrow. Never NULL. */
    if (shape != PURP_CURSOR_DEFAULT)
        return purp_cursor_theme_get(theme, PURP_CURSOR_DEFAULT, scale);
    return purp_cursor_fallback_arrow(theme, target);
}
