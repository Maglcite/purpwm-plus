/*
 * purpwm+ — compile-time configuration (in the spirit of dwm's config.h).
 *
 * Edit this file and rebuild (`make`) to customise the translucent purple look, fonts, gaps,
 * tags, window rules and the commands bound to keys and status-bar pills.
 *
 * Pure header: no libwayland, no EGL. Every table below is `static const` with PURP_UNUSED so
 * translation units that do not use a table compile warning-free under -Wall -Wextra.
 */
#ifndef PURPWM_CONFIG_H
#define PURPWM_CONFIG_H

#include "types.h"

/* =========================================================================================== */
/* Identity, paths and environment                                                            */
/* =========================================================================================== */

#ifndef PURPWM_VERSION
#define PURPWM_VERSION "0.1.0"
#endif

#define PURP_NAME                    "purpwm+"
/* XDG_CURRENT_DESKTOP exported to children (portal and session lookups key on it). */
#define PURP_XDG_CURRENT_DESKTOP     "purpwm"
/* Wayland socket search range: wayland-1 .. wayland-32 (`ListeningSocket::bind_auto`). */
#define PURP_SOCKET_FIRST            1
#define PURP_SOCKET_LAST             32
#define PURP_SEAT_NAME               "seat-0"

/* Config directory under $XDG_CONFIG_HOME (or $HOME/.config). The legacy purpwm directory is
 * read as a fallback for monitors.json and the saved scale; writes always go to purpwm+. */
#define PURP_CONFIG_DIR_NAME         "purpwm+"
#define PURP_LEGACY_CONFIG_DIR_NAME  "purpwm"
#define PURP_MONITORS_FILE_NAME      "monitors.json"
#define PURP_SCALE_FILE_NAME         "scale"

/* Runtime sockets, all under $XDG_RUNTIME_DIR (fallback /run/user/<uid>).
 * IPC:      purpwm+-ipc-<WAYLAND_DISPLAY>.sock, symlinked as purpwm+-ipc.sock and purpwm-ipc.sock
 *           (the latter keeps Spotlight's Display Settings working unchanged).
 * Capture:  purpwm+-capture-<WAYLAND_DISPLAY>.sock (portal screenshot transport).
 * Spotlight daemon fast path: spotlight.sock (payload `"Toggle"\n`, JSON string incl. quotes). */
#define PURP_IPC_SOCKET_PREFIX       "purpwm+-ipc-"
#define PURP_IPC_LINK_NAME           "purpwm+-ipc.sock"
#define PURP_IPC_LEGACY_LINK_NAME    "purpwm-ipc.sock"
#define PURP_CAPTURE_SOCKET_PREFIX   "purpwm+-capture-"
#define PURP_SPOTLIGHT_SOCKET_NAME   "spotlight.sock"
#define PURP_SPOTLIGHT_TOGGLE_MSG    "\"Toggle\"\n"
#define PURP_SPOTLIGHT_KEEPALIVE_NS  "Spotlight-keepalive"

