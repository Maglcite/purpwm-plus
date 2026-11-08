/*
 * purpwm+ — software-rasterised UI: font + pixel helpers, status bar, PAM lock screen, desktop
 * notifications, Mod+h help sheet, XCursor theme and wallpaper.
 *
 *   src/font.c       FreeType2 + Fontconfig font loading (compiled-in DejaVu Sans Mono fallback),
 *                    UTF-8 decoding, 1024-entry glyph cache, premultiplied ARGB8888 helpers
 *   src/bar.c        per-monitor status bar raster, pill hit-testing, background status poller
 *   src/lock.c       PAM screen locker (worker thread per attempt, 15 s timeout)
 *   src/notify.c     native GDBus org.freedesktop.Notifications server + toast raster
 *   src/help.c       Mod+h 3-column keybinding cheat sheet raster
 *   src/cursor.c     libXcursor theme loader + built-in purple arrow (Slice 3)
 *   src/wallpaper.c  gdk-pixbuf cover-crop wallpaper composited over #11081F (Slice 3)
 *
 * Pure header: no libwayland, no GL, no GLib/FreeType types (those stay private to the .c files).
 * Every raster is a purp_pixel_buf_t (types.h) in PHYSICAL pixels that render.c uploads; UI
 * objects are driven from the compositor thread only, except where a function says otherwise.
 * All of these modules are linked into the unit tests.
 */
#ifndef PURPWM_UI_H
#define PURPWM_UI_H

#include <pthread.h>

#include "config.h"
#include "types.h"

/* =========================================================================================== */
/* Pixel buffers and drawing helpers (src/font.c)                                             */
/* =========================================================================================== */

/* Ensure `buf` is width x height (each >= 1), allocating a process-unique id on first use.
 * Contents are cleared to transparent when the size changes; generation is bumped. */
bool purp_pixel_buf_ensure(purp_pixel_buf_t *buf, int32_t width, int32_t height);
/* Fill the whole buffer with `rgba` (premultiplied on write) and bump generation. */
void purp_pixel_buf_clear(purp_pixel_buf_t *buf, purp_rgba_t rgba);
/* Mark the contents changed (render.c re-uploads on the next frame). */
void purp_pixel_buf_touch(purp_pixel_buf_t *buf);
/* Free pixels and zero the struct (render.c evicts unused textures by itself). */
void purp_pixel_buf_free(purp_pixel_buf_t *buf);
/* Premultiply a straight colour into one ARGB8888 word (0xAARRGGBB). */
uint32_t purp_rgba_premultiply(purp_rgba_t rgba);

/* Replace pixel (x, y) with `rgba` (premultiplied); out-of-bounds is ignored. */
void purp_pixel_put(purp_pixel_buf_t *buf, int32_t x, int32_t y, purp_rgba_t rgba);
/* Source-over blend `rgba` scaled by `coverage` (0..255, glyph antialiasing) onto (x, y). */
void purp_pixel_blend(purp_pixel_buf_t *buf, int32_t x, int32_t y, purp_rgba_t rgba,
                      uint8_t coverage);
/* Replace a clipped rectangle with `rgba` (bar pills, panels). */
void purp_fill_rect(purp_pixel_buf_t *buf, int32_t x, int32_t y, int32_t w, int32_t h,
                    purp_rgba_t rgba);
/* Source-over blend a clipped rectangle. */
void purp_blend_rect(purp_pixel_buf_t *buf, int32_t x, int32_t y, int32_t w, int32_t h,
                     purp_rgba_t rgba);
/* Outline of `thickness` px drawn inside the rectangle. */
void purp_stroke_rect(purp_pixel_buf_t *buf, int32_t x, int32_t y, int32_t w, int32_t h,
                      int32_t thickness, purp_rgba_t rgba);

/* =========================================================================================== */
/* UTF-8 (src/font.c)                                                                         */
/* =========================================================================================== */

/* Decode one code point at *s and advance *s past it (never past the NUL). Invalid or truncated
 * sequences yield U+FFFD and advance one byte. Returns 0 at the terminating NUL. */
uint32_t purp_utf8_decode(const char **s);
/* Encode `cp` into out (>= 4 bytes); returns the byte count (0 for invalid code points). */
size_t purp_utf8_encode(uint32_t cp, char *out);
/* Number of code points in `s`. */
size_t purp_utf8_count(const char *s);
/* Copy at most `max_chars` code points of `src` into dst (always NUL-terminated, never splitting a
 * sequence). Returns the bytes written. */
size_t purp_utf8_truncate(char *dst, size_t dst_len, const char *src, size_t max_chars);
/* Copy `src` trimmed of surrounding whitespace with \n \r \t turned into spaces, limited to
 * `max_chars` code points (notification sanitising). */
