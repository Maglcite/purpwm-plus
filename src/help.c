#include "ui.h"

static const purp_help_row_t session_rows[] = {
    { "Mod + h", "Show / hide this help" },
    { "Mod + q", "Close window" },
    { "Mod + Shift + q", "Quit purpwm+" },
    { "Mod + Shift + l", "Lock screen" },
};
static const purp_help_row_t launch_rows[] = {
    { "Tap Mod", "App launcher" },
    { "Mod + p", "App launcher" },
    { "Mod + Return", "Terminal" },
    { "Mod + s  /  Mod + `", "Scratchpad terminal" },
    { "Mod + Esc", "System monitor" },
    { "Mod + w", "Wi-Fi manager" },
    { "Mod + Shift + w", "Wi-Fi on / off" },
    { "Mod + Shift + b", "Bluetooth manager" },
    { "Mod + Ctrl + b", "Bluetooth on / off" },
};
static const purp_help_row_t window_rows[] = {
    { "Mod + j  /  k", "Focus next / previous" },
    { "Mod + Shift + Return", "Swap with master" },
    { "Mod + Arrows", "Move window" },
    { "Mod + Shift + Arrows", "Resize (hold to glide)" },
    { "Mod + Shift + Space", "Toggle floating" },
    { "Mod + Shift + f  /  Mod + F11", "Fullscreen" },
    { "Mod + o", "Fully opaque window" },
    { "Mod + Left-drag", "Move window" },
    { "Mod + Right-drag", "Resize window" },
    { "Mod + Middle-click", "Toggle floating" },
    { "Mod + ,  /  .", "Focus monitor" },
    { "Mod + Shift + ,  /  .", "Move to monitor" },
};
static const purp_help_row_t layout_rows[] = {
    { "Mod + t", "Tile  []=" },
    { "Mod + u", "Centered master  |M|" },
    { "Mod + g", "Grid  ###" },
    { "Mod + m", "Monocle  [M]" },
    { "Mod + f", "Floating  ><>" },
    { "Mod + Space", "Previous layout" },
    { "Mod + Ctrl + Space", "Cycle layouts" },
    { "Mod + [  /  ]", "Shrink / grow master" },
    { "Mod + i  /  d", "More / fewer masters" },
};
static const purp_help_row_t tag_rows[] = {
    { "Mod + 1..9", "View tag" },
    { "Mod + Shift + 1..9", "Move window to tag" },
    { "Mod + Ctrl + 1..9", "Add tag to view" },
    { "Mod + Ctrl + Shift + 1..9", "Toggle window on tag" },
    { "Mod + 0", "View all tags" },
    { "Mod + Shift + 0", "Window on all tags" },
    { "Mod + Tab", "Previous tags" },
};
static const purp_help_row_t look_rows[] = {
    { "Mod + Ctrl + =  /  -", "Bigger / smaller UI" },
    { "Mod + =  /  -", "More / less gap" },
    { "Mod + Shift + g", "Gaps on / off" },
    { "Mod + Ctrl + 0", "Reset gaps" },
    { "Mod + b", "Show / hide bar" },
};
static const purp_help_row_t media_rows[] = {
    { "Print", "Screenshot" },
    { "Shift + Print", "Screenshot window" },
    { "Volume keys", "Volume / mute" },
    { "Brightness keys", "Brightness" },
    { "Media keys", "Play / next / previous" },
};

const purp_help_section_t purp_help_sections[] = {
    { "Session", session_rows, PURP_ARRAY_LEN(session_rows) },
    { "Launch", launch_rows, PURP_ARRAY_LEN(launch_rows) },
    { "Windows", window_rows, PURP_ARRAY_LEN(window_rows) },
    { "Layouts", layout_rows, PURP_ARRAY_LEN(layout_rows) },
    { "Tags", tag_rows, PURP_ARRAY_LEN(tag_rows) },
    { "Look", look_rows, PURP_ARRAY_LEN(look_rows) },
    { "Media & screenshots", media_rows, PURP_ARRAY_LEN(media_rows) },
};
const size_t purp_help_section_count = PURP_ARRAY_LEN(purp_help_sections);

