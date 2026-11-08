/* Keyboard bindings, compositor grabs and Wayland seat input routing. */
#include "purpwm.h"
#include "render.h"

#include <dirent.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static int32_t input_clamp_i32(int64_t value) {
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

static bool remove_code(uint32_t *codes, size_t *count, uint32_t code) {
    for (size_t i = 0; i < *count; ++i) {
        if (codes[i] == code) {
            codes[i] = codes[--*count];
            return true;
        }
    }
    return false;
}

static void add_code(uint32_t *codes, size_t *count, size_t cap, uint32_t code) {
    for (size_t i = 0; i < *count; ++i)
        if (codes[i] == code) return;
    if (*count < cap) codes[(*count)++] = code;
}

static bool touch_swallowed(const purp_server_t *s, int32_t slot) {
    for (size_t i = 0; i < s->swallowed_touch_count; ++i)
        if (s->swallowed_touch_slots[i] == slot) return true;
    return false;
}

static void swallow_touch(purp_server_t *s, int32_t slot) {
    if (!touch_swallowed(s, slot) && s->swallowed_touch_count < PURP_MAX_TOUCH_POINTS)
        s->swallowed_touch_slots[s->swallowed_touch_count++] = slot;
}

static bool remove_touch(purp_server_t *s, int32_t slot) {
    for (size_t i = 0; i < s->swallowed_touch_count; ++i) {
        if (s->swallowed_touch_slots[i] == slot) {
            s->swallowed_touch_slots[i] = s->swallowed_touch_slots[--s->swallowed_touch_count];
            return true;
        }
    }
    return false;
}

static void swallow_button(purp_server_t *s, uint32_t button) {
    add_code(s->swallowed_buttons, &s->swallowed_button_count,
             PURP_MAX_PRESSED_BUTTONS, button);
}

static bool is_mod_sym(uint32_t sym) {
    return sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R ||
           sym == XKB_KEY_Meta_L || sym == XKB_KEY_Meta_R ||
           sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R;
}

bool purp_input_lookup_binding(uint32_t sym, uint32_t raw, uint32_t mods,
                               bool help_visible, purp_action_t *out) {
    purp_action_t a = {0};
    bool shift = (mods & PURP_MOD_SHIFT) != 0;
    bool ctrl = (mods & PURP_MOD_CTRL) != 0;
    bool mod = purp_mods_has_mod(mods);
    if (help_visible && sym == XKB_KEY_Escape && !mod) {
        a.kind = PURP_ACTION_TOGGLE_HELP;
        goto found;
    }
    switch (sym) {
    case XKB_KEY_XF86AudioRaiseVolume: a.kind = PURP_ACTION_VOLUME_UP; break;
    case XKB_KEY_XF86AudioLowerVolume: a.kind = PURP_ACTION_VOLUME_DOWN; break;
    case XKB_KEY_XF86AudioMute: a.kind = PURP_ACTION_VOLUME_MUTE; break;
    case XKB_KEY_XF86AudioMicMute: a.kind = PURP_ACTION_MIC_MUTE; break;
    case XKB_KEY_XF86MonBrightnessUp: a.kind = PURP_ACTION_BRIGHTNESS_UP; break;
    case XKB_KEY_XF86MonBrightnessDown: a.kind = PURP_ACTION_BRIGHTNESS_DOWN; break;
    case XKB_KEY_XF86AudioPlay:
    case XKB_KEY_XF86AudioPause: a.kind = PURP_ACTION_MEDIA_PLAY; break;
    case XKB_KEY_XF86AudioNext: a.kind = PURP_ACTION_MEDIA_NEXT; break;
    case XKB_KEY_XF86AudioPrev: a.kind = PURP_ACTION_MEDIA_PREV; break;
    case XKB_KEY_XF86ScreenSaver: a.kind = PURP_ACTION_LOCK_SCREEN; break;
    case XKB_KEY_Print:
        a.kind = shift || mod ? PURP_ACTION_SCREENSHOT_WINDOW : PURP_ACTION_SCREENSHOT;
        break;
    default: break;
    }
    if (a.kind != PURP_ACTION_NONE) goto found;
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        a.kind = PURP_ACTION_SWITCH_VT;
        a.vt = (int)(sym - XKB_KEY_XF86Switch_VT_1 + 1);
        goto found;
    }
    if (ctrl && (mods & PURP_MOD_ALT) && raw >= XKB_KEY_F1 && raw <= XKB_KEY_F12) {
        a.kind = PURP_ACTION_SWITCH_VT;
        a.vt = (int)(raw - XKB_KEY_F1 + 1);
        goto found;
    }
    if (!mod) return false;
    static const uint32_t shifted_tags[] = {
        XKB_KEY_exclam, XKB_KEY_at, XKB_KEY_numbersign, XKB_KEY_dollar,
        XKB_KEY_percent, XKB_KEY_asciicircum, XKB_KEY_ampersand,
        XKB_KEY_asterisk, XKB_KEY_parenleft
    };
    for (size_t i = 0; i < PURP_TAG_COUNT; ++i) {
        if (sym != XKB_KEY_1 + i && sym != shifted_tags[i]) continue;
        a.tags = (uint16_t)(1u << i);
        a.kind = ctrl ? (shift ? PURP_ACTION_TOGGLE_TAG_WINDOW : PURP_ACTION_TOGGLE_VIEW_TAGS)
                      : (shift ? PURP_ACTION_TAG_WINDOW : PURP_ACTION_VIEW_TAGS);
        goto found;
    }
    if (sym == XKB_KEY_0 || sym == XKB_KEY_parenright) {
        a.kind = ctrl ? PURP_ACTION_RESET_GAPS :
                 (shift ? PURP_ACTION_TAG_WINDOW : PURP_ACTION_VIEW_TAGS);
        a.tags = PURP_TAGMASK;
        goto found;
    }
    if (sym >= XKB_KEY_Left && sym <= XKB_KEY_Down) {
        a.kind = shift ? PURP_ACTION_RESIZE_WINDOW : PURP_ACTION_MOVE_WINDOW;
        if (sym == XKB_KEY_Left) a.dx = -1;
        if (sym == XKB_KEY_Right) a.dx = 1;
        if (sym == XKB_KEY_Up) a.dy = -1;
        if (sym == XKB_KEY_Down) a.dy = 1;
        goto found;
    }
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter: a.kind = shift ? PURP_ACTION_ZOOM : PURP_ACTION_SPAWN_TERMINAL; break;
    case XKB_KEY_Escape: a.kind = PURP_ACTION_SPAWN_SYSTEM_MONITOR; break;
    case XKB_KEY_p: case XKB_KEY_P: a.kind = PURP_ACTION_SPAWN_LAUNCHER; break;
    case XKB_KEY_w: case XKB_KEY_W:
        a.kind = shift ? PURP_ACTION_TOGGLE_WIFI : PURP_ACTION_SPAWN_WIFI; break;
    case XKB_KEY_s: case XKB_KEY_S: case XKB_KEY_grave:
        a.kind = PURP_ACTION_TOGGLE_SCRATCHPAD; break;
    case XKB_KEY_j: case XKB_KEY_J: a.kind = PURP_ACTION_FOCUS_DELTA; a.delta = 1; break;
    case XKB_KEY_k: case XKB_KEY_K: a.kind = PURP_ACTION_FOCUS_DELTA; a.delta = -1; break;
    case XKB_KEY_h: case XKB_KEY_H: a.kind = PURP_ACTION_TOGGLE_HELP; break;
    case XKB_KEY_bracketleft: a.kind = PURP_ACTION_ADJUST_MFACT; a.mfact_delta = -PURP_MFACT_STEP; break;
    case XKB_KEY_l: case XKB_KEY_L:
        a.kind = shift || ctrl ? PURP_ACTION_LOCK_SCREEN : PURP_ACTION_ADJUST_MFACT;
        a.mfact_delta = PURP_MFACT_STEP; break;
    case XKB_KEY_bracketright: a.kind = PURP_ACTION_ADJUST_MFACT; a.mfact_delta = PURP_MFACT_STEP; break;
    case XKB_KEY_i: case XKB_KEY_I: a.kind = PURP_ACTION_ADJUST_NMASTER; a.delta = 1; break;
    case XKB_KEY_d: case XKB_KEY_D: a.kind = PURP_ACTION_ADJUST_NMASTER; a.delta = -1; break;
    case XKB_KEY_t: case XKB_KEY_T: a.kind = PURP_ACTION_SET_LAYOUT; a.layout = PURP_LAYOUT_TILE; break;
    case XKB_KEY_m: case XKB_KEY_M: a.kind = PURP_ACTION_SET_LAYOUT; a.layout = PURP_LAYOUT_MONOCLE; break;
    case XKB_KEY_u: case XKB_KEY_U: a.kind = PURP_ACTION_SET_LAYOUT; a.layout = PURP_LAYOUT_CENTERED_MASTER; break;
    case XKB_KEY_g: case XKB_KEY_G:
        a.kind = shift ? PURP_ACTION_TOGGLE_GAPS : PURP_ACTION_SET_LAYOUT;
        a.layout = PURP_LAYOUT_GRID; break;
    case XKB_KEY_f: case XKB_KEY_F:
        a.kind = shift ? PURP_ACTION_TOGGLE_FULLSCREEN : PURP_ACTION_SET_LAYOUT;
        a.layout = PURP_LAYOUT_FLOATING; break;
    case XKB_KEY_F11: a.kind = PURP_ACTION_TOGGLE_FULLSCREEN; break;
    case XKB_KEY_o: case XKB_KEY_O: a.kind = PURP_ACTION_TOGGLE_OPACITY; break;
    case XKB_KEY_equal: case XKB_KEY_plus:
        a.kind = ctrl ? PURP_ACTION_ADJUST_SCALE : PURP_ACTION_ADJUST_GAPS;
        a.scale_delta = PURP_SCALE_STEP; a.delta = PURP_GAP_STEP; break;
    case XKB_KEY_minus: case XKB_KEY_underscore:
        a.kind = ctrl ? PURP_ACTION_ADJUST_SCALE : PURP_ACTION_ADJUST_GAPS;
        a.scale_delta = -PURP_SCALE_STEP; a.delta = -PURP_GAP_STEP; break;
    case XKB_KEY_space:
        a.kind = shift ? PURP_ACTION_TOGGLE_FLOATING :
                 (ctrl ? PURP_ACTION_CYCLE_LAYOUT : PURP_ACTION_TOGGLE_LAST_LAYOUT); break;
    case XKB_KEY_b: case XKB_KEY_B:
        a.kind = shift ? PURP_ACTION_SPAWN_BLUETOOTH :
                 (ctrl ? PURP_ACTION_TOGGLE_BLUETOOTH : PURP_ACTION_TOGGLE_BAR); break;
    case XKB_KEY_comma: case XKB_KEY_less:
        a.kind = shift ? PURP_ACTION_MOVE_WINDOW_TO_MONITOR : PURP_ACTION_FOCUS_MONITOR;
        a.delta = -1; break;
    case XKB_KEY_period: case XKB_KEY_greater:
        a.kind = shift ? PURP_ACTION_MOVE_WINDOW_TO_MONITOR : PURP_ACTION_FOCUS_MONITOR;
        a.delta = 1; break;
    case XKB_KEY_Tab: a.kind = PURP_ACTION_SWAP_PREV_TAGS; break;
    case XKB_KEY_q: case XKB_KEY_Q:
        a.kind = shift ? PURP_ACTION_QUIT : PURP_ACTION_CLOSE_WINDOW; break;
    default: return false;
    }