size_t purp_utf8_sanitize_line(char *dst, size_t dst_len, const char *src, size_t max_chars);
/* Remove the last code point of a NUL-terminated buffer in place, zeroing the removed bytes;
 * returns the bytes removed (lock screen backspace). */
size_t purp_utf8_pop_wipe(char *s);

/* =========================================================================================== */
/* Fonts (src/font.c)                                                                         */
/* =========================================================================================== */

/* Compiled-in DejaVu Sans Mono (build/embedded_font.c, generated by `make protocols`). */
extern const unsigned char purp_bundled_font[];
extern const size_t purp_bundled_font_len;

#define PURP_GLYPH_CACHE_LIMIT 1024

/* A rasterised glyph: 8-bit coverage, `width` x `height`, positioned at (pen_x + left,
 * baseline - top). */
typedef struct purp_glyph {
    int32_t width;
    int32_t height;
    int32_t left;
    int32_t top;
    int32_t advance;      /* px, rounded */
    uint8_t *coverage;    /* owned by the font cache; NULL for empty glyphs (spaces) */
} purp_glyph_t;

/*
 * Load the UI font: $PURPWM_FONT (non-empty), then config.h PURP_FONT_PATHS, then Fontconfig's
 * best "monospace" match, then purp_bundled_font. Logs "Loaded font: <path>" /
 * "Unusable font <path>: <err>" / "No system font usable; using the bundled DejaVu Sans Mono".
 * Never returns NULL unless allocation fails or the bundled font is corrupt.
 */
purp_font_t *purp_font_load(void);
/* Load from a file (NULL on failure). */
purp_font_t *purp_font_load_file(const char *path);
/* Load from memory (the bytes must outlive the font; purp_bundled_font qualifies). */
purp_font_t *purp_font_load_memory(const unsigned char *data, size_t len);
void purp_font_destroy(purp_font_t *font);
/* Path the font came from, or "<bundled>". */
const char *purp_font_source(const purp_font_t *font);
/* Glyph for `cp` at `px` pixels (cached by (cp, px); the cache is cleared past
 * PURP_GLYPH_CACHE_LIMIT). The pointer stays valid until the next purp_font_glyph call that
 * evicts. Missing glyphs fall back to '?'. Not thread-safe: compositor thread only. */
const purp_glyph_t *purp_font_glyph(purp_font_t *font, uint32_t cp, float px);
/* Ascent/descent for `px` (positive values, px). */
void purp_font_metrics(purp_font_t *font, float px, int32_t *ascent, int32_t *descent);
/* Advance width of UTF-8 `text` at `px`. A NULL font measures chars * max((int)(px*0.6), 8). */
int32_t purp_font_measure(purp_font_t *font, const char *text, float px);
/* Draw `text` with its baseline at `baseline_y`, starting at pen x `x`; glyphs are blended with
 * coverage. Returns the pen x after the text. A NULL font draws nothing and returns x. */
int32_t purp_font_draw(purp_font_t *font, purp_pixel_buf_t *buf, const char *text, int32_t x,
                       int32_t baseline_y, float px, purp_rgba_t rgba);
/* Like purp_font_draw but stops before the first glyph that would cross `x + max_w`. */
int32_t purp_font_draw_clipped(purp_font_t *font, purp_pixel_buf_t *buf, const char *text,
                               int32_t x, int32_t baseline_y, float px, int32_t max_w,
                               purp_rgba_t rgba);
/* Longest prefix of `text` (whole code points) whose width is <= max_w, copied into dst. */
size_t purp_font_clip_to_width(purp_font_t *font, const char *text, float px, int32_t max_w,
                               char *dst, size_t dst_len);

/* =========================================================================================== */
/* Status bar (src/bar.c)                                                                     */
/* =========================================================================================== */

/*
 * Layout per monitor (physical px, s = scale): height round(28 s); font 13.5 s; tag cells
 * round(34 s) wide for tags 1..9; layout-symbol pill; window title; right-aligned pills drawn
 * right-to-left: metrics ("[BAT [+]N%  |  ]CPU NN%  |  MEM N.NG  |  Mon DD HH:MM"), VOL, BT,
 * WIFI, "Flameshot". Hit-test order: tags, layout, wifi, bt, volume, metrics, flameshot, else
 * focus-next.
 */
#define PURP_BAR_TAG_CELL_WIDTH         34
#define PURP_BAR_LAYOUT_CELL_EXTRA      52
#define PURP_BAR_STATUS_INTERVAL_MS     2000   /* /proc, sysfs batteries, wpctl */
#define PURP_BAR_RADIO_EVERY_N_TICKS    3      /* nmcli / bluetoothctl every ~6 s */
#define PURP_BAR_IMMEDIATE_DEBOUNCE_MS  350
#define PURP_BAR_STATUS_CHECK_MS        500    /* tick_status throttle */
#define PURP_BAR_COMMAND_TIMEOUT_MS     3000   /* subprocess timeout; output pipes drained */
#define PURP_BAR_LABEL_MAX              96
#define PURP_BAR_METRICS_MAX            192