typedef struct help_layout {
    float px;
    size_t columns[PURP_HELP_COLUMNS][PURP_ARRAY_LEN(purp_help_sections)];
    size_t column_counts[PURP_HELP_COLUMNS];
    size_t column_count;
    int32_t key_w;
    int32_t col_w;
    int32_t line_h;
    int32_t pad;
    int32_t gap;
    int32_t header_h;
    int32_t width;
    int32_t height;
} help_layout_t;

static help_layout_t help_measure(purp_font_t *font, float px) {
    help_layout_t layout;
    memset(&layout, 0, sizeof(layout));
    layout.px = px;
    layout.line_h = purp_round_i32((double)px * 1.6);
    layout.pad = purp_round_i32((double)px * 1.6);
    layout.gap = purp_round_i32((double)px * 1.4);
    layout.header_h = purp_round_i32((double)px * 1.4 * 2.2);
    int32_t desc_w = 0;
    size_t section_lines[PURP_ARRAY_LEN(purp_help_sections)];
    size_t total_lines = 0;
    for (size_t i = 0; i < purp_help_section_count; ++i) {
        const purp_help_section_t *section = &purp_help_sections[i];
        section_lines[i] = section->row_count + 2;
        total_lines += section_lines[i];
        for (size_t j = 0; j < section->row_count; ++j) {
            layout.key_w = purp_max_i32(layout.key_w,
                                        purp_font_measure(font, section->rows[j].keys, px));
            desc_w = purp_max_i32(desc_w, purp_font_measure(font, section->rows[j].description, px));
        }
    }
    layout.col_w = layout.key_w + layout.gap + desc_w;
    size_t share = (total_lines + PURP_HELP_COLUMNS - 1) / PURP_HELP_COLUMNS;
    layout.column_count = 1;
    size_t filled = 0;
    for (size_t i = 0; i < purp_help_section_count; ++i) {
        if (filled >= share && layout.column_count < PURP_HELP_COLUMNS) {
            layout.column_count++;
            filled = 0;
        }
        size_t column = layout.column_count - 1;
        layout.columns[column][layout.column_counts[column]++] = i;
        filled += section_lines[i];
    }
    size_t tallest = 0;
    for (size_t column = 0; column < layout.column_count; ++column) {
        size_t lines = 0;
        for (size_t i = 0; i < layout.column_counts[column]; ++i)
            lines += section_lines[layout.columns[column][i]];
        if (lines > tallest) tallest = lines;
    }
    int32_t columns = (int32_t)layout.column_count;
    layout.width = layout.pad * 2 + layout.col_w * columns + layout.gap * 2 * (columns - 1);
    layout.height = layout.pad * 2 + layout.header_h + (int32_t)tallest * layout.line_h;
    return layout;
}

static void help_draw_top(purp_font_t *font, purp_pixel_buf_t *buffer, const char *text, int32_t x,
                          int32_t top, float px, purp_rgba_t color) {
    purp_font_draw(font, buffer, text, x, top + (int32_t)(px * 0.8f), px, color);
}