/* Environment variables read or exported by purpwm+. */
#define PURP_ENV_SCALE               "PURPWM_SCALE"            /* highest-priority scale */
#define PURP_ENV_DEFAULT_SCALE       "PURPWM_DEFAULT_SCALE"    /* used when no scale file */
#define PURP_ENV_TERMINAL            "PURPWM_TERMINAL"         /* program | "auto" | "none" */
#define PURP_ENV_SPOTLIGHT           "PURPWM_SPOTLIGHT"        /* program | "auto" | "none" */
#define PURP_ENV_AUTOSTART           "PURPWM_AUTOSTART"        /* "0" disables autostart */
#define PURP_ENV_STANDALONE          "PURPWM_STANDALONE"       /* set => --standalone */
#define PURP_ENV_VIRTUAL_MONITORS    "PURPWM_VIRTUAL_MONITORS" /* name:WxH[@HZ][:SCALE],... */
#define PURP_ENV_MONITORS_CONFIG     "PURPWM_MONITORS_CONFIG"  /* explicit monitors.json path */
#define PURP_ENV_IPC_SOCKET          "PURPWM_IPC_SOCKET"
#define PURP_ENV_UPDATE_IPC_LINK     "PURPWM_UPDATE_IPC_LINK"  /* "1"/"true" force link refresh */
#define PURP_ENV_CAPTURE_SOCKET      "PURPWM_CAPTURE_SOCKET"
#define PURP_ENV_PAM_CONFDIR         "PURPWM_PAM_CONFDIR"      /* => pam_start_confdir() */
#define PURP_ENV_PAM_SERVICE         "PURPWM_PAM_SERVICE"
#define PURP_ENV_WALLPAPER           "PURPWM_WALLPAPER"
#define PURP_ENV_FONT                "PURPWM_FONT"
#define PURP_ENV_NOTIFY_DBUS         "PURPWM_NOTIFY_DBUS"      /* start notifications w/o autostart */
#define PURP_ENV_RESOLUTION          "PURPWM_RESOLUTION"       /* DRM initial "WxH" */
#define PURP_ENV_REFRESH_RATE        "PURPWM_REFRESH_RATE"     /* DRM initial Hz */
#define PURP_ENV_DRM_DEVICE          "PURPWM_DRM_DEVICE"       /* explicit /dev/dri/cardN */
#define PURP_ENV_DEBUG               "PURPWM_DEBUG"            /* enables PURP_LOG_DEBUG output */

/* PAM: service lookup order is $PURPWM_PAM_SERVICE, then "purpwm+", "purpwm", "vlock", "login"
 * (the first whose file exists in the conf dir, default /etc/pam.d). */
#define PURP_PAM_DEFAULT_CONFDIR     "/etc/pam.d"
#define PURP_PAM_SERVICE_PRIMARY     "purpwm+"
#define PURP_PAM_SERVICE_LEGACY      "purpwm"
#define PURP_PAM_SERVICE_VLOCK       "vlock"
#define PURP_PAM_SERVICE_LOGIN       "login"

/* Screenshots: $HOME/<dir>/purpwm+-YYYYmmdd-HHMMSS[-N].png (mode 0600, written atomically via
 * .purpwm+-<stamp>-<pid>-<n>.png.tmp, up to 64 attempts) and offered on the clipboard. */
#define PURP_SCREENSHOT_DIR          "Pictures/Screenshots"
#define PURP_SCREENSHOT_PREFIX       "purpwm+-"
#define PURP_SCREENSHOT_MIME         "image/png"

/* App id used by the companion polkit agent (matched by a window rule below). */
#define PURP_POLKIT_AGENT_APP_ID     "org.purpwm.PolkitAuthenticationAgent"

/* =========================================================================================== */
/* Scale, geometry and motion                                                                 */
/* =========================================================================================== */

/* Default output scale (HiDPI). Change live with Mod+Ctrl+= / Mod+Ctrl+- (remembered in
 * ~/.config/purpwm+/scale), or override one launch with PURPWM_SCALE=1.75. */
#define PURP_DEFAULT_SCALE           1.5
#define PURP_SCALE_STEP              0.25
#define PURP_SCALE_MIN               1.0
#define PURP_SCALE_MAX               3.0
/* apply_monitors rounds scales to this step before clamping to [SCALE_MIN, SCALE_MAX]. */
#define PURP_MONITOR_SCALE_ROUNDING  0.05