typedef enum purp_bar_action_kind {
    PURP_BAR_ACTION_TAG = 0,         /* `tag` holds 0..8 */
    PURP_BAR_ACTION_CYCLE_LAYOUT,
    PURP_BAR_ACTION_WIFI,
    PURP_BAR_ACTION_BLUETOOTH,
    PURP_BAR_ACTION_VOLUME,
    PURP_BAR_ACTION_FLAMESHOT,
    PURP_BAR_ACTION_SYSTEM_MONITOR,
    PURP_BAR_ACTION_FOCUS_NEXT,
} purp_bar_action_kind_t;

typedef struct purp_bar_action {
    purp_bar_action_kind_t kind;
    int tag;
} purp_bar_action_t;

/* Text and states shown in the right-hand pills (published by the poller thread). */
typedef struct purp_status_snapshot {
    char wifi_label[PURP_BAR_LABEL_MAX];      /* "WIFI: <ssid, 14 chars>", "WIFI: Off", ... */
    bool wifi_connected;
    char bt_label[PURP_BAR_LABEL_MAX];        /* "BT: <device, 12 chars>", "BT: On", "BT: Off" */
    bool bt_active;
    char volume_label[PURP_BAR_LABEL_MAX];    /* "VOL 45%", "VOL Mute 45%", "VOL N/A" */
    bool volume_unmuted;
    char sys_metrics[PURP_BAR_METRICS_MAX];
} purp_status_snapshot_t;

/* Pixel ranges [start, end) of the pills in the last raster (physical px). */
typedef struct purp_bar_range {
    int32_t start;
    int32_t end;
} purp_bar_range_t;

typedef struct purp_bar_slot {
    bool in_use;
    purp_pixel_buf_t buffer;
    int32_t width;                 /* physical px (>= 320) */
    int32_t height;
    double scale;
    float font_px;
    bool dirty;
    uint64_t last_status_gen;
    bool has_last_inputs;
    purp_bar_inputs_t last_inputs; /* last_inputs.focused_title points at last_title */
    char last_title[512];
    int32_t tag_cell_width;
    int32_t layout_cell_end;
    purp_bar_range_t wifi;
    purp_bar_range_t bt;
    purp_bar_range_t volume;
    purp_bar_range_t flameshot;
    purp_bar_range_t metrics;
} purp_bar_slot_t;

typedef struct purp_bar {
    purp_font_t *font;                         /* borrowed (server->font) */
    purp_bar_slot_t slots[PURP_MAX_MONITORS];
    uint64_t last_status_check_ms;
    purp_status_snapshot_t current;            /* compositor-thread copy */
    uint64_t status_gen;                       /* bumped when `current` changes */
    atomic_bool pending_redraw;                /* edge-triggered "redraw needed" */

    /* Background poller (bar.c). `shared` and the request fields are guarded by `lock`. */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool thread_started;
    bool stop;
    purp_status_snapshot_t shared;
    bool shared_changed;
    bool immediate_pending;                    /* single-flight immediate refresh */
    uint64_t immediate_due_ms;
} purp_bar_t;

/* Initialise with `font`, collect an initial snapshot synchronously (fast metrics only; radio
 * pills start as "WIFI: ...", "BT: ...", "VOL ...") and, when `start_poller`, start the status
 * thread (2 s cadence, radios every 3rd pass or on an immediate request). */
bool purp_bar_init(purp_bar_t *bar, purp_font_t *font, bool start_poller);
/* Stop and join the poller, free rasters. */
void purp_bar_finish(purp_bar_t *bar);
/* Track a monitor's scale (creating its slot, default width 1280 px); marks dirty on change. */
void purp_bar_set_monitor_scale(purp_bar_t *bar, size_t monitor, double scale);
/* Forget slots >= max(count, 1). */
void purp_bar_prune_monitors(purp_bar_t *bar, size_t count);
/* Edge-triggered: returns true once per mark_dirty (atomic exchange). Thread-safe. */
bool purp_bar_is_dirty(purp_bar_t *bar);
void purp_bar_mark_dirty(purp_bar_t *bar);
/* Ask the poller to refresh everything (incl. radios) in PURP_BAR_IMMEDIATE_DEBOUNCE_MS.
 * Single-flight: returns false while a request is already pending. */