found:
    if (out) *out = a;
    return true;
}

static bool run_helper(const char *const *argv, char *out, size_t len) {
    return purp_run_capture(argv, PURP_BAR_COMMAND_TIMEOUT_MS, out, len);
}

static void *wifi_worker(void *unused) {
    char text[512];
    const char *const query[] = {"nmcli", "radio", "wifi", NULL};
    bool enabled = run_helper(query, text, sizeof(text)) && strstr(text, "enabled") != NULL;
    const char *const toggle[] = {"nmcli", "radio", "wifi", enabled ? "off" : "on", NULL};
    if (run_helper(toggle, text, sizeof(text)) && !enabled) {
        usleep(PURP_WIFI_CONNECT_DELAY_MS * 1000u);
        const char *const connect[] = {"nmcli", "device", "connect", PURP_WIFI_AUTOCONNECT_DEVICE, NULL};
        run_helper(connect, text, sizeof(text));
    }
    return NULL;
}

static void *bluetooth_worker(void *unused) {
    char text[2048];
    const char *const query[] = {"bluetoothctl", "show", NULL};
    bool enabled = run_helper(query, text, sizeof(text)) && strstr(text, "Powered: yes") != NULL;
    if (!enabled) {
        const char *const unblock[] = {"rfkill", "unblock", "bluetooth", NULL};
        run_helper(unblock, text, sizeof(text));
    }
    const char *const toggle[] = {"bluetoothctl", "power", enabled ? "off" : "on", NULL};
    run_helper(toggle, text, sizeof(text));
    return NULL;
}

static bool start_worker(void *(*func)(void *), void *arg) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    int rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (rc == 0) rc = pthread_create(&thread, &attr, func, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) purp_log(PURP_LOG_WARN, "Input helper thread: %s", strerror(rc));
    return rc == 0;
}

void purp_input_toggle_wifi_radio(purp_server_t *s) {
    start_worker(wifi_worker, NULL);
    purp_bar_request_immediate_refresh(&s->bar);
}

void purp_input_toggle_bluetooth_power(purp_server_t *s) {
    start_worker(bluetooth_worker, NULL);
    purp_bar_request_immediate_refresh(&s->bar);
}

static bool read_backlight_value(const char *name, const char *field, long long *value) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s/%s", PURP_BACKLIGHT_SYSFS_DIR, name, field);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    FILE *file = fopen(path, "re");
    if (!file) return false;
    bool ok = fscanf(file, "%lld", value) == 1;
    fclose(file);
    return ok;
}

static void *backlight_worker(void *arg) {
    int delta = *(int *)arg;
    free(arg);
    DIR *dir = opendir(PURP_BACKLIGHT_SYSFS_DIR);
    if (!dir) return NULL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        long long max, current;
        if (!read_backlight_value(entry->d_name, "max_brightness", &max) || max <= 0 ||
            max > UINT32_MAX || !read_backlight_value(entry->d_name, "brightness", &current)) continue;
        long long step = max * llabs((long long)delta) / 100;
        if (step < 1) step = 1;
        long long value = current + (delta < 0 ? -step : step);
        long long min = max / 100;
        if (min < 1) min = 1;
        if (value < min) value = min;
        if (value > max) value = max;
        char number[32], text[512];
        snprintf(number, sizeof(number), "%lld", value);
        const char *const command[] = {"busctl", "call", "org.freedesktop.login1",
            "/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session",
            "SetBrightness", "ssu", "backlight", entry->d_name, number, NULL};
        run_helper(command, text, sizeof(text));
    }
    closedir(dir);
    return NULL;
}

void purp_input_adjust_backlight_logind(int percent_delta) {
    if (!percent_delta) return;
    int *delta = malloc(sizeof(*delta));
    if (!delta) return;
    *delta = percent_delta;
    if (!start_worker(backlight_worker, delta)) free(delta);
}

