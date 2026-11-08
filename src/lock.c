#include "ui.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct purp_lock_attempt {
    atomic_size_t refs;
    pthread_mutex_t lock;
    bool done;
    bool unlocked;
    char error[PURP_LOCK_MESSAGE_MAX];
    char password[PURP_LOCK_PASSWORD_MAX + 1];
};

atomic_size_t purp_pam_conversations = 0;

static void copy_lock_string(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    size_t len = strlen(src);
    if (len >= dst_len) len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool current_username(char out[256]) {
    out[0] = '\0';
    long suggestion = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t size = suggestion > 0 ? (size_t)suggestion : 1024;
    if (size < 256) size = 256;
    if (size > 65536) size = 65536;
    char *storage = malloc(size);
    if (!storage) return false;
    bool found = false;
    for (;;) {
        struct passwd pwd;
        struct passwd *result = NULL;
        int status = getpwuid_r(getuid(), &pwd, storage, size, &result);
        if (status == 0) {
            if (result && pwd.pw_name) {
                copy_lock_string(out, 256, pwd.pw_name);
                found = *out != '\0';
            }
            break;
        }
        if (status != ERANGE || size >= 65536) break;
        size_t next = size * 2;
        if (next > 65536) next = 65536;
        char *grown = realloc(storage, next);
        if (!grown) break;
        storage = grown;
        size = next;
    }
    free(storage);
    return found;
}

bool purp_lock_key_from_keysym(uint32_t keysym, uint32_t utf32, uint32_t mods,
                               purp_lock_key_t *out) {
    if (!out) return false;
    if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
        *out = (purp_lock_key_t){ .kind = PURP_LOCK_KEY_SUBMIT };
        return true;
    }
    if (keysym == XKB_KEY_BackSpace) {
        *out = (purp_lock_key_t){ .kind = PURP_LOCK_KEY_BACKSPACE };
        return true;
    }
    if (keysym == XKB_KEY_Escape ||
        ((mods & PURP_MOD_CTRL) != 0 && (keysym == XKB_KEY_u || keysym == XKB_KEY_U))) {
        *out = (purp_lock_key_t){ .kind = PURP_LOCK_KEY_CLEAR };
        return true;
    }
    if ((mods & (PURP_MOD_CTRL | PURP_MOD_ALT | PURP_MOD_LOGO)) != 0) return false;
    if (utf32 < 0x20u || (utf32 >= 0x7fu && utf32 <= 0x9fu) || utf32 > 0x10ffffu ||
        (utf32 >= 0xd800u && utf32 <= 0xdfffu))
        return false;
    char encoded[4];
    if (purp_utf8_encode(utf32, encoded) == 0) return false;
    *out = (purp_lock_key_t){ .kind = PURP_LOCK_KEY_CHAR, .codepoint = utf32 };
    return true;
}

void purp_lock_init(purp_lock_screen_t *lock) {
    if (!lock) return;
    memset(lock, 0, sizeof(*lock));
    current_username(lock->username);
}

static void attempt_unref(purp_lock_attempt_t *attempt) {
    if (!attempt) return;
    if (atomic_fetch_sub_explicit(&attempt->refs, 1, memory_order_acq_rel) != 1) return;
    explicit_bzero(attempt->password, sizeof(attempt->password));
    explicit_bzero(attempt->error, sizeof(attempt->error));
    pthread_mutex_destroy(&attempt->lock);
    free(attempt);
}

static void lock_cache_clear(purp_lock_screen_t *lock) {
    for (size_t i = 0; i < PURP_LOCK_CACHE_ENTRIES; ++i) {
        purp_pixel_buf_free(&lock->cache[i].buffer);
        memset(&lock->cache[i], 0, sizeof(lock->cache[i]));
    }
}

void purp_lock_finish(purp_lock_screen_t *lock) {
    if (!lock) return;
    explicit_bzero(lock->password, sizeof(lock->password));
    lock->password_bytes = 0;
    purp_lock_attempt_t *attempt = lock->attempt;
    lock->attempt = NULL;
    lock->checking = false;
    attempt_unref(attempt);
    lock_cache_clear(lock);
    memset(lock, 0, sizeof(*lock));
}

typedef struct pam_conversation_state {
    const char *password;
    char *err;
    size_t err_len;
} pam_conversation_state_t;