bool purp_bar_request_immediate_refresh(purp_bar_t *bar);
/* Throttled (500 ms) copy of the poller's snapshot; true when the bar must be redrawn. */
bool purp_bar_tick_status(purp_bar_t *bar);
/* Hit-test logical x `local_x` on monitor `monitor` (falls back to slot 0, else FOCUS_NEXT). */
purp_bar_action_t purp_bar_hit_test(const purp_bar_t *bar, size_t monitor, int32_t local_x);
/* Raster for monitor `monitor` at `output_width_logical * scale` x round(28 * scale) physical px;
 * redrawn only when inputs, status, scale or width changed. NULL on allocation failure. */
const purp_pixel_buf_t *purp_bar_render(purp_bar_t *bar, size_t monitor,
                                        int32_t output_width_logical, double scale,
                                        const purp_bar_inputs_t *inputs);

/* Pure status parsers (unit-tested). */
/* `nmcli -t -f TYPE,STATE,CONNECTION device status` -> "WIFI: <ssid>" (connected, 14 chars),
 * "WIFI: ..." (connecting), "WIFI: Disconn", or "WIFI: N/A" when no wifi device. */
void purp_bar_wifi_label_from_nmcli(const char *text, char *label, size_t label_len,
                                    bool *connected);
/* `nmcli -t -f TYPE,STATE device status`: true when no wifi device is connected/connecting but
 * one is disconnected. */
bool purp_wifi_needs_autoconnect(const char *text);
/* `wpctl get-volume @DEFAULT_AUDIO_SINK@` ("Volume: 0.45 [MUTED]") -> "VOL 45%" /
 * "VOL Mute 45%" / "VOL N/A". */
void purp_bar_volume_label_from_wpctl(const char *text, char *label, size_t label_len,
                                      bool *unmuted);
/* Local time as "Mon DD HH:MM" (fallback "Jan 01 00:00"). */
void purp_format_local_time(char *out, size_t out_len);

/* =========================================================================================== */
/* PAM lock screen (src/lock.c)                                                               */
/* =========================================================================================== */

#define PURP_LOCK_PASSWORD_MAX      256    /* bytes */
#define PURP_LOCK_MAX_DOTS          32
#define PURP_LOCK_TIMEOUT_MS        15000
#define PURP_LOCK_CACHE_ENTRIES     8
#define PURP_LOCK_MESSAGE_MAX       256
#define PURP_LOCK_MSG_WRONG         "Wrong password \xE2\x80\x94 try again"
#define PURP_LOCK_MSG_FAILED        "Authentication failed \xE2\x80\x94 try again"
#define PURP_LOCK_MSG_TIMEOUT       "Authentication timed out \xE2\x80\x94 try again"
#define PURP_LOCK_MSG_ERROR         "Authentication error \xE2\x80\x94 try again"

typedef enum purp_lock_key_kind {
    PURP_LOCK_KEY_CHAR = 0,
    PURP_LOCK_KEY_BACKSPACE,
    PURP_LOCK_KEY_CLEAR,
    PURP_LOCK_KEY_SUBMIT,
} purp_lock_key_kind_t;

typedef struct purp_lock_key {
    purp_lock_key_kind_t kind;
    uint32_t codepoint;          /* PURP_LOCK_KEY_CHAR */
} purp_lock_key_t;

typedef enum purp_lock_poll {
    PURP_LOCK_POLL_PENDING = 0,  /* still locked (idle or checking) */
    PURP_LOCK_POLL_FAILED,       /* attempt rejected; message set, password cleared */
    PURP_LOCK_POLL_UNLOCKED,
} purp_lock_poll_t;

/* Shared between the compositor and one PAM worker thread (refcounted, see lock.c). */
typedef struct purp_lock_attempt purp_lock_attempt_t;

typedef struct purp_lock_cache_entry {
    bool in_use;
    int32_t width;
    int32_t height;
    double scale;
    size_t dots;
    bool checking;
    char message[PURP_LOCK_MESSAGE_MAX];
    char clock[32];
    purp_pixel_buf_t buffer;
} purp_lock_cache_entry_t;

typedef struct purp_lock_screen {
    char password[PURP_LOCK_PASSWORD_MAX + 1];   /* wiped with explicit_bzero */
    size_t password_bytes;
    char username[256];                          /* getpwuid_r, cached at init */
    char message[PURP_LOCK_MESSAGE_MAX];         /* "" when none */
    bool checking;
    purp_lock_attempt_t *attempt;                /* in-flight PAM attempt or NULL */
    uint64_t attempt_started_ms;
    char last_clock[32];
    purp_lock_cache_entry_t cache[PURP_LOCK_CACHE_ENTRIES];
} purp_lock_screen_t;

/* Number of PAM conversation prompts answered (tests observe the conversation ran). */
extern atomic_size_t purp_pam_conversations;

/* Keysym (xkbcommon) + UTF-32 + PURP_MOD_* -> lock key: Return/KP_Enter submit, BackSpace,
 * Escape or Ctrl+u clear, printable non-control characters type. False for everything else. */