/* Built-in top status bar height (logical px, multiplied by the monitor scale). */
#define PURP_BAR_HEIGHT              28
/* Window border width (logical px, dwm `borderpx`). */
#define PURP_BORDER_WIDTH            2
/* Inner / outer gaps (logical px). Runtime clamps: inner 0..48, outer 0..64. */
#define PURP_GAP_INNER               6
#define PURP_GAP_OUTER               8
#define PURP_GAP_INNER_MAX           48
#define PURP_GAP_OUTER_MAX           64
/* Master area factor (clamped 0.10..0.90 at runtime) and master count. */
#define PURP_DEFAULT_MFACT           0.55f
#define PURP_MFACT_MIN               0.10f
#define PURP_MFACT_MAX               0.90f
#define PURP_MFACT_STEP              0.05f
#define PURP_DEFAULT_NMASTER         1
/* Smallest height a tiled row resize leaves a window; smallest weight per tiled cfact. */
#define PURP_MIN_TILED_ROW           80
#define PURP_MIN_CFACT               0.05f
/* Mod+Arrow floating move step (logical px). */
#define PURP_MOVE_STEP               40
/* Floating cascade grid step, cross-monitor recoverable strip, adopt-client-size margin. */
#define PURP_CASCADE_STEP            32
#define PURP_VISIBLE_STRIP           32
/* Interactive (pointer) resize minimum frame size (logical px). */
#define PURP_MIN_DRAG_WIDTH          120
#define PURP_MIN_DRAG_HEIGHT         80
/* Self-sized floating dialogs: smallest adopted size. */
#define PURP_MIN_CLIENT_SIZE         64
/* Gaps adjusted per Mod+= / Mod+- press. */
#define PURP_GAP_STEP                2

/* Mod+Shift+Arrow glides the edge (or the tiling split beside it): starts at SPEED_MIN and eases
 * to SPEED_MAX (logical px/s) over RAMP_SECS; a quick tap still glides RESIZE_STEP pixels.
 * Frames longer than GLIDE_MAX_FRAME_SECS are credited as that long. */
#define PURP_RESIZE_STEP             16.0
#define PURP_RESIZE_SPEED_MIN        240.0
#define PURP_RESIZE_SPEED_MAX        1400.0
#define PURP_RESIZE_RAMP_SECS        0.8
#define PURP_GLIDE_MAX_FRAME_SECS    0.05

/* Bar scroll: accumulated continuous axis units per tag/volume step; v120 => 15 units/notch. */
#define PURP_BAR_SCROLL_STEP         15.0

/* Keyboard repeat advertised to clients (wl_keyboard.repeat_info). */
#define PURP_KEY_REPEAT_DELAY_MS     200
#define PURP_KEY_REPEAT_RATE         25

/* =========================================================================================== */
/* Timings                                                                                    */
/* =========================================================================================== */

#define PURP_IDLE_SLEEP_MS               8      /* nothing to draw: sleep this long */
#define PURP_TICK_INTERVAL_MS            8      /* DRM/headless maintenance tick */
#define PURP_DRM_DISPATCH_TIMEOUT_MS     16
#define PURP_MALLOC_TRIM_INTERVAL_MS     30000  /* malloc_trim(0) on a helper thread */
#define PURP_ACTIVATION_TOKEN_TTL_MS     10000  /* xdg_activation_v1 token lifetime */
#define PURP_WIFI_CONNECT_DELAY_MS       800    /* `nmcli radio wifi on` -> device connect */

/* =========================================================================================== */
/* Translucent purple theme palette                                                           */
/* =========================================================================================== */

/* Deep obsidian-purple compositor background (#11081F @ 86%). */
#define PURP_COLOR_BG_ROOT           ((purp_color_t){ 0.067f, 0.031f, 0.122f, 0.86f })
/* Active border: vibrant electric purple (#A855F7 @ 96%). */
#define PURP_COLOR_BORDER_ACTIVE     ((purp_color_t){ 0.659f, 0.333f, 0.969f, 0.96f })
/* 1px outer glow around the active border (#C084FC @ 42%). */
#define PURP_COLOR_BORDER_GLOW       ((purp_color_t){ 0.753f, 0.518f, 0.988f, 0.42f })
/* Inactive border: muted dark plum (#3B1F5E @ 72%). */
#define PURP_COLOR_BORDER_INACTIVE   ((purp_color_t){ 0.231f, 0.122f, 0.369f, 0.72f })
/* Opaque black behind fullscreen windows. */
#define PURP_COLOR_FULLSCREEN_BACKDROP ((purp_color_t){ 0.0f, 0.0f, 0.0f, 1.0f })
/* Nested host window clear colour around virtual monitor tiles. */
#define PURP_COLOR_HOST_CLEAR        ((purp_color_t){ 0.05f, 0.03f, 0.09f, 1.0f })