static void free_pam_replies(struct pam_response *replies, size_t count) {
    if (!replies) return;
    for (size_t i = 0; i < count; ++i) {
        if (replies[i].resp) {
            size_t len = strlen(replies[i].resp);
            explicit_bzero(replies[i].resp, len + 1);
            free(replies[i].resp);
        }
    }
    free(replies);
}

static int pam_conversation(int num_msg, const struct pam_message **msg,
                            struct pam_response **resp, void *appdata_ptr) {
    if (num_msg <= 0 || !msg || !resp || !appdata_ptr) return PAM_CONV_ERR;
    *resp = NULL;
    pam_conversation_state_t *state = appdata_ptr;
    struct pam_response *replies = calloc((size_t)num_msg, sizeof(*replies));
    if (!replies) return PAM_BUF_ERR;
    for (size_t i = 0; i < (size_t)num_msg; ++i) {
        if (!msg[i]) {
            free_pam_replies(replies, (size_t)num_msg);
            return PAM_BUF_ERR;
        }
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            replies[i].resp = strdup(state->password);
            if (!replies[i].resp) {
                free_pam_replies(replies, (size_t)num_msg);
                return PAM_BUF_ERR;
            }
            break;
        case PAM_ERROR_MSG:
            if (msg[i]->msg && state->err && state->err_len > 0)
                copy_lock_string(state->err, state->err_len, msg[i]->msg);
            break;
        case PAM_TEXT_INFO:
            break;
        default:
            free_pam_replies(replies, (size_t)num_msg);
            return PAM_CONV_ERR;
        }
        atomic_fetch_add_explicit(&purp_pam_conversations, 1, memory_order_relaxed);
    }
    *resp = replies;
    return PAM_SUCCESS;
}

bool purp_pam_check(const char *service, const char *user, const char *password,
                    const char *confdir, char *err, size_t err_len) {
    if (err && err_len > 0) err[0] = '\0';
    if (!service || !*service || !user || !*user || !password) return false;
    pam_conversation_state_t state = { .password = password, .err = err, .err_len = err_len };
    struct pam_conv conversation = { .conv = pam_conversation, .appdata_ptr = &state };
    pam_handle_t *handle = NULL;
    int status;
    if (confdir) status = pam_start_confdir(service, user, &conversation, confdir, &handle);
    else status = pam_start(service, user, &conversation, &handle);
    if (status != PAM_SUCCESS || !handle) {
        if (handle) pam_end(handle, status);
        return false;
    }
    status = pam_authenticate(handle, 0);
    pam_end(handle, status);
    return status == PAM_SUCCESS;
}

static bool pam_service_exists(const char *dir, const char *service) {
    size_t dir_len = strlen(dir), service_len = strlen(service);
    if (dir_len + service_len + 2 > PATH_MAX) return false;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, service);
    return access(path, F_OK) == 0;
}

bool purp_pam_authenticate_current_user(const char *password, char *err, size_t err_len) {
    char username[256];
    if (!password || !current_username(username)) {
        if (err && err_len > 0) err[0] = '\0';
        return false;
    }
    const char *confdir = getenv(PURP_ENV_PAM_CONFDIR);
    if (confdir && !*confdir) confdir = NULL;
    const char *dir = confdir ? confdir : PURP_PAM_DEFAULT_CONFDIR;
    const char *service = getenv(PURP_ENV_PAM_SERVICE);
    if (service && !*service) service = NULL;
    if (!service) {
        if (pam_service_exists(dir, PURP_PAM_SERVICE_PRIMARY)) service = PURP_PAM_SERVICE_PRIMARY;
        else if (pam_service_exists(dir, PURP_PAM_SERVICE_LEGACY)) service = PURP_PAM_SERVICE_LEGACY;
        else if (pam_service_exists(dir, PURP_PAM_SERVICE_VLOCK)) service = PURP_PAM_SERVICE_VLOCK;
        else service = PURP_PAM_SERVICE_LOGIN;
    }
    bool ok = purp_pam_check(service, username, password, confdir, err, err_len);
    explicit_bzero(username, sizeof(username));
    return ok;
}

