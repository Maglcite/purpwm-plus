#include "ui.h"

#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits.h>
#include <stdatomic.h>
#include <unistd.h>

struct purp_font {
    FT_Library library;
    FT_Face face;
    char *source;
    size_t cache_len;
    struct {
        bool used;
        uint32_t cp;
        int32_t px_tenths;
        purp_glyph_t glyph;
    } cache[PURP_GLYPH_CACHE_LIMIT];
};

static atomic_uint_fast64_t purp_next_pixel_id = 1;

static uint64_t pixel_id_next(void) {
    uint64_t id = atomic_fetch_add_explicit(&purp_next_pixel_id, 1, memory_order_relaxed);
    if (id == 0) id = atomic_fetch_add_explicit(&purp_next_pixel_id, 1, memory_order_relaxed);
    return id;
}

bool purp_pixel_buf_ensure(purp_pixel_buf_t *buf, int32_t width, int32_t height) {
    if (!buf || width < 1 || height < 1 || width > INT32_MAX / 4) return false;
    int32_t stride = width * 4;
    if ((size_t)height > SIZE_MAX / (size_t)stride) return false;
    if (buf->data && buf->width == width && buf->height == height && buf->stride == stride)
        return true;

    size_t bytes = (size_t)stride * (size_t)height;
    uint8_t *data = calloc(1, bytes);
    if (!data) return false;
    free(buf->data);
    if (buf->id == 0) buf->id = pixel_id_next();
    buf->data = data;
    buf->width = width;
    buf->height = height;
    buf->stride = stride;
    buf->generation++;
    return true;
}