bool purp_lock_key_from_keysym(uint32_t keysym, uint32_t utf32, uint32_t mods,
                               purp_lock_key_t *out);
void purp_lock_init(purp_lock_screen_t *lock);
/* Wipe the password, detach any worker (it frees its share), free rasters. */
void purp_lock_finish(purp_lock_screen_t *lock);
/* Typing clears the message; input is ignored while checking. Submit with a non-empty password
 * starts a detached worker thread running purp_pam_authenticate_current_user(). */
void purp_lock_handle_key(purp_lock_screen_t *lock, purp_lock_key_t key);
/* Collect the worker result: UNLOCKED, FAILED (PAM error text, PURP_LOCK_MSG_WRONG,
 * PURP_LOCK_MSG_FAILED on worker loss, PURP_LOCK_MSG_TIMEOUT after 15 s), or PENDING. */
purp_lock_poll_t purp_lock_poll(purp_lock_screen_t *lock);
bool purp_lock_is_checking(const purp_lock_screen_t *lock);
/* True when the minute changed since the last raster. */
bool purp_lock_needs_clock_redraw(purp_lock_screen_t *lock);
/* Typed code points. */
size_t purp_lock_password_len(const purp_lock_screen_t *lock);
/* Current message or NULL. */
const char *purp_lock_message(const purp_lock_screen_t *lock);
/* Full-screen raster (#11081F, big clock, date, password field with dots, status and
 * "Locked · <user>" lines), cached per (w, h, scale). */
const purp_pixel_buf_t *purp_lock_render(purp_lock_screen_t *lock, purp_font_t *font,
                                         int32_t width, int32_t height, double scale);
/* One synchronous PAM authentication of `user` against `service`; `confdir` non-NULL uses
 * pam_start_confdir(). The conversation answers ECHO_OFF/ECHO_ON with the password, captures
 * PAM_ERROR_MSG into err (may be NULL), ignores TEXT_INFO. */
bool purp_pam_check(const char *service, const char *user, const char *password,
                    const char *confdir, char *err, size_t err_len);
/* Resolve the service ($PURPWM_PAM_SERVICE, purpwm+, purpwm, vlock, login in
 * $PURPWM_PAM_CONFDIR or /etc/pam.d) and authenticate the current user. */
bool purp_pam_authenticate_current_user(const char *password, char *err, size_t err_len);

/* =========================================================================================== */
/* Desktop notifications (src/notify.c)                                                       */
/* =========================================================================================== */

/*
 * GDBus server on the session bus, own name org.freedesktop.Notifications (REPLACE |
 * ALLOW_REPLACEMENT) at /org/freedesktop/Notifications, run on its own thread + GMainContext:
 *   GetCapabilities() -> ["body"]
 *   Notify(s app, u replaces_id, s icon, s summary, s body, as actions, a{sv} hints,
 *          i expire_timeout) -> u id          (urgency from hints["urgency"], default 1)
 *   CloseNotification(u id)                   (id 0 -> org.freedesktop.Notifications.Error.
 *                                              InvalidId "Notification ID 0 is invalid")
 *   GetServerInformation() -> ("purpwm+", "purpwm+", PURPWM_VERSION, "1.2")
 *   signal NotificationClosed(u id, u reason): 1 expired, 2 dismissed by the user,
 *          3 closed by CloseNotification.
 * Ids: replaces_id > 0 is reused; otherwise the next id in 1..=0x7FFFFFFF not currently live.
 * Events cross to the compositor thread through a bounded queue (64, oldest dropped).
 */
#define PURP_NOTIFY_MAX_VISIBLE      4
#define PURP_NOTIFY_TOAST_WIDTH      340
#define PURP_NOTIFY_TOAST_HEIGHT     68
#define PURP_NOTIFY_TOAST_GAP        8
#define PURP_NOTIFY_DEFAULT_TIMEOUT_MS 5000
#define PURP_NOTIFY_CRITICAL_TIMEOUT_MS 12000
#define PURP_NOTIFY_MAX_TIMEOUT_MS   86400000ull
#define PURP_NOTIFY_QUEUE_CAPACITY   64
#define PURP_NOTIFY_APP_MAX_CHARS    128
#define PURP_NOTIFY_SUMMARY_MAX_CHARS 256
#define PURP_NOTIFY_BODY_MAX_CHARS   512
#define PURP_NOTIFY_CACHE_ENTRIES    8
#define PURP_NOTIFY_MAX_ID           0x7FFFFFFFu
#define PURP_NOTIFY_DBUS_NAME        "org.freedesktop.Notifications"
#define PURP_NOTIFY_DBUS_PATH        "/org/freedesktop/Notifications"

typedef enum purp_notify_close_reason {
    PURP_NOTIFY_CLOSED_EXPIRED = 1,
    PURP_NOTIFY_CLOSED_DISMISSED = 2,
    PURP_NOTIFY_CLOSED_BY_CALL = 3,
} purp_notify_close_reason_t;