static void *pam_worker(void *userdata) {
    purp_lock_attempt_t *attempt = userdata;
    char error[PURP_LOCK_MESSAGE_MAX] = {0};
    bool unlocked = purp_pam_authenticate_current_user(attempt->password, error, sizeof(error));
    pthread_mutex_lock(&attempt->lock);
    attempt->unlocked = unlocked;
    copy_lock_string(attempt->error, sizeof(attempt->error), error);
    attempt->done = true;
    pthread_mutex_unlock(&attempt->lock);
    explicit_bzero(error, sizeof(error));
    attempt_unref(attempt);
    return NULL;
}

static void wipe_password(purp_lock_screen_t *lock) {
    explicit_bzero(lock->password, sizeof(lock->password));
    lock->password_bytes = 0;
}

void purp_lock_handle_key(purp_lock_screen_t *lock, purp_lock_key_t key) {
    if (!lock || lock->checking) return;
    switch (key.kind) {
    case PURP_LOCK_KEY_CHAR: {
        lock->message[0] = '\0';
        char encoded[4];
        size_t bytes = purp_utf8_encode(key.codepoint, encoded);
        if (bytes == 0 || lock->password_bytes + bytes > PURP_LOCK_PASSWORD_MAX) return;
        memcpy(lock->password + lock->password_bytes, encoded, bytes);
        lock->password_bytes += bytes;
        lock->password[lock->password_bytes] = '\0';
        break;
    }
    case PURP_LOCK_KEY_BACKSPACE: {
        size_t removed = purp_utf8_pop_wipe(lock->password);
        if (removed <= lock->password_bytes) lock->password_bytes -= removed;
        else lock->password_bytes = 0;
        break;
    }
    case PURP_LOCK_KEY_CLEAR:
        wipe_password(lock);
        break;
    case PURP_LOCK_KEY_SUBMIT: {
        if (lock->password_bytes == 0) return;
        purp_lock_attempt_t *attempt = calloc(1, sizeof(*attempt));
        if (!attempt) {
            wipe_password(lock);
            copy_lock_string(lock->message, sizeof(lock->message), PURP_LOCK_MSG_ERROR);
            return;
        }
        atomic_init(&attempt->refs, 1);
        if (pthread_mutex_init(&attempt->lock, NULL) != 0) {
            free(attempt);
            wipe_password(lock);
            copy_lock_string(lock->message, sizeof(lock->message), PURP_LOCK_MSG_ERROR);
            return;
        }
        memcpy(attempt->password, lock->password, lock->password_bytes + 1);
        wipe_password(lock);
        lock->message[0] = '\0';
        lock->checking = true;
        lock->attempt = attempt;
        lock->attempt_started_ms = purp_now_ms();
        atomic_fetch_add_explicit(&attempt->refs, 1, memory_order_relaxed);
        pthread_attr_t attr;
        int attr_status = pthread_attr_init(&attr);
        if (attr_status == 0) {
            attr_status = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (attr_status != 0) pthread_attr_destroy(&attr);
        }
        if (attr_status != 0) {
            lock->attempt = NULL;
            lock->checking = false;
            attempt_unref(attempt);
            attempt_unref(attempt);
            copy_lock_string(lock->message, sizeof(lock->message), PURP_LOCK_MSG_ERROR);
            return;
        }
        pthread_t thread;
        int status = pthread_create(&thread, &attr, pam_worker, attempt);
        pthread_attr_destroy(&attr);
        if (status != 0) {
            lock->attempt = NULL;
            lock->checking = false;
            attempt_unref(attempt);
            attempt_unref(attempt);
            copy_lock_string(lock->message, sizeof(lock->message), PURP_LOCK_MSG_ERROR);
        }
        break;
    }
    }
}

static bool string_has_nonspace(const char *s) {
    for (; *s; ++s) if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') return true;
    return false;
}