void purp_input_spawn_launcher(purp_server_t *s) {
    const char *configured = getenv(PURP_ENV_SPOTLIGHT);
    if (configured && strcmp(configured, "none") == 0) return;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) runtime = s->runtime_dir;
    int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", runtime, PURP_SPOTLIGHT_SOCKET_NAME);
    if (n > 0 && (size_t)n < sizeof(addr.sun_path)) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd >= 0) {
            bool sent = false;
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                const char message[] = PURP_SPOTLIGHT_TOGGLE_MSG;
                size_t offset = 0;
                while (offset < sizeof(message) - 1) {
                    ssize_t count = send(fd, message + offset, sizeof(message) - 1 - offset, MSG_NOSIGNAL);
                    if (count < 0 && errno == EINTR) continue;
                    if (count <= 0) break;
                    offset += (size_t)count;
                }
                sent = offset == sizeof(message) - 1;
            }
            close(fd);
            if (sent) return;
        }
    }
    const char *const args[] = {"--toggle", NULL};
    purp_server_spawn_configured(s, PURP_ENV_SPOTLIGHT, args,
                                 PURP_COMMANDS(PURP_LAUNCHER_COMMANDS), "launcher");
}

static void spawn_list(purp_server_t *s, purp_command_list_t list, const char *label) {
    purp_server_spawn_first(s, list, label, false);
}

static void isolate_pointer_touch(purp_server_t *s) {
    purp_input_cancel_grabs(s);
    uint32_t now = purp_server_time_ms(s);
    for (size_t n = s->seat.pressed_button_count; n > 0; --n) {
        uint32_t button = s->seat.pressed_buttons[n - 1];
        swallow_button(s, button);
        purp_seat_pointer_send_button(s, now, button, false);
    }
    s->seat.pressed_button_count = 0;
    for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i) {
        if (!s->seat.touch[i].active) continue;
        int32_t slot = s->seat.touch[i].slot;
        swallow_touch(s, slot);
        purp_seat_touch_send_cancel(s, slot);
    }
    purp_seat_pointer_send_frame(s);
}

void purp_input_execute_action(purp_server_t *s, const purp_action_t *a, uint32_t keycode) {
    if (!a || purp_server_is_locked(s)) return;
    purp_wm_t *wm = &s->wm;
    purp_window_t *win;
    bool relayout = true;
    switch (a->kind) {
    case PURP_ACTION_NONE: return;
    case PURP_ACTION_TOGGLE_HELP:
        if (!s->help.visible) isolate_pointer_touch(s);
        purp_help_toggle(&s->help);
        purp_input_sync_pointer_focus(s, 0);
        purp_host_damage_all(s);
        return;
    case PURP_ACTION_SPAWN_LAUNCHER: purp_input_spawn_launcher(s); return;
    case PURP_ACTION_SPAWN_TERMINAL: purp_server_spawn_terminal(s); return;
    case PURP_ACTION_SPAWN_SYSTEM_MONITOR: spawn_list(s, PURP_COMMANDS(PURP_SYSTEM_MONITOR_COMMANDS), "system monitor"); return;
    case PURP_ACTION_SPAWN_WIFI: spawn_list(s, PURP_COMMANDS(PURP_WIFI_COMMANDS), "Wi-Fi manager"); return;
    case PURP_ACTION_TOGGLE_WIFI: purp_input_toggle_wifi_radio(s); return;
    case PURP_ACTION_SPAWN_BLUETOOTH: spawn_list(s, PURP_COMMANDS(PURP_BLUETOOTH_COMMANDS), "Bluetooth manager"); return;
    case PURP_ACTION_TOGGLE_BLUETOOTH: purp_input_toggle_bluetooth_power(s); return;
    case PURP_ACTION_TOGGLE_SCRATCHPAD:
        if (purp_wm_toggle_scratchpad(wm) == PURP_SCRATCHPAD_NEEDS_SPAWN) purp_server_spawn_terminal(s);
        break;
    case PURP_ACTION_LOCK_SCREEN: purp_server_lock_session(s); return;
    case PURP_ACTION_CLOSE_WINDOW:
        win = purp_wm_focused_window(wm);
        if (win && win->toplevel) purp_xdg_toplevel_send_close(win->toplevel);
        return;
    case PURP_ACTION_QUIT: s->should_quit = true; return;
    case PURP_ACTION_FOCUS_DELTA: s->layer_focus = NULL; purp_wm_cycle_focus(wm, a->delta); break;
    case PURP_ACTION_MOVE_WINDOW: purp_wm_move_focused_window(wm, a->dx, a->dy); break;
    case PURP_ACTION_RESIZE_WINDOW:
        win = purp_wm_focused_window(wm);
        if (!win || win->is_fullscreen) return;
        if (!s->key_resize.active || s->key_resize.window_id != win->id) {
            s->key_resize.active = true;
            s->key_resize.window_id = win->id;
            purp_key_glide_init(&s->key_resize.glide, purp_now_us());
        }
        purp_key_glide_press(&s->key_resize.glide, keycode != 0, keycode, a->dx, a->dy, purp_now_us());
        purp_host_damage_all(s);
        return;
    case PURP_ACTION_ADJUST_MFACT: purp_wm_adjust_mfact(wm, a->mfact_delta); break;
    case PURP_ACTION_ADJUST_NMASTER: purp_wm_adjust_nmaster(wm, a->delta); break;
    case PURP_ACTION_ADJUST_GAPS: purp_wm_adjust_gaps(wm, a->delta); break;
    case PURP_ACTION_ADJUST_SCALE: purp_server_adjust_scale(s, a->scale_delta); return;
    case PURP_ACTION_RESET_GAPS: purp_wm_reset_gaps(wm); break;
    case PURP_ACTION_TOGGLE_GAPS: purp_wm_toggle_gaps(wm); break;
    case PURP_ACTION_ZOOM: purp_wm_zoom_focused(wm); break;
    case PURP_ACTION_SET_LAYOUT: purp_wm_set_layout(wm, a->layout); break;
    case PURP_ACTION_CYCLE_LAYOUT: purp_wm_cycle_layout(wm); break;
    case PURP_ACTION_TOGGLE_LAST_LAYOUT: purp_wm_toggle_last_layout(wm); break;
    case PURP_ACTION_TOGGLE_FLOATING: purp_wm_toggle_floating_focused(wm); break;
    case PURP_ACTION_TOGGLE_FULLSCREEN: purp_wm_toggle_fullscreen_focused(wm); break;
    case PURP_ACTION_TOGGLE_OPACITY: purp_wm_toggle_opacity_focused(wm); break;
    case PURP_ACTION_TOGGLE_BAR: purp_wm_toggle_bar(wm); break;
    case PURP_ACTION_VIEW_TAGS: purp_wm_view_tags(wm, a->tags); break;
    case PURP_ACTION_TOGGLE_VIEW_TAGS: purp_wm_toggle_view_tags(wm, a->tags); break;
    case PURP_ACTION_TAG_WINDOW: purp_wm_tag_focused_window(wm, a->tags); break;
    case PURP_ACTION_TOGGLE_TAG_WINDOW: purp_wm_toggle_tag_focused_window(wm, a->tags); break;
    case PURP_ACTION_SWAP_PREV_TAGS: purp_wm_swap_prev_tags(wm); break;
    case PURP_ACTION_VOLUME_UP: spawn_list(s, PURP_COMMANDS(PURP_VOL_UP_COMMANDS), "volume up"); relayout = false; break;
    case PURP_ACTION_VOLUME_DOWN: spawn_list(s, PURP_COMMANDS(PURP_VOL_DOWN_COMMANDS), "volume down"); relayout = false; break;
    case PURP_ACTION_VOLUME_MUTE: spawn_list(s, PURP_COMMANDS(PURP_VOL_MUTE_COMMANDS), "volume mute"); relayout = false; break;
    case PURP_ACTION_MIC_MUTE: spawn_list(s, PURP_COMMANDS(PURP_MIC_MUTE_COMMANDS), "microphone mute"); relayout = false; break;
    case PURP_ACTION_BRIGHTNESS_UP: case PURP_ACTION_BRIGHTNESS_DOWN: {
        bool up = a->kind == PURP_ACTION_BRIGHTNESS_UP;
        purp_command_list_t list = up ? PURP_COMMANDS(PURP_BRIGHTNESS_UP_COMMANDS) : PURP_COMMANDS(PURP_BRIGHTNESS_DOWN_COMMANDS);
        if (purp_command_exists(list.items[0].argv[0])) spawn_list(s, list, "brightness");
        else purp_input_adjust_backlight_logind(up ? PURP_BRIGHTNESS_STEP_PERCENT : -PURP_BRIGHTNESS_STEP_PERCENT);
        return;
    }
    case PURP_ACTION_MEDIA_PLAY: spawn_list(s, PURP_COMMANDS(PURP_MEDIA_PLAY_COMMANDS), "media play"); return;
    case PURP_ACTION_MEDIA_NEXT: spawn_list(s, PURP_COMMANDS(PURP_MEDIA_NEXT_COMMANDS), "media next"); return;
    case PURP_ACTION_MEDIA_PREV: spawn_list(s, PURP_COMMANDS(PURP_MEDIA_PREV_COMMANDS), "media previous"); return;
    case PURP_ACTION_SCREENSHOT: purp_server_request_screenshot(s, false); return;
    case PURP_ACTION_SCREENSHOT_WINDOW: purp_server_request_screenshot(s, true); return;
    case PURP_ACTION_FOCUS_MONITOR: s->layer_focus = NULL; purp_wm_cycle_monitor(wm, a->delta); break;
    case PURP_ACTION_MOVE_WINDOW_TO_MONITOR: purp_wm_move_focused_window_to_monitor(wm, a->delta); break;
    case PURP_ACTION_SWITCH_VT: if (a->vt >= 1 && a->vt <= 12) s->pending_vt_switch = a->vt; return;
    }
    if (relayout) purp_server_relayout(s);
    else purp_bar_request_immediate_refresh(&s->bar);
}