typedef struct purp_notification {
    uint32_t id;
    char app_name[PURP_NOTIFY_APP_MAX_CHARS * 4 + 1];
    char summary[PURP_NOTIFY_SUMMARY_MAX_CHARS * 4 + 1];
    char body[PURP_NOTIFY_BODY_MAX_CHARS * 4 + 1];
    uint8_t urgency;             /* 0 low, 1 normal, 2 critical */
    uint64_t expires_at_ms;      /* CLOCK_MONOTONIC ms; 0 = never */
} purp_notification_t;

typedef enum purp_notify_event_kind {
    PURP_NOTIFY_EVENT_SHOW = 0,
    PURP_NOTIFY_EVENT_CLOSE,
} purp_notify_event_kind_t;

/* One D-Bus event for the compositor thread. */
typedef struct purp_notify_event {
    purp_notify_event_kind_t kind;
    purp_notification_t notification;   /* SHOW: id already assigned */
    int32_t expire_timeout;             /* SHOW: raw D-Bus value */
    uint32_t close_id;                  /* CLOSE */
} purp_notify_event_t;

/* D-Bus side (GDBus thread, GMainContext, owner id, live-id set); private to notify.c. */
typedef struct purp_notify_dbus purp_notify_dbus_t;

typedef struct purp_notify_cache_entry {
    bool in_use;
    double scale;
    int32_t width;
    int32_t height;
    uint64_t generation;
    purp_pixel_buf_t buffer;
} purp_notify_cache_entry_t;

typedef struct purp_notify_center {
    purp_notification_t toasts[PURP_NOTIFY_MAX_VISIBLE];   /* newest first */
    size_t toast_count;
    uint64_t content_generation;
    uint32_t next_local_id;                                /* ids for in-process pushes */

    pthread_mutex_t queue_lock;                            /* guards queue[] */
    purp_notify_event_t *queue;                            /* ring, PURP_NOTIFY_QUEUE_CAPACITY */
    size_t queue_head;
    size_t queue_len;

    purp_notify_dbus_t *dbus;                              /* NULL until started */
    purp_notify_cache_entry_t cache[PURP_NOTIFY_CACHE_ENTRIES];
} purp_notify_center_t;

bool purp_notify_init(purp_notify_center_t *center);
/* Stop the D-Bus thread (releasing the name) and free everything. */
void purp_notify_finish(purp_notify_center_t *center);
/* Own the bus name on a dedicated thread. If DBUS_SESSION_BUS_ADDRESS is unset and
 * <runtime_dir>/bus exists, connect to unix:path=<runtime_dir>/bus. */
bool purp_notify_start_dbus(purp_notify_center_t *center, const char *runtime_dir);
/* Expiry duration for a D-Bus expire_timeout: 0 -> never (returns 0); < 0 -> 12000 ms for
 * urgency >= 2 else 5000 ms; > 0 -> clamped 1..86400000 ms. */
uint64_t purp_notify_timeout_ms(int32_t expire_timeout, uint8_t urgency);
/*
 * In-process toast (also used for compositor messages): replaces `replaces_id` when live, else
 * inserts newest-first, keeping at most 4. Strings are sanitised and truncated. timeout_ms 0 means
 * the default (clamped 1000..30000 otherwise). Returns the id.
 */
uint32_t purp_notify_push(purp_notify_center_t *center, uint32_t replaces_id, const char *app,
                          const char *summary, const char *body, uint8_t urgency,
                          uint64_t timeout_ms);
/* Enqueue an event as the D-Bus thread does (thread-safe; drops the oldest beyond 64). */
void purp_notify_enqueue(purp_notify_center_t *center, const purp_notify_event_t *event);
/* Apply queued events and expire toasts (emitting NotificationClosed reason 1). True when the
 * visible stack changed. */
bool purp_notify_tick(purp_notify_center_t *center, uint64_t now_ms);
bool purp_notify_is_empty(const purp_notify_center_t *center);
size_t purp_notify_count(const purp_notify_center_t *center);
/* Stack rectangle in output-local logical coordinates: x = w - min(340, w) - 8,
 * y = (bar ? 28 : 0) + 8, height n*68 + (n-1)*8. False when empty. */
bool purp_notify_stack_geometry(const purp_notify_center_t *center, int32_t output_width,
                                bool bar_visible, purp_rect_t *out);
/* Dismiss the card under the global point `pt` on `output_rect` (reason 2). True when hit. */
bool purp_notify_dismiss_at(purp_notify_center_t *center, purp_pointf_t pt,
                            purp_rect_t output_rect, bool bar_visible);
/* Raster of the whole stack (physical px) for the given scale; NULL when empty. `*origin`
 * receives the output-local physical position. Cached by (scale, size, content generation). */