static void help_paint(const help_layout_t *layout, purp_font_t *font, purp_pixel_buf_t *buffer) {
    int32_t w = layout->width, h = layout->height;
    float px = layout->px;
    int32_t border = purp_max_i32(purp_round_i32(px / 7.0f), 2);
    purp_fill_rect(buffer, 0, 0, w, h, PURP_BAR_ACCENT_RGBA);
    purp_fill_rect(buffer, border, border, w - 2 * border, h - 2 * border, PURP_HELP_PANEL_BG_RGBA);

    float title_px = px * 1.4f;
    help_draw_top(font, buffer, PURP_HELP_TITLE, layout->pad, layout->pad, title_px,
                  PURP_BAR_TEXT_FG_RGBA);
    int32_t hint_x = w - layout->pad - purp_font_measure(font, PURP_HELP_HINT, px);
    int32_t hint_top = layout->pad + (int32_t)((title_px - px) * 0.8f);
    help_draw_top(font, buffer, PURP_HELP_HINT, hint_x, hint_top, px, PURP_BAR_TEXT_DIM_RGBA);

    for (size_t column = 0; column < layout->column_count; ++column) {
        int32_t x = layout->pad + (int32_t)column * (layout->col_w + layout->gap * 2);
        int32_t y = layout->pad + layout->header_h;
        for (size_t n = 0; n < layout->column_counts[column]; ++n) {
            const purp_help_section_t *section = &purp_help_sections[layout->columns[column][n]];
            help_draw_top(font, buffer, section->title, x, y, px, PURP_HELP_SECTION_RGBA);
            purp_fill_rect(buffer, x, y + layout->line_h - (int32_t)(px * 0.35f), layout->col_w, 1,
                           PURP_BAR_ACCENT_RGBA);
            y += layout->line_h;
            for (size_t row = 0; row < section->row_count; ++row) {
                help_draw_top(font, buffer, section->rows[row].keys, x, y, px, PURP_BAR_ACCENT_RGBA);
                help_draw_top(font, buffer, section->rows[row].description,
                              x + layout->key_w + layout->gap, y, px, PURP_BAR_TEXT_FG_RGBA);
                y += layout->line_h;
            }
            y += layout->line_h;
        }
    }
    purp_pixel_buf_touch(buffer);
}

bool purp_help_rasterize(purp_font_t *font, int32_t width, int32_t height, double scale,
                         purp_pixel_buf_t *out, purp_point_t *origin) {
    if (!out || !origin || !isfinite(scale) || scale <= 0) return false;
    float px = 15.0f * (float)scale;
    help_layout_t layout;
    for (;;) {
        layout = help_measure(font, px);
        bool fits = layout.width <= width * 95 / 100 && layout.height <= height * 92 / 100;
        if (fits || px < 8.0f) break;
        px *= 0.9f;
    }
    origin->x = purp_max_i32((width - layout.width) / 2, 0);
    origin->y = purp_max_i32((height - layout.height) / 2, 0);
    if (!purp_pixel_buf_ensure(out, purp_max_i32(layout.width, 1), purp_max_i32(layout.height, 1)))
        return false;
    help_paint(&layout, font, out);
    return true;
}

void purp_help_init(purp_help_overlay_t *help) {
    if (help) memset(help, 0, sizeof(*help));
}

void purp_help_finish(purp_help_overlay_t *help) {
    if (!help) return;
    for (size_t i = 0; i < PURP_HELP_CACHE_ENTRIES; ++i) purp_pixel_buf_free(&help->cache[i].buffer);
    memset(help, 0, sizeof(*help));
}

void purp_help_toggle(purp_help_overlay_t *help) {
    if (help) help->visible = !help->visible;
}

static void help_cache_clear(purp_help_overlay_t *help) {
    for (size_t i = 0; i < PURP_HELP_CACHE_ENTRIES; ++i) {
        purp_pixel_buf_free(&help->cache[i].buffer);
        memset(&help->cache[i], 0, sizeof(help->cache[i]));
    }
}

const purp_pixel_buf_t *purp_help_render(purp_help_overlay_t *help, purp_font_t *font,
                                         int32_t width, int32_t height, double scale,
                                         purp_point_t *origin) {
    if (!help || !help->visible || !origin) return NULL;
    purp_help_cache_entry_t *free_entry = NULL;
    for (size_t i = 0; i < PURP_HELP_CACHE_ENTRIES; ++i) {
        purp_help_cache_entry_t *entry = &help->cache[i];
        if (entry->in_use && entry->width == width && entry->height == height && entry->scale == scale) {
            *origin = entry->origin;
            return &entry->buffer;
        }
        if (!entry->in_use && !free_entry) free_entry = entry;
    }
    if (!free_entry) {
        help_cache_clear(help);
        free_entry = &help->cache[0];
    }
    purp_point_t location;
    if (!purp_help_rasterize(font, width, height, scale, &free_entry->buffer, &location)) return NULL;
    free_entry->in_use = true;
    free_entry->width = width;
    free_entry->height = height;
    free_entry->scale = scale;
    free_entry->origin = location;
    *origin = location;
    return &free_entry->buffer;
}