purp_lock_poll_t purp_lock_poll(purp_lock_screen_t *lock) {
    if (!lock || !lock->attempt) return PURP_LOCK_POLL_PENDING;
    purp_lock_attempt_t *attempt = lock->attempt;
    bool done, unlocked;
    char error[PURP_LOCK_MESSAGE_MAX];
    pthread_mutex_lock(&attempt->lock);
    done = attempt->done;
    unlocked = attempt->unlocked;
    copy_lock_string(error, sizeof(error), attempt->error);
    pthread_mutex_unlock(&attempt->lock);
    if (done) {
        lock->attempt = NULL;
        lock->checking = false;
        attempt_unref(attempt);
        if (unlocked) {
            wipe_password(lock);
            lock->message[0] = '\0';
            explicit_bzero(error, sizeof(error));
            return PURP_LOCK_POLL_UNLOCKED;
        }
        copy_lock_string(lock->message, sizeof(lock->message),
                         string_has_nonspace(error) ? error : PURP_LOCK_MSG_WRONG);
        explicit_bzero(error, sizeof(error));
        return PURP_LOCK_POLL_FAILED;
    }
    if (purp_now_ms() - lock->attempt_started_ms >= PURP_LOCK_TIMEOUT_MS) {
        lock->attempt = NULL;
        lock->checking = false;
        attempt_unref(attempt);
        wipe_password(lock);
        copy_lock_string(lock->message, sizeof(lock->message), PURP_LOCK_MSG_TIMEOUT);
        explicit_bzero(error, sizeof(error));
        return PURP_LOCK_POLL_FAILED;
    }
    explicit_bzero(error, sizeof(error));
    return PURP_LOCK_POLL_PENDING;
}

bool purp_lock_is_checking(const purp_lock_screen_t *lock) {
    return lock && lock->checking;
}

size_t purp_lock_password_len(const purp_lock_screen_t *lock) {
    return lock ? purp_utf8_count(lock->password) : 0;
}

const char *purp_lock_message(const purp_lock_screen_t *lock) {
    return lock && lock->message[0] ? lock->message : NULL;
}

bool purp_lock_needs_clock_redraw(purp_lock_screen_t *lock) {
    if (!lock) return false;
    char clock[32];
    purp_format_local_time(clock, sizeof(clock));
    if (strcmp(lock->last_clock, clock) == 0) return false;
    copy_lock_string(lock->last_clock, sizeof(lock->last_clock), clock);
    return true;
}

static int32_t lock_px(double value, double scale) {
    return purp_round_i32(value * scale);
}

static void draw_lock_top(purp_font_t *font, purp_pixel_buf_t *buffer, const char *text, int32_t x,
                          int32_t top, float px, purp_rgba_t color) {
    purp_font_draw(font, buffer, text, x, top + (int32_t)(px * 0.8f), px, color);
}

static int32_t centered_x(purp_font_t *font, const char *text, float px, int32_t width) {
    return width / 2 - purp_font_measure(font, text, px) / 2;
}