/* Opacity of the focused / unfocused windows (fullscreen windows are always opaque). */
#define PURP_OPACITY_ACTIVE          0.94f
#define PURP_OPACITY_INACTIVE        0.84f
/* Alpha used for popups and above-window layer surfaces, and for the DnD icon. */
#define PURP_OPACITY_POPUP           0.96f
#define PURP_OPACITY_DND_ICON        0.92f

/* Status bar palette (straight RGBA 0..255; rasterisers premultiply). */
#define PURP_BAR_BG_RGBA             ((purp_rgba_t){ 0x17, 0x0B, 0x29, 210 })
#define PURP_BAR_TAG_ACTIVE_RGBA     ((purp_rgba_t){ 0x7C, 0x3A, 0xED, 235 })
#define PURP_BAR_LAYOUT_BG_RGBA      ((purp_rgba_t){ 0x2A, 0x14, 0x47, 224 })
#define PURP_BAR_ACCENT_LINE_RGBA    ((purp_rgba_t){ 0x93, 0x33, 0xEA, 230 })
#define PURP_BAR_ACCENT_RGBA         ((purp_rgba_t){ 0xC0, 0x84, 0xFC, 255 })
#define PURP_BAR_TEXT_FG_RGBA        ((purp_rgba_t){ 0xF3, 0xE8, 0xFF, 255 })
#define PURP_BAR_TEXT_DIM_RGBA       ((purp_rgba_t){ 0x8B, 0x7A, 0xA8, 255 })
#define PURP_BAR_URGENT_RGBA         ((purp_rgba_t){ 0xEC, 0x48, 0x99, 240 })
#define PURP_BAR_BT_ACTIVE_RGBA      ((purp_rgba_t){ 0x4C, 0x1D, 0x95, 225 })
/* Lock screen, notification and help overlay palette. */
#define PURP_LOCK_BG_RGBA            ((purp_rgba_t){ 0x11, 0x08, 0x1F, 255 })
#define PURP_LOCK_ERROR_RGBA         ((purp_rgba_t){ 0xF4, 0x3F, 0x5E, 255 })
#define PURP_TOAST_CRITICAL_RGBA     ((purp_rgba_t){ 0xF4, 0x3F, 0x5E, 245 })
#define PURP_HELP_PANEL_BG_RGBA      ((purp_rgba_t){ 0x14, 0x09, 0x24, 242 })
#define PURP_HELP_SECTION_RGBA       ((purp_rgba_t){ 0xE9, 0xD5, 0xFF, 255 })
/* Wallpaper is composited over this opaque colour (#11081F). */
#define PURP_WALLPAPER_BG_RGBA       ((purp_rgba_t){ 0x11, 0x08, 0x1F, 255 })
/* Built-in purple arrow cursor palette: outline, highlight, interior, shadow. */
#define PURP_CURSOR_OUTLINE_RGBA     ((purp_rgba_t){ 0xFA, 0xF5, 0xFF, 255 })
#define PURP_CURSOR_HIGHLIGHT_RGBA   ((purp_rgba_t){ 0xD8, 0xB4, 0xFE, 255 })
#define PURP_CURSOR_FILL_RGBA        ((purp_rgba_t){ 0x93, 0x33, 0xEA, 255 })
#define PURP_CURSOR_SHADOW_RGBA      ((purp_rgba_t){ 0x0D, 0x05, 0x18, 190 })

/* =========================================================================================== */
/* Tags and fonts                                                                             */
/* =========================================================================================== */

/* Workspace tag labels (dwm 1..9 bitmask tags; PURP_TAG_COUNT / PURP_TAGMASK live in types.h). */
static const char *const PURP_TAG_NAMES[PURP_TAG_COUNT] PURP_UNUSED = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9",
};

/* Status bar font size in logical px (multiplied by the monitor scale). */
#define PURP_BAR_FONT_SIZE           13.5f