const purp_pixel_buf_t *purp_notify_render(purp_notify_center_t *center, purp_font_t *font,
                                           int32_t output_width_logical, bool bar_visible,
                                           double scale, purp_point_t *origin);

/* =========================================================================================== */
/* Mod+h help overlay (src/help.c)                                                            */
/* =========================================================================================== */

#define PURP_HELP_COLUMNS        3
#define PURP_HELP_CACHE_ENTRIES  8
#define PURP_HELP_TITLE          "purpwm+ keybindings"
#define PURP_HELP_HINT           "Mod = Super or Alt   \xC2\xB7   Mod + h / Esc / click to close"

typedef struct purp_help_row {
    const char *keys;
    const char *description;
} purp_help_row_t;

typedef struct purp_help_section {
    const char *title;
    const purp_help_row_t *rows;
    size_t row_count;
} purp_help_section_t;

/* Sections Session, Launch, Windows, Layouts, Tags, Look, Media & screenshots; keep in sync with
 * the bindings in src/input.c. */
extern const purp_help_section_t purp_help_sections[];
extern const size_t purp_help_section_count;

typedef struct purp_help_cache_entry {
    bool in_use;
    int32_t width;
    int32_t height;
    double scale;
    purp_point_t origin;
    purp_pixel_buf_t buffer;
} purp_help_cache_entry_t;

typedef struct purp_help_overlay {
    bool visible;
    purp_help_cache_entry_t cache[PURP_HELP_CACHE_ENTRIES];
} purp_help_overlay_t;

void purp_help_init(purp_help_overlay_t *help);
void purp_help_finish(purp_help_overlay_t *help);
void purp_help_toggle(purp_help_overlay_t *help);
/* Cached panel for an output of width x height physical px; NULL when hidden. `*origin`
 * receives the centred physical position. */
const purp_pixel_buf_t *purp_help_render(purp_help_overlay_t *help, purp_font_t *font,
                                         int32_t width, int32_t height, double scale,
                                         purp_point_t *origin);
/* Uncached raster: font starts at 15 * scale px and shrinks by 0.9 until the panel fits within
 * 95% x 92% of the output (or px < 8). `out` is (re)allocated; `origin` centres it. */
bool purp_help_rasterize(purp_font_t *font, int32_t width, int32_t height, double scale,
                         purp_pixel_buf_t *out, purp_point_t *origin);

/* =========================================================================================== */
/* Cursor theme (src/cursor.c)                                                                */
/* =========================================================================================== */

/* wp_cursor_shape_device_v1.shape values (v2 adds DND_ASK and ALL_RESIZE). */
typedef enum purp_cursor_shape {
    PURP_CURSOR_DEFAULT = 1,
    PURP_CURSOR_CONTEXT_MENU = 2,
    PURP_CURSOR_HELP = 3,
    PURP_CURSOR_POINTER = 4,
    PURP_CURSOR_PROGRESS = 5,
    PURP_CURSOR_WAIT = 6,
    PURP_CURSOR_CELL = 7,
    PURP_CURSOR_CROSSHAIR = 8,
    PURP_CURSOR_TEXT = 9,
    PURP_CURSOR_VERTICAL_TEXT = 10,
    PURP_CURSOR_ALIAS = 11,
    PURP_CURSOR_COPY = 12,
    PURP_CURSOR_MOVE = 13,
    PURP_CURSOR_NO_DROP = 14,
    PURP_CURSOR_NOT_ALLOWED = 15,
    PURP_CURSOR_GRAB = 16,
    PURP_CURSOR_GRABBING = 17,
    PURP_CURSOR_E_RESIZE = 18,
    PURP_CURSOR_N_RESIZE = 19,
    PURP_CURSOR_NE_RESIZE = 20,
    PURP_CURSOR_NW_RESIZE = 21,
    PURP_CURSOR_S_RESIZE = 22,
    PURP_CURSOR_SE_RESIZE = 23,
    PURP_CURSOR_SW_RESIZE = 24,
    PURP_CURSOR_W_RESIZE = 25,
    PURP_CURSOR_EW_RESIZE = 26,
    PURP_CURSOR_NS_RESIZE = 27,
    PURP_CURSOR_NESW_RESIZE = 28,
    PURP_CURSOR_NWSE_RESIZE = 29,
    PURP_CURSOR_COL_RESIZE = 30,
    PURP_CURSOR_ROW_RESIZE = 31,
    PURP_CURSOR_ALL_SCROLL = 32,
    PURP_CURSOR_ZOOM_IN = 33,
    PURP_CURSOR_ZOOM_OUT = 34,
    PURP_CURSOR_DND_ASK = 35,
    PURP_CURSOR_ALL_RESIZE = 36,
    PURP_CURSOR_SHAPE_MAX = 36
} purp_cursor_shape_t;