void purp_host_keyboard_key(purp_server_t *s, uint32_t time, uint32_t keycode, bool pressed) {
    uint32_t sym = purp_seat_keysym(s, keycode);
    uint32_t raw = purp_seat_raw_keysym(s, keycode);
    bool changed = purp_seat_update_xkb(s, keycode, pressed);
    if (pressed) sym = purp_seat_keysym(s, keycode);
    if (!pressed && s->key_resize.active) purp_key_glide_release(&s->key_resize.glide, keycode);
    if (purp_server_is_locked(s)) {
        s->mod_tap_candidate = false;
        remove_code(s->intercepted_keys, &s->intercepted_key_count, keycode);
        if (!pressed) remove_code(s->seat.pressed_keys, &s->seat.pressed_key_count, keycode);
        if (pressed && s->lock_screen) {
            purp_lock_key_t key;
            if (purp_lock_key_from_keysym(sym, purp_seat_utf32(s, keycode), s->seat.modifiers, &key)) {
                purp_lock_handle_key(s->lock_screen, key);
                purp_host_damage_all(s);
            }
        }
        return;
    }
    bool solo = false;
    if (is_mod_sym(sym) || is_mod_sym(raw)) {
        if (pressed) {
            bool other = s->mod_tap_candidate || s->seat.pressed_key_count != 0;
            s->mod_tap_candidate = !other;
        } else {
            solo = s->mod_tap_candidate;
            s->mod_tap_candidate = false;
        }
    } else if (pressed) {
        s->mod_tap_candidate = false;
        remove_code(s->intercepted_keys, &s->intercepted_key_count, keycode);
        purp_action_t action;
        if (purp_input_lookup_binding(sym, raw, s->seat.modifiers, s->help.visible, &action)) {
            add_code(s->intercepted_keys, &s->intercepted_key_count, PURP_MAX_INTERCEPTED_KEYS, keycode);
            purp_input_execute_action(s, &action, keycode);
            if (changed) purp_seat_keyboard_send_modifiers(s);
            return;
        }
    } else if (remove_code(s->intercepted_keys, &s->intercepted_key_count, keycode)) {
        if (changed) purp_seat_keyboard_send_modifiers(s);
        return;
    }
    purp_input_sync_keyboard_focus(s);
    purp_seat_keyboard_send_key(s, time, keycode, pressed);
    if (changed) purp_seat_keyboard_send_modifiers(s);
    if (solo) purp_input_spawn_launcher(s);
}

void purp_input_sync_keyboard_focus(purp_server_t *s) {
    purp_surface_t *surface = NULL;
    if (!purp_server_is_locked(s)) {
        purp_layer_surface_t *layer = purp_layer_keyboard_target(s);
        if (layer) surface = layer->surface;
        else {
            purp_window_t *win = purp_wm_focused_window(&s->wm);
            if (win && purp_wm_is_window_visible(&s->wm, win)) surface = purp_xdg_window_surface(win);
        }
    }
    if (!wl_list_empty(&s->popup_grabs)) {
        struct wl_client *client = surface ? wl_resource_get_client(surface->resource) : NULL;
        purp_popup_t *popup;
        wl_list_for_each(popup, &s->popup_grabs, grab_link) {
            if (wl_resource_get_client(popup->xdg->surface->resource) != client) {
                purp_xdg_dismiss_popup_grabs(s);
                break;
            }
        }
        if (!wl_list_empty(&s->popup_grabs)) {
            popup = wl_container_of(s->popup_grabs.next, popup, grab_link);
            surface = popup->xdg->surface;
        }
    }
    purp_seat_set_keyboard_focus(s, surface);
}

static purp_surface_t *tree_at(purp_surface_t *root, purp_pointf_t pt,
                                purp_pointf_t root_origin, purp_pointf_t *origin) {
    double sx, sy;
    purp_surface_t *surface = purp_surface_at(root, pt.x - root_origin.x,
                                            pt.y - root_origin.y, &sx, &sy);
    if (surface && origin) *origin = (purp_pointf_t){pt.x - sx, pt.y - sy};
    return surface;
}