uint32_t purp_rgba_premultiply(purp_rgba_t rgba) {
    uint32_t a = rgba.a;
    uint32_t r = (uint32_t)rgba.r * a / 255u;
    uint32_t g = (uint32_t)rgba.g * a / 255u;
    uint32_t b = (uint32_t)rgba.b * a / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void purp_pixel_buf_clear(purp_pixel_buf_t *buf, purp_rgba_t rgba) {
    if (!buf || !buf->data || buf->width <= 0 || buf->height <= 0) return;
    uint32_t pixel = purp_rgba_premultiply(rgba);
    for (int32_t y = 0; y < buf->height; ++y) {
        uint32_t *row = (uint32_t *)(void *)(buf->data + (size_t)y * (size_t)buf->stride);
        for (int32_t x = 0; x < buf->width; ++x) row[x] = pixel;
    }
    buf->generation++;
}

void purp_pixel_buf_touch(purp_pixel_buf_t *buf) {
    if (buf) buf->generation++;
}

void purp_pixel_buf_free(purp_pixel_buf_t *buf) {
    if (!buf) return;
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

void purp_pixel_put(purp_pixel_buf_t *buf, int32_t x, int32_t y, purp_rgba_t rgba) {
    if (!buf || !buf->data || x < 0 || y < 0 || x >= buf->width || y >= buf->height) return;
    uint32_t *pixels = (uint32_t *)(void *)(buf->data + (size_t)y * (size_t)buf->stride);
    pixels[x] = purp_rgba_premultiply(rgba);
}

void purp_pixel_blend(purp_pixel_buf_t *buf, int32_t x, int32_t y, purp_rgba_t rgba,
                      uint8_t coverage) {
    if (!buf || !buf->data || x < 0 || y < 0 || x >= buf->width || y >= buf->height ||
        coverage == 0)
        return;
    uint32_t src_a = (uint32_t)rgba.a * coverage / 255u;
    if (src_a == 0) return;
    uint32_t inv_a = 255u - src_a;
    uint32_t src_r = (uint32_t)rgba.r * src_a / 255u;
    uint32_t src_g = (uint32_t)rgba.g * src_a / 255u;
    uint32_t src_b = (uint32_t)rgba.b * src_a / 255u;
    uint32_t *pixels = (uint32_t *)(void *)(buf->data + (size_t)y * (size_t)buf->stride);
    uint32_t dst = pixels[x];
    uint32_t dst_a = (dst >> 24) & 0xffu;
    uint32_t dst_r = (dst >> 16) & 0xffu;
    uint32_t dst_g = (dst >> 8) & 0xffu;
    uint32_t dst_b = dst & 0xffu;
    uint32_t out_a = src_a + dst_a * inv_a / 255u;
    uint32_t out_r = src_r + dst_r * inv_a / 255u;
    uint32_t out_g = src_g + dst_g * inv_a / 255u;
    uint32_t out_b = src_b + dst_b * inv_a / 255u;
    if (out_a > 255u) out_a = 255u;
    if (out_r > 255u) out_r = 255u;
    if (out_g > 255u) out_g = 255u;
    if (out_b > 255u) out_b = 255u;
    pixels[x] = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static bool rect_clip(const purp_pixel_buf_t *buf, int32_t x, int32_t y, int32_t w, int32_t h,
                      int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
    if (!buf || !buf->data || w <= 0 || h <= 0) return false;
    int64_t left = x, top = y, right = (int64_t)x + w, bottom = (int64_t)y + h;
    left = left < 0 ? 0 : (left > buf->width ? buf->width : left);
    top = top < 0 ? 0 : (top > buf->height ? buf->height : top);
    right = right < 0 ? 0 : (right > buf->width ? buf->width : right);
    bottom = bottom < 0 ? 0 : (bottom > buf->height ? buf->height : bottom);
    if (right <= left || bottom <= top) return false;
    *x0 = (int32_t)left;
    *y0 = (int32_t)top;
    *x1 = (int32_t)right;
    *y1 = (int32_t)bottom;
    return true;
}

void purp_fill_rect(purp_pixel_buf_t *buf, int32_t x, int32_t y, int32_t w, int32_t h,
                    purp_rgba_t rgba) {
    int32_t x0, y0, x1, y1;
    if (!rect_clip(buf, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    uint32_t pixel = purp_rgba_premultiply(rgba);
    for (int32_t row_y = y0; row_y < y1; ++row_y) {
        uint32_t *row = (uint32_t *)(void *)(buf->data + (size_t)row_y * (size_t)buf->stride);
        for (int32_t col = x0; col < x1; ++col) row[col] = pixel;
    }
}

void purp_blend_rect(purp_pixel_buf_t *buf, int32_t x, int32_t y, int32_t w, int32_t h,
                     purp_rgba_t rgba) {
    int32_t x0, y0, x1, y1;
    if (!rect_clip(buf, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (int32_t row = y0; row < y1; ++row)
        for (int32_t col = x0; col < x1; ++col) purp_pixel_blend(buf, col, row, rgba, 255);
}

void purp_stroke_rect(purp_pixel_buf_t *buf, int32_t x, int32_t y, int32_t w, int32_t h,
                      int32_t thickness, purp_rgba_t rgba) {
    if (thickness <= 0 || w <= 0 || h <= 0) return;
    int32_t t = purp_min_i32(thickness, purp_min_i32(w, h));
    purp_fill_rect(buf, x, y, w, t, rgba);
    purp_fill_rect(buf, x, y + h - t, w, t, rgba);
    if (h > 2 * t) {
        purp_fill_rect(buf, x, y + t, t, h - 2 * t, rgba);
        purp_fill_rect(buf, x + w - t, y + t, t, h - 2 * t, rgba);
    }
}

uint32_t purp_utf8_decode(const char **s) {
    if (!s || !*s) return 0;
    const uint8_t *p = (const uint8_t *)*s;
    if (p[0] == 0) return 0;
    if (p[0] < 0x80) {
        *s = (const char *)(p + 1);
        return p[0];
    }

    size_t need;
    uint32_t cp;
    uint32_t minimum;
    if (p[0] >= 0xc2 && p[0] <= 0xdf) {
        need = 2;
        cp = p[0] & 0x1fu;
        minimum = 0x80;
    } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
        need = 3;
        cp = p[0] & 0x0fu;
        minimum = 0x800;
    } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
        need = 4;
        cp = p[0] & 0x07u;
        minimum = 0x10000;
    } else {
        *s = (const char *)(p + 1);
        return 0xfffdu;
    }
    for (size_t i = 1; i < need; ++i) {
        if ((p[i] & 0xc0u) != 0x80u) {
            *s = (const char *)(p + 1);
            return 0xfffdu;
        }
        cp = (cp << 6) | (p[i] & 0x3fu);
    }
    if (cp < minimum || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        *s = (const char *)(p + 1);
        return 0xfffdu;
    }
    *s = (const char *)(p + need);
    return cp;
}

size_t purp_utf8_encode(uint32_t cp, char *out) {
    if (!out || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) return 0;
    if (cp <= 0x7fu) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7ffu) {
        out[0] = (char)(0xc0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3fu));
        return 2;
    }
    if (cp <= 0xffffu) {
        out[0] = (char)(0xe0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (cp & 0x3fu));
        return 3;
    }
    out[0] = (char)(0xf0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
    out[3] = (char)(0x80u | (cp & 0x3fu));
    return 4;
}

size_t purp_utf8_count(const char *s) {
    if (!s) return 0;
    size_t count = 0;
    while (*s) {
        purp_utf8_decode(&s);
        count++;
    }
    return count;
}

size_t purp_utf8_truncate(char *dst, size_t dst_len, const char *src, size_t max_chars) {
    if (!dst || dst_len == 0) return 0;
    size_t written = 0, chars = 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    const char *p = src;
    while (*p && chars < max_chars) {
        const char *start = p;
        purp_utf8_decode(&p);
        size_t bytes = (size_t)(p - start);
        if (bytes >= dst_len - written) break;
        memcpy(dst + written, start, bytes);
        written += bytes;
        chars++;
    }
    dst[written] = '\0';
    return written;
}

static bool utf8_space(uint32_t cp) {
    if (cp >= 0x09u && cp <= 0x0du) return true;
    return cp == 0x20u || cp == 0x85u || cp == 0xa0u || cp == 0x1680u ||
           (cp >= 0x2000u && cp <= 0x200au) || cp == 0x2028u || cp == 0x2029u ||
           cp == 0x202fu || cp == 0x205fu || cp == 0x3000u;
}

size_t purp_utf8_sanitize_line(char *dst, size_t dst_len, const char *src, size_t max_chars) {
    if (!dst || dst_len == 0) return 0;
    dst[0] = '\0';
    if (!src || max_chars == 0) return 0;

    const char *p = src, *first = NULL, *last = NULL;
    while (*p) {
        const char *start = p;
        uint32_t cp = purp_utf8_decode(&p);
        if (!utf8_space(cp)) {
            if (!first) first = start;
            last = p;
        }
    }
    if (!first || !last || first >= last) return 0;

    size_t written = 0, chars = 0;
    p = first;
    while (p < last && *p && chars < max_chars) {
        const char *start = p;
        uint32_t cp = purp_utf8_decode(&p);
        if (p > last) p = last;
        const char *source = start;
        char replacement = ' ';
        size_t bytes;
        if (cp == '\n' || cp == '\r' || cp == '\t') {
            source = &replacement;
            bytes = 1;
        } else {
            bytes = (size_t)(p - start);
        }
        if (bytes >= dst_len - written) break;
        memcpy(dst + written, source, bytes);
        written += bytes;
        chars++;
    }
    dst[written] = '\0';
    return written;
}

size_t purp_utf8_pop_wipe(char *s) {
    if (!s || !*s) return 0;
    const char *p = s, *last = s;
    while (*p) {
        last = p;
        purp_utf8_decode(&p);
    }
    size_t bytes = strlen(last);
    explicit_bzero((char *)last, bytes + 1);
    return bytes;
}

static purp_font_t *font_alloc(void) {
    purp_font_t *font = calloc(1, sizeof(*font));
    if (!font) return NULL;
    FT_Error error = FT_Init_FreeType(&font->library);
    if (error) {
        free(font);
        return NULL;
    }
    return font;
}

purp_font_t *purp_font_load_file(const char *path) {
    if (!path || !*path) return NULL;
    purp_font_t *font = font_alloc();
    if (!font) return NULL;
    FT_Error error = FT_New_Face(font->library, path, 0, &font->face);
    if (error) {
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }
    font->source = strdup(path);
    if (!font->source) {
        FT_Done_Face(font->face);
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }
    return font;
}

purp_font_t *purp_font_load_memory(const unsigned char *data, size_t len) {
    if (!data || len == 0 || len > (size_t)LONG_MAX) return NULL;
    purp_font_t *font = font_alloc();
    if (!font) return NULL;
    FT_Error error = FT_New_Memory_Face(font->library, data, (FT_Long)len, 0, &font->face);
    if (error) {
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }
    font->source = strdup("<bundled>");
    if (!font->source) {
        FT_Done_Face(font->face);
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }
    return font;
}

static purp_font_t *try_font_path(const char *path, bool log_missing) {
    if (!path || !*path || (!log_missing && access(path, R_OK) != 0)) return NULL;
    purp_font_t *font = purp_font_load_file(path);
    if (font) PURP_INFO("Loaded font: %s", path);
    else PURP_WARN("Unusable font %s: FreeType could not load the face", path);
    return font;
}

purp_font_t *purp_font_load(void) {
    const char *configured = getenv(PURP_ENV_FONT);
    if (configured && *configured) {
        purp_font_t *font = try_font_path(configured, true);
        if (font) return font;
    }
    for (size_t i = 0; i < PURP_ARRAY_LEN(PURP_FONT_PATHS); ++i) {
        purp_font_t *font = try_font_path(PURP_FONT_PATHS[i], false);
        if (font) return font;
    }

    if (FcInit()) {
        FcPattern *pattern = FcNameParse((const FcChar8 *)"monospace");
        if (pattern) {
            FcConfig *config = FcConfigGetCurrent();
            if (config && FcConfigSubstitute(config, pattern, FcMatchPattern)) {
                FcDefaultSubstitute(pattern);
                FcResult result;
                FcPattern *match = FcFontMatch(config, pattern, &result);
                if (match) {
                    FcChar8 *file = NULL;
                    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
                        purp_font_t *font = try_font_path((const char *)file, true);
                        FcPatternDestroy(match);
                        FcPatternDestroy(pattern);
                        if (font) return font;
                        goto fontconfig_done;
                    }
                    FcPatternDestroy(match);
                }
            }
            FcPatternDestroy(pattern);
        }
    }

fontconfig_done:
    PURP_WARN("No system font usable; using the bundled DejaVu Sans Mono");
    return purp_font_load_memory(purp_bundled_font, purp_bundled_font_len);
}

void purp_font_destroy(purp_font_t *font) {
    if (!font) return;
    for (size_t i = 0; i < PURP_GLYPH_CACHE_LIMIT; ++i) free(font->cache[i].glyph.coverage);
    if (font->face) FT_Done_Face(font->face);
    if (font->library) FT_Done_FreeType(font->library);
    free(font->source);
    free(font);
}

const char *purp_font_source(const purp_font_t *font) {
    return font && font->source ? font->source : "<bundled>";
}

static int32_t font_size_key(float px) {
    if (!isfinite(px) || px <= 0.0f) return 1;
    double scaled = floor((double)px * 10.0 + 0.5);
    if (scaled >= INT32_MAX) return INT32_MAX;
    if (scaled < 0) return 0;
    return (int32_t)scaled;
}

static bool font_set_size(purp_font_t *font, int32_t px_tenths) {
    int64_t size = ((int64_t)px_tenths * 64 + 5) / 10;
    if (size < 1) size = 1;
    if (size > INT32_MAX) size = INT32_MAX;
    return FT_Set_Char_Size(font->face, 0, (FT_F26Dot6)size, 72, 72) == 0;
}

static void cache_clear(purp_font_t *font) {
    for (size_t i = 0; i < PURP_GLYPH_CACHE_LIMIT; ++i) {
        free(font->cache[i].glyph.coverage);
        font->cache[i].glyph.coverage = NULL;
        font->cache[i].used = false;
    }
    font->cache_len = 0;
}

const purp_glyph_t *purp_font_glyph(purp_font_t *font, uint32_t cp, float px) {
    if (!font || !font->face) return NULL;
    int32_t key = font_size_key(px);
    size_t free_slot = PURP_GLYPH_CACHE_LIMIT;
    for (size_t i = 0; i < PURP_GLYPH_CACHE_LIMIT; ++i) {
        if (font->cache[i].used && font->cache[i].cp == cp && font->cache[i].px_tenths == key)
            return &font->cache[i].glyph;
        if (!font->cache[i].used && free_slot == PURP_GLYPH_CACHE_LIMIT) free_slot = i;
    }
    if (free_slot == PURP_GLYPH_CACHE_LIMIT) {
        cache_clear(font);
        free_slot = 0;
    }
    if (!font_set_size(font, key)) return NULL;

    uint32_t draw_cp = cp;
    if (FT_Get_Char_Index(font->face, draw_cp) == 0 && draw_cp != '?') draw_cp = '?';
    if (FT_Load_Char(font->face, draw_cp, FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL) != 0)
        return NULL;
    FT_GlyphSlot slot = font->face->glyph;
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return NULL;

    purp_glyph_t glyph = {
        .width = (int32_t)slot->bitmap.width,
        .height = (int32_t)slot->bitmap.rows,
        .left = slot->bitmap_left,
        .top = slot->bitmap_top,
        .advance = (int32_t)ceil((double)slot->advance.x / 64.0),
    };
    if (glyph.width > 0 && glyph.height > 0) {
        size_t bytes = (size_t)glyph.width * (size_t)glyph.height;
        if (glyph.height != 0 && bytes / (size_t)glyph.height != (size_t)glyph.width) return NULL;
        glyph.coverage = malloc(bytes);
        if (!glyph.coverage) return NULL;
        int pitch = slot->bitmap.pitch;
        for (int32_t y = 0; y < glyph.height; ++y) {
            const uint8_t *src;
            if (pitch >= 0) src = slot->bitmap.buffer + (size_t)y * (size_t)pitch;
            else src = slot->bitmap.buffer + (size_t)(glyph.height - 1 - y) * (size_t)(-pitch);
            memcpy(glyph.coverage + (size_t)y * (size_t)glyph.width, src, (size_t)glyph.width);
        }
    }

    font->cache[free_slot].used = true;
    font->cache[free_slot].cp = cp;
    font->cache[free_slot].px_tenths = key;
    font->cache[free_slot].glyph = glyph;
    font->cache_len++;
    return &font->cache[free_slot].glyph;
}

void purp_font_metrics(purp_font_t *font, float px, int32_t *ascent, int32_t *descent) {
    if (ascent) *ascent = 0;
    if (descent) *descent = 0;
    if (!font || !font->face || !font_set_size(font, font_size_key(px))) return;
    if (ascent) *ascent = (int32_t)ceil((double)font->face->size->metrics.ascender / 64.0);
    if (descent) *descent = (int32_t)ceil((double)-font->face->size->metrics.descender / 64.0);
}

int32_t purp_font_measure(purp_font_t *font, const char *text, float px) {
    if (!text) return 0;
    if (!font) {
        int64_t width = (int64_t)purp_utf8_count(text) * purp_max_i32((int32_t)(px * 0.6f), 8);
        return width > INT32_MAX ? INT32_MAX : (int32_t)width;
    }
    int64_t width = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp = purp_utf8_decode(&p);
        const purp_glyph_t *glyph = purp_font_glyph(font, cp, px);
        if (glyph) width += glyph->advance;
        if (width >= INT32_MAX) return INT32_MAX;
    }
    return (int32_t)width;
}

static int32_t font_draw_inner(purp_font_t *font, purp_pixel_buf_t *buf, const char *text,
                               int32_t x, int32_t baseline_y, float px, int32_t max_w,
                               purp_rgba_t rgba, bool clipped) {
    if (!font || !text) return x;
    int64_t pen = x;
    const char *p = text;
    while (*p) {
        uint32_t cp = purp_utf8_decode(&p);
        const purp_glyph_t *glyph = purp_font_glyph(font, cp, px);
        if (!glyph) continue;
        if (clipped && (max_w <= 0 || pen + glyph->advance > (int64_t)x + max_w)) break;
        if (buf && glyph->coverage) {
            int32_t gx = (int32_t)(pen + glyph->left);
            int32_t gy = baseline_y - glyph->top;
            for (int32_t row = 0; row < glyph->height; ++row)
                for (int32_t col = 0; col < glyph->width; ++col) {
                    uint8_t coverage = glyph->coverage[(size_t)row * (size_t)glyph->width + (size_t)col];
                    if (coverage) purp_pixel_blend(buf, gx + col, gy + row, rgba, coverage);
                }
        }
        pen += glyph->advance;
        if (pen > INT32_MAX) return INT32_MAX;
    }
    return (int32_t)pen;
}

int32_t purp_font_draw(purp_font_t *font, purp_pixel_buf_t *buf, const char *text, int32_t x,
                       int32_t baseline_y, float px, purp_rgba_t rgba) {
    return font_draw_inner(font, buf, text, x, baseline_y, px, 0, rgba, false);
}

int32_t purp_font_draw_clipped(purp_font_t *font, purp_pixel_buf_t *buf, const char *text,
                               int32_t x, int32_t baseline_y, float px, int32_t max_w,
                               purp_rgba_t rgba) {
    return font_draw_inner(font, buf, text, x, baseline_y, px, max_w, rgba, true);
}

size_t purp_font_clip_to_width(purp_font_t *font, const char *text, float px, int32_t max_w,
                               char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return 0;
    dst[0] = '\0';
    if (!text || max_w < 0) return 0;
    size_t written = 0;
    int64_t width = 0;
    const char *p = text;
    while (*p) {
        const char *start = p;
        uint32_t cp = purp_utf8_decode(&p);
        int32_t advance;
        if (font) {
            const purp_glyph_t *glyph = purp_font_glyph(font, cp, px);
            if (!glyph) continue;
            advance = glyph->advance;
        } else {
            advance = purp_max_i32((int32_t)(px * 0.6f), 8);
        }
        if (width + advance > max_w) break;
        size_t bytes = (size_t)(p - start);
        if (bytes >= dst_len - written) break;
        memcpy(dst + written, start, bytes);
        written += bytes;
        width += advance;
    }
    dst[written] = '\0';
    return written;
}