/* Candidate monospace fonts, tried in order after $PURPWM_FONT and before Fontconfig's
 * "monospace" match and the compiled-in DejaVu Sans Mono (purp_bundled_font). */
static const char *const PURP_FONT_PATHS[] PURP_UNUSED = {
    "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf",
    "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/FiraCode-Regular.ttf",
    "/usr/share/fonts/TTF/Hack-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/gnu-free/FreeMono.ttf",
    "/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
};

/* =========================================================================================== */
/* dwm-style window rules                                                                     */
/* =========================================================================================== */

/* Matched when a window maps or updates its app_id/title (see purp_window_rule_matches()).
 * A NULL matcher is a wildcard; a rule with both matchers NULL never matches; every non-NULL
 * matcher must be a substring of its (non-empty) field. Matching stops once a rule hits or once
 * both app_id and title are known (purp_window_rules_settled()). */
typedef struct purp_window_rule {
    const char *app_id_match;   /* substring of the xdg app_id, or NULL for any */
    const char *title_match;    /* substring of the title, or NULL for any */
    uint16_t tags;              /* tag mask to assign; 0 keeps the current tags */
    bool is_floating;           /* float (centred, client-sized) */
    bool has_opacity;           /* whether `opacity` overrides the focus-dependent opacity */
    float opacity;              /* 1.0 forces fully opaque (video players, games, pinentry) */
} purp_window_rule_t;

#define PURP_RULE(app, title, tagmask, floating, opac)                                       \
    { (app), (title), (tagmask), (floating), true, (opac) }
#define PURP_RULE_NO_OPACITY(app, title, tagmask, floating)                                  \
    { (app), (title), (tagmask), (floating), false, 1.0f }

static const purp_window_rule_t PURP_WINDOW_RULES[] PURP_UNUSED = {
    /* Spotlight launcher and purpwm control popups always float centred. */
    PURP_RULE("spotlight", NULL, 0, true, 0.96f),
    PURP_RULE("purpwm-control", NULL, 0, true, 0.96f),
    PURP_RULE(PURP_POLKIT_AGENT_APP_ID, NULL, 0, true, 0.98f),
    PURP_RULE(NULL, "Authentication Required", 0, true, 0.98f),
    PURP_RULE(NULL, "purpwm Wi-Fi Manager", 0, true, 0.96f),
    PURP_RULE(NULL, "purpwm Bluetooth Manager", 0, true, 0.96f),
    PURP_RULE(NULL, "purpwm System Monitor", 0, true, 0.96f),
    /* Flameshot's selection/annotation UI needs the captured pixels fully opaque. */
    PURP_RULE(NULL, "Screenshot \xE2\x80\x94 purpwm", 0, true, 1.0f),
    PURP_RULE("flameshot", NULL, 0, true, 1.0f),
    /* Common utility / settings dialogs float automatically. */
    PURP_RULE_NO_OPACITY("pavucontrol", NULL, 0, true),
    PURP_RULE_NO_OPACITY("nm-connection-editor", NULL, 0, true),
    PURP_RULE("com.system76.CosmicSettings", NULL, 0, true, 0.98f),
    PURP_RULE_NO_OPACITY("blueman-manager", NULL, 0, true),
    PURP_RULE_NO_OPACITY("xdg-desktop-portal", NULL, 0, true),
    PURP_RULE("pinentry", NULL, 0, true, 1.0f),
    PURP_RULE_NO_OPACITY(NULL, "Open File", 0, true),
    PURP_RULE_NO_OPACITY(NULL, "Save File", 0, true),
    /* Media and video players render fully opaque. */
    PURP_RULE("mpv", NULL, 0, false, 1.0f),
    PURP_RULE("imv", NULL, 0, true, 1.0f),
    PURP_RULE("vlc", NULL, 0, false, 1.0f),
    /* Games are always fully opaque (Sober = Roblox). */
    PURP_RULE("org.vinegarhq.Sober", NULL, 0, false, 1.0f),
    PURP_RULE("steam_app_", NULL, 0, false, 1.0f),
};
#define PURP_WINDOW_RULE_COUNT PURP_ARRAY_LEN(PURP_WINDOW_RULES)