purp_surface_t *purp_input_surface_under(purp_server_t *s, purp_pointf_t pt, purp_pointf_t *origin) {
    if (origin) *origin = (purp_pointf_t){0};
    if (purp_server_is_locked(s) || s->help.visible) return NULL;
    purp_popup_t *popup = purp_xdg_popup_at(s, pt);
    if (popup) {
        purp_point_t loc = purp_xdg_popup_global_loc(popup);
        purp_xdg_surface_t *xdg = popup->xdg;
        purp_pointf_t base = {
            input_clamp_i32((int64_t)loc.x - (xdg->has_geometry ? xdg->geometry.x : 0)),
            input_clamp_i32((int64_t)loc.y - (xdg->has_geometry ? xdg->geometry.y : 0))};
        purp_surface_t *hit = tree_at(xdg->surface, pt, base, origin);
        if (hit) return hit;
    }
    purp_pointf_t layer_origin;
    purp_layer_surface_t *layer = purp_layer_at(s, pt, &layer_origin);
    if (layer) {
        purp_surface_t *hit = tree_at(layer->surface, pt, layer_origin, origin);
        if (hit) return hit;
    }
    /* The bar is drawn over windows, but never over top/overlay layers or popups. */
    size_t mon;
    int32_t local_x;
    if (purp_wm_bar_hit_monitor(&s->wm, pt, &mon, &local_x)) return NULL;
    size_t count = s->wm.window_count;
    if (count) {
        size_t order[count];
        size_t n = purp_wm_stacking_order(&s->wm, order, count);
        for (size_t i = 0; i < n; ++i) {
            purp_window_t *win = s->wm.windows[order[i]];
            if (!purp_rect_contains_f(win->geo, pt)) continue;
            purp_surface_t *root = purp_xdg_window_surface(win);
            if (!root) continue;
            purp_point_t loc = purp_window_surface_loc(win);
            purp_surface_t *hit = tree_at(root, pt, (purp_pointf_t){loc.x, loc.y}, origin);
            if (hit) return hit;
            if (origin) *origin = (purp_pointf_t){loc.x, loc.y};
            return root;
        }
    }
    wl_list_for_each_reverse(layer, &s->layers, link) {
        if (!purp_layer_is_mapped(layer) || purp_layer_is_keepalive(layer) ||
            purp_layer_is_above_windows(layer)) continue;
        purp_surface_t *hit = tree_at(layer->surface, pt,
                                      (purp_pointf_t){layer->geo.x, layer->geo.y}, origin);
        if (hit) return hit;
    }
    return NULL;
}

void purp_input_sync_pointer_focus(purp_server_t *s, uint32_t time) {
    if (!time) time = purp_server_time_ms(s);
    if (purp_server_is_locked(s) || s->help.visible) {
        if (purp_seat_drag_active(s)) purp_seat_drag_cancel(s);
        purp_seat_pointer_focus(s, NULL, (purp_pointf_t){0}, time);
        return;
    }
    if (s->grab.kind != PURP_GRAB_NONE || s->seat.pressed_button_count) return;
    purp_pointf_t origin;
    purp_surface_t *surface = purp_input_surface_under(s, s->wm.pointer_location, &origin);
    purp_seat_pointer_focus(s, surface, origin, time);
}

void purp_input_cancel_grabs(purp_server_t *s) {
    s->grab = (purp_interactive_grab_t){.touch_slot = -1};
    s->touch_grab_slot = -1;
    s->key_resize = (purp_key_resize_t){0};
    if (purp_seat_drag_active(s)) purp_seat_drag_cancel(s);
}

bool purp_input_tick_key_resize(purp_server_t *s) {
    if (!s->key_resize.active) return false;
    int idx = purp_wm_window_index_by_id(&s->wm, s->key_resize.window_id);
    if (purp_server_is_locked(s) || s->help.visible || idx < 0 ||
        !purp_wm_is_window_visible(&s->wm, s->wm.windows[idx]) || s->wm.windows[idx]->is_fullscreen) {
        s->key_resize.active = false;
        return false;
    }
    if (!purp_mods_has_mod(s->seat.modifiers) || !(s->seat.modifiers & PURP_MOD_SHIFT))
        purp_key_glide_release_all(&s->key_resize.glide);
    purp_point_t delta = purp_key_glide_advance(&s->key_resize.glide, purp_now_us());
    bool done = purp_key_glide_is_done(&s->key_resize.glide);
    bool moved = delta.x || delta.y;
    if (moved) {
        purp_wm_resize_window_by(&s->wm, (size_t)idx, delta.x, delta.y);
        purp_server_relayout(s);
    }
    if (done) s->key_resize.active = false;
    return moved || !done;
}

static bool begin_grab(purp_server_t *s, size_t idx, purp_pointf_t pos,
                         bool resize, bool allow_tiled, uint32_t edges, bool forward, int32_t slot) {
    purp_window_t *win = s->wm.windows[idx];
    if (win->is_fullscreen) return false;
    purp_interactive_grab_t grab = {.window_id = win->id, .start_pointer = pos,
        .edges = edges, .forward_release = forward, .touch_slot = slot};
    if (resize && allow_tiled && purp_wm_tiled_resize_plan(&s->wm, idx, edges, &grab.plan)) {
        grab.kind = PURP_GRAB_TILED_RESIZE;
    } else {
        if (!purp_wm_floating_grab_geometry(&s->wm, idx, &grab.start_geo)) return false;
        grab.kind = resize ? PURP_GRAB_RESIZE : PURP_GRAB_MOVE;
    }
    s->grab = grab;
    s->touch_grab_slot = slot;
    purp_server_relayout(s);
    return true;
}

void purp_input_begin_client_grab(purp_server_t *s, purp_toplevel_t *top,
                                  uint32_t serial, bool resize, uint32_t edges) {
    if (!top || !top->window || purp_server_is_locked(s) || s->help.visible ||
        s->grab.kind != PURP_GRAB_NONE || purp_seat_drag_active(s)) return;
    if (resize && (!edges || (edges & ~15u) ||
        (edges & (PURP_EDGE_LEFT | PURP_EDGE_RIGHT)) == (PURP_EDGE_LEFT | PURP_EDGE_RIGHT) ||
        (edges & (PURP_EDGE_TOP | PURP_EDGE_BOTTOM)) == (PURP_EDGE_TOP | PURP_EDGE_BOTTOM))) return;
    struct wl_client *client = wl_resource_get_client(top->resource);
    if (!purp_seat_validate_grab_serial(s, client, serial)) return;
    int idx = purp_wm_window_index(&s->wm, top->window);
    if (idx < 0 || !purp_wm_is_window_visible(&s->wm, top->window)) return;
    int32_t slot = -1;
    purp_pointf_t pos = s->wm.pointer_location;
    bool pointer = serial == s->seat.last_button_serial && s->seat.pressed_button_count &&
        purp_xdg_window_for_surface(s, s->seat.pointer_focus) == top->window;
    if (!pointer && serial == s->seat.last_touch_serial) {
        for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i) {
            purp_touch_point_t *touch = &s->seat.touch[i];
            if (touch->active && touch->surface &&
                purp_xdg_window_for_surface(s, touch->surface) == top->window) {
                slot = touch->slot;
                pos = touch->pos;
                break;
            }
        }
    }
    if (!pointer && slot < 0) return;
    purp_wm_focus_window(&s->wm, idx);
    begin_grab(s, (size_t)idx, pos, resize, true, edges, true, slot);
}

static bool update_grab(purp_server_t *s, purp_pointf_t pos) {
    purp_interactive_grab_t *grab = &s->grab;
    if (grab->kind == PURP_GRAB_NONE) return false;
    purp_window_t *win = purp_wm_window_by_id(&s->wm, grab->window_id);
    if (!win || win->is_fullscreen) {
        purp_input_cancel_grabs(s);
        return true;
    }
    int32_t dx = purp_round_i32(pos.x - grab->start_pointer.x);
    int32_t dy = purp_round_i32(pos.y - grab->start_pointer.y);
    if (grab->kind == PURP_GRAB_TILED_RESIZE) {
        purp_wm_apply_tiled_resize(&s->wm, &grab->plan, dx, dy);
    } else if (grab->kind == PURP_GRAB_RESIZE) {
        purp_rect_t wanted = purp_resize_rect(grab->start_geo, dx, dy, grab->edges,
                                              PURP_MIN_DRAG_WIDTH, PURP_MIN_DRAG_HEIGHT);
        purp_size_t size = purp_window_clamp_frame_to_client_limits(win, purp_rect_size(wanted));
        wanted.w = size.w;
        wanted.h = size.h;
        if (grab->edges & PURP_EDGE_LEFT) wanted.x = grab->start_geo.x + grab->start_geo.w - size.w;
        if (grab->edges & PURP_EDGE_TOP) wanted.y = grab->start_geo.y + grab->start_geo.h - size.h;
        win->size_from_client = false;
        win->geo = win->float_geo = wanted;
    } else {
        purp_point_t loc = {grab->start_geo.x + dx, grab->start_geo.y + dy};
        loc = purp_clamp_floating_loc_across_monitors(loc, purp_rect_size(grab->start_geo),
                                                       s->wm.monitors, s->wm.monitor_count);
        win->geo.x = win->float_geo.x = loc.x;
        win->geo.y = win->float_geo.y = loc.y;
        int mon = purp_wm_monitor_at(&s->wm, pos);
        if (mon < 0) mon = (int)purp_wm_nearest_enabled_monitor(&s->wm, purp_rect_center(win->geo));
        if ((size_t)mon < s->wm.monitor_count) {
            bool changed = win->monitor != mon;
            win->monitor = mon;
            if (changed && !win->is_scratchpad) win->tags = s->wm.monitors[mon].workspace.selected_tags;
            s->wm.focused_monitor = mon;
            if (changed) purp_server_update_surface_outputs(s);
        }
    }
    purp_server_relayout(s);
    return true;
}