#define PURP_CURSOR_DEFAULT_THEME   "default"
#define PURP_CURSOR_DEFAULT_SIZE    24u
#define PURP_CURSOR_MIN_PX          8u
#define PURP_CURSOR_CACHE_ENTRIES   64
/* Built-in arrow: a 20x26 canvas at scale 1 whose first 23 rows are drawn from the purpwm
 * bitmap (W outline, L highlight, P fill, S shadow, '.' transparent; PURP_CURSOR_*_RGBA). */
#define PURP_CURSOR_ARROW_WIDTH     20
#define PURP_CURSOR_ARROW_HEIGHT    26

/* A cursor raster at its drawn (physical) size; draw at pointer - hotspot. */
typedef struct purp_cursor_image {
    purp_pixel_buf_t buffer;
    int32_t hotspot_x;
    int32_t hotspot_y;
} purp_cursor_image_t;

typedef struct purp_cursor_cache_entry {
    bool in_use;
    bool missing;                /* lookup failed; remembered to avoid disk scans */
    purp_cursor_shape_t shape;
    uint32_t target_px;
    purp_cursor_image_t image;
} purp_cursor_cache_entry_t;

typedef struct purp_cursor_theme {
    char theme[128];             /* $XCURSOR_THEME or "default" */
    uint32_t base_size;          /* $XCURSOR_SIZE (> 0) or 24 */
    purp_cursor_cache_entry_t cache[PURP_CURSOR_CACHE_ENTRIES];
    size_t cache_next;           /* round-robin eviction */
    purp_cursor_image_t arrow;   /* built-in fallback at arrow_px */
    uint32_t arrow_px;
} purp_cursor_theme_t;

/* CSS cursor name ("default", "context-menu", ..., "all-resize"); NULL if out of range. */
const char *purp_cursor_shape_name(purp_cursor_shape_t shape);
/* NULL-terminated legacy X cursor alias list for `shape` ("left_ptr", "hand2", ...). */
const char *const *purp_cursor_shape_alt_names(purp_cursor_shape_t shape);
/* Target size: max(round(base_size * scale), 8). */
uint32_t purp_cursor_target_px(uint32_t base_size, double scale);
void purp_cursor_theme_init(purp_cursor_theme_t *theme);
void purp_cursor_theme_finish(purp_cursor_theme_t *theme);
/*
 * Cursor for `shape` at `scale`: theme lookup by CSS name then aliases (XcursorLibraryLoadImage
 * picking the nearest size >= target, else the largest below, first frame), scaled to the target
 * with the hotspot scaled alike; missing shapes fall back to DEFAULT, then to the built-in purple
 * arrow. Never NULL.
 */
const purp_cursor_image_t *purp_cursor_theme_get(purp_cursor_theme_t *theme,
                                                 purp_cursor_shape_t shape, double scale);
/* Rasterise the built-in purple arrow at `scale` (nearest-neighbour), hotspot (0, 0). */
bool purp_cursor_builtin_arrow(purp_cursor_image_t *out, double scale);

/* =========================================================================================== */
/* Wallpaper (src/wallpaper.c)                                                                */
/* =========================================================================================== */

#define PURP_WALLPAPER_CACHE_ENTRIES 8

typedef struct purp_wallpaper_entry {
    bool in_use;
    int32_t width;
    int32_t height;
    purp_pixel_buf_t buffer;
} purp_wallpaper_entry_t;

typedef struct purp_wallpaper {
    bool loaded;
    void *source;                /* GdkPixbuf * (RGBA source image), private to wallpaper.c */
    int32_t source_width;
    int32_t source_height;
    purp_wallpaper_entry_t cache[PURP_WALLPAPER_CACHE_ENTRIES];   /* cleared when full */
} purp_wallpaper_t;

/* Load $PURPWM_WALLPAPER if set ("Unable to load wallpaper" warning on failure). */
void purp_wallpaper_init_from_env(purp_wallpaper_t *wp);
/* Load an image file (any gdk-pixbuf format); replaces the current one. */
bool purp_wallpaper_load(purp_wallpaper_t *wp, const char *path);
void purp_wallpaper_finish(purp_wallpaper_t *wp);
/* Centred source window with the destination aspect ratio (cover crop). */
purp_rect_t purp_wallpaper_cover_crop(int32_t src_w, int32_t src_h, int32_t dst_w, int32_t dst_h);
/* Opaque raster of width x height physical px (bilinear scale of the cover crop, composited
 * over #11081F, premultiplied), cached per (w, h). NULL when no wallpaper is loaded. */
const purp_pixel_buf_t *purp_wallpaper_get(purp_wallpaper_t *wp, int32_t width, int32_t height);

#endif /* PURPWM_UI_H */