/* =========================================================================================== */
/* Commands (launcher, terminal, system monitor, radios, audio, media, session helpers)        */
/* =========================================================================================== */

/* One candidate command: NULL-terminated argv (argv[0] looked up in $PATH unless it contains a
 * '/'). Lists are tried in order and the first candidate whose argv[0] exists is spawned
 * (spawn_first_available). Entries whose argv[0] is NULL are skipped (empty lists). */
#define PURP_COMMAND_MAX_ARGV 10
typedef struct purp_command {
    const char *argv[PURP_COMMAND_MAX_ARGV];
} purp_command_t;

typedef struct purp_command_list {
    const purp_command_t *items;
    size_t count;
} purp_command_list_t;

#define PURP_COMMANDS(array) ((purp_command_list_t){ (array), PURP_ARRAY_LEN(array) })

/* App launcher, tried when Mod is tapped alone or Mod+p and the spotlight.sock fast path fails.
 * Override the executable with PURPWM_SPOTLIGHT ("none" disables). */
static const purp_command_t PURP_LAUNCHER_COMMANDS[] PURP_UNUSED = {
    { { "spotlight", "--toggle", NULL } },
};

/* Terminals for Mod+Return and the Mod+s scratchpad (override with PURPWM_TERMINAL). */
static const purp_command_t PURP_TERMINAL_COMMANDS[] PURP_UNUSED = {
    { { "alacritty", NULL } },
    { { "foot", NULL } },
    { { "kitty", NULL } },
    { { "wezterm", NULL } },
    { { "cosmic-term", NULL } },
    { { "gnome-terminal", NULL } },
    { { "weston-terminal", NULL } },
    { { "xterm", NULL } },
};

/* System monitor: Mod+Escape or a click on the CPU/MEM/BAT metrics. */
static const purp_command_t PURP_SYSTEM_MONITOR_COMMANDS[] PURP_UNUSED = {
    { { "alacritty", "--class", "purpwm-control", "-t", "purpwm System Monitor", "-e", "btop",
        NULL } },
    { { "alacritty", "--class", "purpwm-control", "-t", "purpwm System Monitor", "-e", "top",
        NULL } },
};

/* Wi-Fi manager: Mod+w or a click on the WIFI pill. */
static const purp_command_t PURP_WIFI_COMMANDS[] PURP_UNUSED = {
    { { "purpwm+-wifi", NULL } },
    { { "alacritty", "--class", "purpwm-control", "-t", "purpwm Wi-Fi Manager", "-e", "nmtui",
        NULL } },
    { { "nm-connection-editor", NULL } },
    { { "cosmic-settings", "wireless", NULL } },
};

/* Bluetooth manager: Mod+Shift+b or a click on the BT pill. */
static const purp_command_t PURP_BLUETOOTH_COMMANDS[] PURP_UNUSED = {
    { { "purpwm+-bt", NULL } },
    { { "cosmic-settings", "bluetooth", NULL } },
};