void purp_input_handle_bar_click(purp_server_t *s, size_t mon, int32_t x, uint32_t button) {
    if (mon >= s->wm.monitor_count || purp_server_is_locked(s) || s->help.visible) return;
    s->wm.focused_monitor = (int)mon;
    purp_bar_action_t hit = purp_bar_hit_test(&s->bar, mon, x);
    purp_workspace_t *ws = &s->wm.monitors[mon].workspace;
    switch (hit.kind) {
    case PURP_BAR_ACTION_TAG: {
        uint16_t tags = (uint16_t)(1u << hit.tag);
        if (button == PURP_BTN_RIGHT || (s->seat.modifiers & PURP_MOD_CTRL))
            purp_wm_toggle_view_tags_on(&s->wm, mon, tags);
        else if (button == PURP_BTN_MIDDLE || (s->seat.modifiers & PURP_MOD_SHIFT))
            purp_wm_tag_focused_window(&s->wm, tags);
        else purp_wm_view_tags_on(&s->wm, mon, tags);
        break;
    }
    case PURP_BAR_ACTION_CYCLE_LAYOUT:
        purp_wm_set_layout_on(&s->wm, mon, button == PURP_BTN_RIGHT ?
            purp_workspace_prev_layout(ws) : purp_layout_next(purp_workspace_layout(ws)));
        break;
    case PURP_BAR_ACTION_WIFI:
        if (button == PURP_BTN_RIGHT) purp_input_toggle_wifi_radio(s);
        else spawn_list(s, PURP_COMMANDS(PURP_WIFI_COMMANDS), "Wi-Fi manager");
        break;
    case PURP_BAR_ACTION_BLUETOOTH:
        if (button == PURP_BTN_RIGHT) purp_input_toggle_bluetooth_power(s);
        else spawn_list(s, PURP_COMMANDS(PURP_BLUETOOTH_COMMANDS), "Bluetooth manager");
        break;
    case PURP_BAR_ACTION_VOLUME:
        if (button == PURP_BTN_RIGHT) spawn_list(s, PURP_COMMANDS(PURP_MIXER_COMMANDS), "audio mixer");
        else {
            spawn_list(s, PURP_COMMANDS(PURP_VOL_MUTE_COMMANDS), "volume mute");
            purp_bar_request_immediate_refresh(&s->bar);
        }
        break;
    case PURP_BAR_ACTION_FLAMESHOT:
        if (button == PURP_BTN_LEFT) spawn_list(s, PURP_COMMANDS(PURP_FLAMESHOT_COMMANDS), "Flameshot");
        break;
    case PURP_BAR_ACTION_SYSTEM_MONITOR:
        spawn_list(s, PURP_COMMANDS(PURP_SYSTEM_MONITOR_COMMANDS), "system monitor"); break;
    case PURP_BAR_ACTION_FOCUS_NEXT:
        if (button == PURP_BTN_MIDDLE) purp_server_spawn_terminal(s);
        else purp_wm_cycle_focus(&s->wm, 1);
        break;
    }
    purp_server_relayout(s);
}

static void pointer_motion(purp_server_t *s, uint32_t time, purp_pointf_t pos) {
    int old_hovered = purp_wm_window_at(&s->wm, s->wm.pointer_location);
    s->wm.pointer_location = pos;
    if (purp_server_is_locked(s)) {
        if (purp_seat_drag_active(s)) purp_seat_drag_cancel(s);
        return;
    }
    purp_host_damage_all(s);
    if (s->help.visible) {
        if (purp_wm_focus_monitor_at_point(&s->wm, pos)) purp_server_relayout(s);
        purp_input_sync_pointer_focus(s, time);
        return;
    }
    if (purp_seat_drag_active(s)) {
        if (!s->seat.drag.is_touch) purp_seat_drag_motion(s, time);
        return;
    }
    if (s->grab.touch_slot < 0 && update_grab(s, pos)) return;
    if (s->seat.pressed_button_count || s->grab.kind != PURP_GRAB_NONE) {
        purp_seat_pointer_send_motion(s, time);
        return;
    }
    purp_popup_t *popup = purp_xdg_popup_at(s, pos);
    bool popup_grabbed = !wl_list_empty(&s->popup_grabs);
    bool switched = !popup_grabbed && !popup && purp_wm_focus_monitor_at_point(&s->wm, pos);
    bool changed = switched;
    purp_pointf_t origin;
    size_t mon;
    int32_t x;
    if (!popup_grabbed && !popup && !purp_layer_at(s, pos, &origin) &&
        !purp_wm_bar_hit_monitor(&s->wm, pos, &mon, &x)) {
        int idx = purp_wm_window_at(&s->wm, pos);
        if (idx >= 0 && (idx != old_hovered || switched)) {
            s->layer_focus = NULL;
            purp_wm_focus_window(&s->wm, idx);
            changed = true;
        }
    }
    /* Monitor focus can change focused_idx itself: never skip keyboard synchronization. */
    if (changed) purp_server_relayout(s);
    if (switched) purp_input_sync_keyboard_focus(s);
    purp_input_sync_pointer_focus(s, time);
}

void purp_host_pointer_motion(purp_server_t *s, uint32_t time, double dx, double dy,
                              double dx_unaccel, double dy_unaccel) {
    purp_pointf_t pos = purp_clamp_pointer_to_monitors(s->wm.monitors, s->wm.monitor_count,
        s->wm.pointer_location, (purp_pointf_t){dx, dy});
    pointer_motion(s, time, pos);
}

void purp_host_pointer_motion_absolute(purp_server_t *s, uint32_t time, double x, double y) {
    purp_pointf_t pos = purp_clamp_pointer_to_monitors(s->wm.monitors, s->wm.monitor_count,
        (purp_pointf_t){x, y}, (purp_pointf_t){0});
    pointer_motion(s, time, pos);
}

static void forward_button(purp_server_t *s, uint32_t time, uint32_t button, bool pressed) {
    purp_input_sync_pointer_focus(s, time);
    purp_seat_pointer_send_button(s, time, button, pressed);
    purp_seat_pointer_send_frame(s);
    if (pressed) purp_input_sync_keyboard_focus(s);
    if (!s->seat.pressed_button_count) purp_input_sync_pointer_focus(s, time);
}

static void focus_layer(purp_server_t *s, purp_layer_surface_t *layer) {
    if (layer->current.keyboard_interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
        s->layer_focus = layer;
        purp_input_sync_keyboard_focus(s);
    }
}