static void lock_redraw(purp_lock_screen_t *lock, purp_font_t *font,
                        purp_lock_cache_entry_t *entry, const char *clock, double scale) {
    purp_pixel_buf_t *buffer = &entry->buffer;
    int32_t w = entry->width, h = entry->height;
    char date[32], time_text[32];
    copy_lock_string(date, sizeof(date), clock);
    char *split = strrchr(date, ' ');
    if (split) {
        *split = '\0';
        copy_lock_string(time_text, sizeof(time_text), split + 1);
    } else {
        copy_lock_string(time_text, sizeof(time_text), clock);
        date[0] = '\0';
    }

    char dots[PURP_LOCK_MAX_DOTS * 3 + 1];
    size_t dot_count = purp_min_i32((int32_t)purp_lock_password_len(lock), PURP_LOCK_MAX_DOTS);
    size_t dot_bytes = 0;
    for (size_t i = 0; i < (size_t)dot_count; ++i) {
        memcpy(dots + dot_bytes, "\xE2\x80\xA2", 3);
        dot_bytes += 3;
    }
    dots[dot_bytes] = '\0';

    const char *status;
    purp_rgba_t status_color;
    if (lock->checking) {
        status = "Checking\xE2\x80\xA6";
        status_color = PURP_BAR_TEXT_DIM_RGBA;
    } else if (lock->message[0]) {
        status = lock->message;
        status_color = PURP_LOCK_ERROR_RGBA;
    } else {
        status = "Type your password and press Enter";
        status_color = PURP_BAR_TEXT_DIM_RGBA;
    }
    char user_line[320];
    snprintf(user_line, sizeof(user_line), "Locked \xC2\xB7 %s", lock->username);

    purp_pixel_buf_clear(buffer, PURP_LOCK_BG_RGBA);
    float time_px = 120.0f * (float)scale;
    float max_time_width = (float)w * 0.9f;
    float measured = (float)purp_font_measure(font, time_text, time_px);
    if (measured > max_time_width && measured > 0.0f && max_time_width > 0.0f)
        time_px = (time_px * max_time_width / measured) > 1.0f ? time_px * max_time_width / measured : 1.0f;
    int32_t y = h / 2 - lock_px(150.0, scale);
    draw_lock_top(font, buffer, time_text, centered_x(font, time_text, time_px, w), y, time_px,
                  PURP_BAR_TEXT_FG_RGBA);
    y += lock_px(150.0, scale);
    float small = 20.0f * (float)scale;
    draw_lock_top(font, buffer, date, centered_x(font, date, small, w), y - lock_px(10.0, scale),
                  small, PURP_BAR_ACCENT_RGBA);

    int32_t field_margin = purp_max_i32(lock_px(8.0, scale), 4);
    int32_t field_w = purp_min_i32(lock_px(360.0, scale), purp_max_i32(w - 2 * field_margin, 1));
    int32_t field_h = lock_px(48.0, scale);
    int32_t field_x = w / 2 - field_w / 2;
    int32_t field_y = y + lock_px(30.0, scale);
    purp_fill_rect(buffer, field_x - lock_px(2.0, scale), field_y - lock_px(2.0, scale),
                   field_w + lock_px(4.0, scale), field_h + lock_px(4.0, scale), PURP_BAR_ACCENT_RGBA);
    purp_fill_rect(buffer, field_x, field_y, field_w, field_h, PURP_BAR_LAYOUT_BG_RGBA);
    float dots_px = 24.0f * (float)scale;
    int32_t dots_y = field_y + (field_h - (int32_t)dots_px) / 2;
    draw_lock_top(font, buffer, dots, centered_x(font, dots, dots_px, w), dots_y, dots_px,
                  PURP_BAR_TEXT_FG_RGBA);

    int32_t status_y = field_y + field_h + lock_px(24.0, scale);
    draw_lock_top(font, buffer, status, centered_x(font, status, small, w), status_y, small, status_color);
    draw_lock_top(font, buffer, user_line, centered_x(font, user_line, small, w),
                  status_y + lock_px(40.0, scale), small, PURP_BAR_TEXT_DIM_RGBA);
    purp_pixel_buf_touch(buffer);
}

const purp_pixel_buf_t *purp_lock_render(purp_lock_screen_t *lock, purp_font_t *font,
                                         int32_t width, int32_t height, double scale) {
    if (!lock) return NULL;
    width = purp_max_i32(width, 1);
    height = purp_max_i32(height, 1);
    if (!isfinite(scale) || scale <= 0) scale = 1.0;
    char clock[32];
    purp_format_local_time(clock, sizeof(clock));
    copy_lock_string(lock->last_clock, sizeof(lock->last_clock), clock);
    size_t dots = purp_lock_password_len(lock);
    if (dots > PURP_LOCK_MAX_DOTS) dots = PURP_LOCK_MAX_DOTS;

    purp_lock_cache_entry_t *entry = NULL, *free_entry = NULL;
    size_t used = 0;
    for (size_t i = 0; i < PURP_LOCK_CACHE_ENTRIES; ++i) {
        purp_lock_cache_entry_t *candidate = &lock->cache[i];
        if (!candidate->in_use) {
            if (!free_entry) free_entry = candidate;
            continue;
        }
        used++;
        if (candidate->width == width && candidate->height == height && candidate->scale == scale) {
            entry = candidate;
            break;
        }
    }
    if (!entry) {
        if (used >= PURP_LOCK_CACHE_ENTRIES) {
            lock_cache_clear(lock);
            free_entry = &lock->cache[0];
        }
        entry = free_entry;
    }
    if (!entry) return NULL;
    if (entry->in_use && entry->dots == dots && entry->checking == lock->checking &&
        strcmp(entry->message, lock->message) == 0 && strcmp(entry->clock, clock) == 0)
        return &entry->buffer;
    if (!purp_pixel_buf_ensure(&entry->buffer, width, height)) return NULL;
    entry->in_use = true;
    entry->width = width;
    entry->height = height;
    entry->scale = scale;
    entry->dots = dots;
    entry->checking = lock->checking;
    copy_lock_string(entry->message, sizeof(entry->message), lock->message);
    copy_lock_string(entry->clock, sizeof(entry->clock), clock);
    lock_redraw(lock, font, entry, clock, scale);
    return &entry->buffer;
}