/* XF86AudioRaiseVolume / LowerVolume / Mute / MicMute. */
static const purp_command_t PURP_VOL_UP_COMMANDS[] PURP_UNUSED = {
    { { "wpctl", "set-volume", "-l", "1.5", "@DEFAULT_AUDIO_SINK@", "5%+", NULL } },
    { { "pactl", "set-sink-volume", "@DEFAULT_SINK@", "+5%", NULL } },
};
static const purp_command_t PURP_VOL_DOWN_COMMANDS[] PURP_UNUSED = {
    { { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", NULL } },
    { { "pactl", "set-sink-volume", "@DEFAULT_SINK@", "-5%", NULL } },
};
static const purp_command_t PURP_VOL_MUTE_COMMANDS[] PURP_UNUSED = {
    { { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL } },
    { { "pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", NULL } },
};
static const purp_command_t PURP_MIC_MUTE_COMMANDS[] PURP_UNUSED = {
    { { "wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle", NULL } },
    { { "pactl", "set-source-mute", "@DEFAULT_SOURCE@", "toggle", NULL } },
};

/* XF86MonBrightnessUp / Down. Without brightnessctl, purpwm+ falls back to logind:
 * `busctl call org.freedesktop.login1 /org/freedesktop/login1/session/auto
 *  org.freedesktop.login1.Session SetBrightness ssu backlight <name> <value>`
 * stepping PURP_BRIGHTNESS_STEP_PERCENT of max_brightness, clamped to [max/100, max]. */
static const purp_command_t PURP_BRIGHTNESS_UP_COMMANDS[] PURP_UNUSED = {
    { { "brightnessctl", "set", "+5%", NULL } },
};
static const purp_command_t PURP_BRIGHTNESS_DOWN_COMMANDS[] PURP_UNUSED = {
    { { "brightnessctl", "set", "5%-", NULL } },
};
#define PURP_BRIGHTNESS_STEP_PERCENT 5
#define PURP_BACKLIGHT_SYSFS_DIR     "/sys/class/backlight"

/* XF86AudioPlay|Pause / Next / Prev. */
static const purp_command_t PURP_MEDIA_PLAY_COMMANDS[] PURP_UNUSED = {
    { { "playerctl", "play-pause", NULL } },
};
static const purp_command_t PURP_MEDIA_NEXT_COMMANDS[] PURP_UNUSED = {
    { { "playerctl", "next", NULL } },
};
static const purp_command_t PURP_MEDIA_PREV_COMMANDS[] PURP_UNUSED = {
    { { "playerctl", "previous", NULL } },
};

/* Flameshot capture UI, opened by its status-bar pill. */
static const purp_command_t PURP_FLAMESHOT_COMMANDS[] PURP_UNUSED = {
    { { "flameshot", "gui", NULL } },
};

/* Audio mixer: right-click on the VOL pill. */
static const purp_command_t PURP_MIXER_COMMANDS[] PURP_UNUSED = {
    { { "pavucontrol", NULL } },
    { { "cosmic-settings", "sound", NULL } },
};

/* Session helpers started by autostart. The portal and polkit agent are tracked by process
 * group and receive SIGTERM on shutdown (shutdown_session). */
static const purp_command_t PURP_SCREENSHOT_PORTAL_COMMANDS[] PURP_UNUSED = {
    { { "purpwm+-portal", "--session", NULL } },
};
static const purp_command_t PURP_KEYRING_COMMANDS[] PURP_UNUSED = {
    { { "purpwm+-keyring", "start", NULL } },
};
static const purp_command_t PURP_POLKIT_COMMANDS[] PURP_UNUSED = {
    { { "purpwm+-polkit-agent", NULL } },
};

/* Optional background commands started once at login. Notification daemons are intentionally
 * absent: purpwm+ owns org.freedesktop.Notifications itself, and mako/dunst would take that bus
 * name away from the built-in toasts. Add entries as { { "prog", "arg", NULL } }. */
static const purp_command_t PURP_AUTOSTART_COMMANDS[] PURP_UNUSED = {
    { { NULL } },
};

/* Network/Bluetooth bring-up at login (background thread, ensure_network_and_bluetooth):
 *   nmcli networking on; nmcli radio wifi on;
 *   if purp_wifi_needs_autoconnect(`nmcli -t -f TYPE,STATE device status`):
 *       nmcli device connect wlan0
 *   rfkill unblock bluetooth; bluetoothctl power on
 * The WIFI pill's right-click toggle runs `nmcli radio wifi` and then `nmcli radio wifi off`, or
 * `nmcli radio wifi on`, sleep PURP_WIFI_CONNECT_DELAY_MS, `nmcli device connect wlan0`.
 * The BT pill's right-click toggle reads `bluetoothctl show` ("Powered: yes") and runs
 * `bluetoothctl power off`, or `rfkill unblock bluetooth` + `bluetoothctl power on`. */
#define PURP_WIFI_AUTOCONNECT_DEVICE "wlan0"

#endif /* PURPWM_CONFIG_H */