static bool dismiss_notification(purp_server_t *s, purp_pointf_t pos) {
    size_t mon = purp_wm_active_monitor_index(&s->wm);
    bool hit = purp_notify_dismiss_at(&s->notify, pos, purp_wm_monitor_rect(&s->wm, mon),
                                      purp_wm_bar_visible_on(&s->wm, mon));
    if (hit) purp_host_damage_all(s);
    return hit;
}

static void focus_clicked_window(purp_server_t *s, purp_pointf_t pos) {
    s->layer_focus = NULL;
    bool changed = purp_wm_focus_monitor_at_point(&s->wm, pos);
    int idx = purp_wm_window_at(&s->wm, pos);
    if (idx >= 0 && (changed || idx != s->wm.focused_idx)) {
        purp_wm_focus_window(&s->wm, idx);
        changed = true;
    }
    if (changed) purp_server_relayout(s);
    else purp_input_sync_keyboard_focus(s);
}

static void dismiss_outside_popups(purp_server_t *s, purp_pointf_t pos) {
    if (wl_list_empty(&s->popup_grabs)) return;
    purp_pointf_t origin;
    purp_surface_t *under = purp_input_surface_under(s, pos, &origin);
    purp_popup_t *popup;
    bool inside = false;
    wl_list_for_each(popup, &s->popup_grabs, grab_link) {
        if (under && purp_surface_root(under) == popup->xdg->surface) inside = true;
    }
    if (!inside) purp_xdg_dismiss_popup_grabs(s);
}

void purp_host_pointer_button(purp_server_t *s, uint32_t time, uint32_t button, bool pressed) {
    if (pressed) s->mod_tap_candidate = false;
    bool swallowed = !pressed && remove_code(s->swallowed_buttons, &s->swallowed_button_count, button);
    if (purp_server_is_locked(s)) {
        if (pressed) swallow_button(s, button);
        else if (!swallowed) remove_code(s->seat.pressed_buttons, &s->seat.pressed_button_count, button);
        if (purp_seat_drag_active(s)) purp_seat_drag_cancel(s);
        return;
    }
    if (s->help.visible) {
        if (pressed) {
            s->help.visible = false;
            swallow_button(s, button);
            purp_input_sync_pointer_focus(s, time);
            purp_host_damage_all(s);
        }
        return;
    }
    if (purp_seat_drag_active(s)) {
        if (pressed) swallow_button(s, button);
        else if (!swallowed && s->seat.pressed_button_count) {
            if (!s->seat.drag.is_touch && button == s->seat.pressed_buttons[0])
                purp_seat_drag_drop(s, time);
            forward_button(s, time, button, false);
        }
        return;
    }
    if (pressed && s->grab.kind != PURP_GRAB_NONE && s->grab.touch_slot < 0) {
        s->grab.kind = PURP_GRAB_NONE;
        s->touch_grab_slot = -1;
        purp_server_relayout(s);
    }
    if (!pressed && s->grab.kind != PURP_GRAB_NONE && s->grab.touch_slot < 0) {
        bool forward = s->grab.forward_release;
        s->grab.kind = PURP_GRAB_NONE;
        s->touch_grab_slot = -1;
        if (forward) forward_button(s, time, button, false);
        purp_server_relayout(s);
        return;
    }
    if (swallowed) return;
    /* An implicit client grab owns all buttons until its last release. */
    if (s->seat.pressed_button_count) {
        forward_button(s, time, button, pressed);
        return;
    }
    purp_pointf_t pos = s->wm.pointer_location;
    if (pressed && dismiss_notification(s, pos)) {
        swallow_button(s, button);
        purp_input_sync_pointer_focus(s, time);
        return;
    }
    if (pressed) dismiss_outside_popups(s, pos);
    if (purp_xdg_popup_at(s, pos)) {
        forward_button(s, time, button, pressed);
        return;
    }
    purp_pointf_t origin;
    purp_layer_surface_t *layer = purp_layer_at(s, pos, &origin);
    if (layer) {
        if (pressed) focus_layer(s, layer);
        forward_button(s, time, button, pressed);
        return;
    }
    size_t mon;
    int32_t x;
    if (pressed && purp_wm_bar_hit_monitor(&s->wm, pos, &mon, &x)) {
        swallow_button(s, button);
        purp_input_handle_bar_click(s, mon, x, button);
        return;
    }
    if (pressed && purp_mods_has_mod(s->seat.modifiers) &&
        (button == PURP_BTN_LEFT || button == PURP_BTN_RIGHT || button == PURP_BTN_MIDDLE)) {
        int idx = purp_wm_window_at(&s->wm, pos);
        if (idx >= 0) {
            purp_wm_focus_window(&s->wm, idx);
            s->layer_focus = NULL;
            swallow_button(s, button);
            if (button == PURP_BTN_MIDDLE) {
                purp_wm_toggle_floating_focused(&s->wm);
                purp_server_relayout(s);
            } else {
                purp_rect_t geo = s->wm.windows[idx]->geo;
                uint32_t edges = PURP_EDGE_BOTTOM_RIGHT;
                purp_tiled_resize_t plan;
                uint32_t nearest = (pos.x >= geo.x + geo.w / 2.0 ? PURP_EDGE_RIGHT : PURP_EDGE_LEFT) |
                                   (pos.y >= geo.y + geo.h / 2.0 ? PURP_EDGE_BOTTOM : PURP_EDGE_TOP);
                bool allow_tiled = button == PURP_BTN_RIGHT &&
                    purp_wm_tiled_resize_plan(&s->wm, (size_t)idx, nearest, &plan);
                if (allow_tiled) edges = nearest;
                /* A missing nearest split must float, not retry the far bottom/right split. */
                if (!begin_grab(s, (size_t)idx, pos, button == PURP_BTN_RIGHT, allow_tiled, edges, false, -1))
                    purp_server_relayout(s);
            }
            return;
        }
    }
    if (pressed) focus_clicked_window(s, pos);
    forward_button(s, time, button, pressed);
}

void purp_host_pointer_axis(purp_server_t *s, const purp_axis_event_t *event) {
    if (purp_server_is_locked(s) || s->help.visible) return;
    purp_axis_event_t axis = *event;
    if (axis.vertical == 0 && axis.vertical_v120) {
        axis.has_vertical = true;
        axis.vertical = axis.vertical_v120 * PURP_BAR_SCROLL_STEP / 120.0;
    }
    if (axis.horizontal == 0 && axis.horizontal_v120) {
        axis.has_horizontal = true;
        axis.horizontal = axis.horizontal_v120 * PURP_BAR_SCROLL_STEP / 120.0;
    }
    purp_pointf_t origin, pos = s->wm.pointer_location;
    size_t mon;
    int32_t x;
    if (!purp_seat_drag_active(s) && !s->seat.pressed_button_count &&
        s->grab.kind == PURP_GRAB_NONE && !purp_xdg_popup_at(s, pos) &&
        !purp_layer_at(s, pos, &origin) && purp_wm_bar_hit_monitor(&s->wm, pos, &mon, &x)) {
        s->bar_scroll_accum += axis.vertical;
        while (fabs(s->bar_scroll_accum) >= PURP_BAR_SCROLL_STEP) {
            int dir = s->bar_scroll_accum > 0 ? 1 : -1;
            s->bar_scroll_accum -= dir * PURP_BAR_SCROLL_STEP;
            if (purp_bar_hit_test(&s->bar, mon, x).kind == PURP_BAR_ACTION_VOLUME) {
                purp_action_t action = {.kind = dir > 0 ? PURP_ACTION_VOLUME_DOWN : PURP_ACTION_VOLUME_UP};
                purp_input_execute_action(s, &action, 0);
            } else {
                int next = (s->wm.monitors[mon].workspace.cur_tag + dir + PURP_TAG_COUNT) % PURP_TAG_COUNT;
                purp_wm_view_tags_on(&s->wm, mon, (uint16_t)(1u << next));
                purp_server_relayout(s);
            }
        }
        return;
    }
    s->bar_scroll_accum = 0;
    if (purp_seat_drag_active(s) || s->grab.kind != PURP_GRAB_NONE) return;
    purp_input_sync_pointer_focus(s, axis.time_ms);
    purp_seat_pointer_send_axis(s, &axis);
    purp_seat_pointer_send_frame(s);
}

void purp_host_pointer_frame(purp_server_t *s) {
    if (!purp_server_is_locked(s) && !s->help.visible) purp_seat_pointer_send_frame(s);
}

static void forward_touch_down(purp_server_t *s, uint32_t time, int32_t slot, purp_pointf_t pos) {
    purp_pointf_t origin;
    purp_surface_t *surface = purp_input_surface_under(s, pos, &origin);
    if (!surface) { swallow_touch(s, slot); return; }
    purp_seat_touch_send_down(s, time, slot, surface, origin, pos);
    purp_seat_touch_send_frame(s);
}

void purp_host_touch_down(purp_server_t *s, uint32_t time, int32_t slot, double x, double y) {
    s->mod_tap_candidate = false;
    if (slot < 0) return;
    purp_touch_point_t *old = purp_seat_touch_point(s, slot);
    if (old && old->active) purp_host_touch_cancel(s, slot);
    remove_touch(s, slot);
    purp_pointf_t pos = {x, y};
    s->wm.pointer_location = pos;
    if (purp_server_is_locked(s)) { swallow_touch(s, slot); return; }
    if (s->help.visible) {
        s->help.visible = false;
        swallow_touch(s, slot);
        purp_input_sync_pointer_focus(s, time);
        purp_host_damage_all(s);
        return;
    }
    if (purp_seat_drag_active(s)) { swallow_touch(s, slot); return; }
    if (dismiss_notification(s, pos)) {
        swallow_touch(s, slot);
        purp_input_sync_pointer_focus(s, time);
        return;
    }
    dismiss_outside_popups(s, pos);
    if (purp_xdg_popup_at(s, pos)) { forward_touch_down(s, time, slot, pos); return; }
    purp_pointf_t origin;
    purp_layer_surface_t *layer = purp_layer_at(s, pos, &origin);
    if (layer) {
        focus_layer(s, layer);
        forward_touch_down(s, time, slot, pos);
        return;
    }
    size_t mon;
    int32_t local_x;
    if (purp_wm_bar_hit_monitor(&s->wm, pos, &mon, &local_x)) {
        swallow_touch(s, slot);
        purp_input_handle_bar_click(s, mon, local_x, PURP_BTN_LEFT);
        return;
    }
    focus_clicked_window(s, pos);
    if (purp_mods_has_mod(s->seat.modifiers) && s->grab.kind == PURP_GRAB_NONE) {
        int idx = purp_wm_window_at(&s->wm, pos);
        if (idx >= 0) {
            swallow_touch(s, slot);
            begin_grab(s, (size_t)idx, pos, false, true, 0, false, slot);
            return;
        }
    }
    forward_touch_down(s, time, slot, pos);
    purp_input_sync_keyboard_focus(s);
}

void purp_host_touch_motion(purp_server_t *s, uint32_t time, int32_t slot, double x, double y) {
    purp_pointf_t pos = {x, y};
    purp_touch_point_t *point = purp_seat_touch_point(s, slot);
    if (point) point->pos = pos;
    s->wm.pointer_location = pos;
    if (purp_server_is_locked(s) || s->help.visible) {
        if (purp_seat_drag_active(s)) purp_seat_drag_cancel(s);
        return;
    }
    if (s->grab.kind != PURP_GRAB_NONE && s->grab.touch_slot == slot && update_grab(s, pos)) return;
    if (touch_swallowed(s, slot)) return;
    if (purp_seat_drag_active(s)) {
        if (s->seat.drag.is_touch && s->seat.drag.touch_slot == slot) purp_seat_drag_motion(s, time);
        return;
    }
    purp_seat_touch_send_motion(s, time, slot, pos);
    purp_seat_touch_send_frame(s);
    purp_host_damage_all(s);
}

void purp_host_touch_up(purp_server_t *s, uint32_t time, int32_t slot) {
    bool swallowed = remove_touch(s, slot);
    bool forward = !swallowed;
    if (s->grab.kind != PURP_GRAB_NONE && s->grab.touch_slot == slot) {
        forward = s->grab.forward_release;
        s->grab = (purp_interactive_grab_t){.touch_slot = -1};
        s->touch_grab_slot = -1;
        purp_server_relayout(s);
    }
    if (purp_server_is_locked(s) || s->help.visible) return;
    if (purp_seat_drag_active(s) && s->seat.drag.is_touch && s->seat.drag.touch_slot == slot)
        purp_seat_drag_drop(s, time);
    if (forward) {
        purp_seat_touch_send_up(s, time, slot);
        purp_seat_touch_send_frame(s);
    }
}

void purp_host_touch_cancel(purp_server_t *s, int32_t slot) {
    remove_touch(s, slot);
    if (s->grab.kind != PURP_GRAB_NONE && s->grab.touch_slot == slot) {
        s->grab = (purp_interactive_grab_t){.touch_slot = -1};
        s->touch_grab_slot = -1;
        purp_server_relayout(s);
    }
    if (purp_seat_drag_active(s) && s->seat.drag.is_touch && s->seat.drag.touch_slot == slot)
        purp_seat_drag_cancel(s);
    purp_seat_touch_send_cancel(s, slot);
}

void purp_host_touch_frame(purp_server_t *s) {
    if (!purp_server_is_locked(s) && !s->help.visible) purp_seat_touch_send_frame(s);
}

void purp_host_session_active(purp_server_t *s, bool active) {
    if (active) {
        purp_input_sync_keyboard_focus(s);
        purp_input_sync_pointer_focus(s, 0);
        purp_host_damage_all(s);
        return;
    }
    purp_input_cancel_grabs(s);
    uint32_t now = purp_server_time_ms(s);
    for (size_t n = s->seat.pressed_key_count; n > 0; --n) {
        uint32_t code = s->seat.pressed_keys[n - 1];
        purp_seat_update_xkb(s, code, false);
        purp_seat_keyboard_send_key(s, now, code, false);
    }
    s->seat.pressed_key_count = 0;
    for (size_t i = 0; i < s->intercepted_key_count; ++i)
        purp_seat_update_xkb(s, s->intercepted_keys[i], false);
    xkb_state_update_mask(s->seat.xkb_state, 0, 0, s->seat.mods_locked, 0, 0, s->seat.group);
    s->seat.mods_depressed = 0;
    s->seat.mods_latched = 0;
    s->seat.modifiers &= PURP_MOD_CAPS | PURP_MOD_NUM;
    purp_seat_keyboard_send_modifiers(s);
    for (size_t n = s->seat.pressed_button_count; n > 0; --n) {
        uint32_t button = s->seat.pressed_buttons[n - 1];
        purp_seat_pointer_send_button(s, now, button, false);
    }
    s->seat.pressed_button_count = 0;
    purp_seat_pointer_send_frame(s);
    for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i)
        if (s->seat.touch[i].active) purp_seat_touch_send_cancel(s, s->seat.touch[i].slot);
    s->intercepted_key_count = 0;
    s->swallowed_button_count = 0;
    s->swallowed_touch_count = 0;
    s->mod_tap_candidate = false;
    s->bar_scroll_accum = 0;
    purp_seat_set_keyboard_focus(s, NULL);
    purp_seat_pointer_focus(s, NULL, (purp_pointf_t){0}, now);
}
