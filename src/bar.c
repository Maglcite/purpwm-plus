#include "ui.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BAR_COMMAND_OUTPUT_LIMIT (1024u * 1024u)

static int32_t bar_px(int32_t value, double scale) {
    return purp_round_i32((double)value * scale);
}

static void copy_string(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    size_t len = strlen(src);
    if (len >= dst_len) len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void purp_format_local_time(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm)) {
        copy_string(out, out_len, "Jan 01 00:00");
        return;
    }
    const char *month = tm.tm_mon >= 0 && tm.tm_mon < 12 ? months[tm.tm_mon] : "Oct";
    snprintf(out, out_len, "%s %02d %02d:%02d", month, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static bool split_nmcli_line(char *line, char **state, char **connection) {
    char *first = strchr(line, ':');
    if (!first) return false;
    *first++ = '\0';
    char *second = strchr(first, ':');
    if (second) {
        *second++ = '\0';
        if (connection) *connection = second;
    } else if (connection) {
        *connection = NULL;
    }
    *state = first;
    return strcmp(line, "wifi") == 0;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

void purp_bar_wifi_label_from_nmcli(const char *text, char *label, size_t label_len,
                                    bool *connected) {
    if (connected) *connected = false;
    if (!label || label_len == 0) return;
    copy_string(label, label_len, "WIFI: N/A");
    if (!text) return;
    char *copy = strdup(text);
    if (!copy) return;
    bool have_fallback = false;
    char fallback[16] = "";
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *state = NULL, *connection = NULL;
        if (!split_nmcli_line(line, &state, &connection)) continue;
        state = trim(state);
        if (connection) connection = trim(connection);
        if (strcmp(state, "connected") == 0 && connection && *connection) {
            char ssid[15 * 4 + 1];
            purp_utf8_truncate(ssid, sizeof(ssid), connection, 14);
            snprintf(label, label_len, "WIFI: %s", ssid);
            if (connected) *connected = true;
            free(copy);
            return;
        }
        if (strncmp(state, "connecting", 10) == 0) {
            copy_string(fallback, sizeof(fallback), "...");
            have_fallback = true;
        } else if (!have_fallback) {
            copy_string(fallback, sizeof(fallback), "Disconn");
            have_fallback = true;
        }
    }
    if (have_fallback) snprintf(label, label_len, "WIFI: %s", fallback);
    free(copy);
}

bool purp_wifi_needs_autoconnect(const char *text) {
    if (!text) return false;
    char *copy = strdup(text);
    if (!copy) return false;
    bool disconnected = false, active = false;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *state = NULL;
        if (!split_nmcli_line(line, &state, NULL)) continue;
        state = trim(state);
        if (strcmp(state, "connected") == 0 || strncmp(state, "connecting", 10) == 0) active = true;
        if (strcmp(state, "disconnected") == 0) disconnected = true;
    }
    free(copy);
    return disconnected && !active;
}

void purp_bar_volume_label_from_wpctl(const char *text, char *label, size_t label_len,
                                      bool *unmuted) {
    if (unmuted) *unmuted = false;
    if (!label || label_len == 0) return;
    copy_string(label, label_len, "VOL N/A");
    if (!text) return;
    const char *p = text;
    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return;
    char *end = NULL;
    errno = 0;
    double level = strtod(p, &end);
    if (end == p || errno == ERANGE || !isfinite(level)) return;
    const char *token_end = end;
    if (*token_end && !isspace((unsigned char)*token_end)) return;
    long long percent = (long long)llround(level * 100.0);
    bool muted = strstr(text, "[MUTED]") != NULL;
    if (muted) snprintf(label, label_len, "VOL Mute %lld%%", percent);
    else {
        snprintf(label, label_len, "VOL %lld%%", percent);
        if (unmuted) *unmuted = true;
    }
}

static bool read_small_file(const char *path, char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    FILE *file = fopen(path, "r");
    if (!file) return false;
    size_t n = fread(out, 1, out_len - 1, file);
    bool ok = !ferror(file);
    fclose(file);
    out[n] = '\0';
    return ok;
}

static bool rfkill_soft_blocked(const char *kind) {
    DIR *dir = opendir("/sys/class/rfkill");
    if (!dir) return false;
    bool blocked = false;
    struct dirent *entry;
    while (!blocked && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[PATH_MAX], value[64];
        int n = snprintf(path, sizeof(path), "/sys/class/rfkill/%s/type", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path) || !read_small_file(path, value, sizeof(value))) continue;
        char *type = trim(value);
        if (strcmp(type, kind) != 0) continue;
        n = snprintf(path, sizeof(path), "/sys/class/rfkill/%s/soft", entry->d_name);
        if (n >= 0 && (size_t)n < sizeof(path) && read_small_file(path, value, sizeof(value)) &&
            strcmp(trim(value), "1") == 0)
            blocked = true;
    }
    closedir(dir);
    return blocked;
}

static bool command_output(const char *program, const char *const args[], char **output) {
    *output = NULL;
    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0) return false;
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        return false;
    }
    if (pid == 0) {
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            if (nullfd > STDERR_FILENO) close(nullfd);
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        char *argv[16];
        size_t argc = 0;
        argv[argc++] = (char *)program;
        for (size_t i = 0; args && args[i] && argc + 1 < PURP_ARRAY_LEN(argv); ++i)
            argv[argc++] = (char *)args[i];
        argv[argc] = NULL;
        execvp(program, argv);
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL, 0) | O_NONBLOCK);

    char *bytes = NULL;
    size_t len = 0, capacity = 0;
    bool allocation_ok = true, child_done = false, timed_out = false;
    int out_fd = out_pipe[0], err_fd = err_pipe[0];
    uint64_t deadline = purp_now_ms() + PURP_BAR_COMMAND_TIMEOUT_MS;
    for (;;) {
        pid_t waited;
        do waited = waitpid(pid, NULL, WNOHANG); while (waited < 0 && errno == EINTR);
        if (waited == pid) child_done = true;
        if (child_done && out_fd < 0 && err_fd < 0) break;
        uint64_t now = purp_now_ms();
        if (now >= deadline) {
            timed_out = true;
            if (!child_done) kill(pid, SIGKILL);
            if (!child_done) {
                do waited = waitpid(pid, NULL, 0); while (waited < 0 && errno == EINTR);
            }
            break;
        }

        struct pollfd fds[2];
        nfds_t count = 0;
        if (out_fd >= 0) {
            fds[count].fd = out_fd;
            fds[count].events = POLLIN | POLLHUP;
            fds[count].revents = 0;
            count++;
        }
        if (err_fd >= 0) {
            fds[count].fd = err_fd;
            fds[count].events = POLLIN | POLLHUP;
            fds[count].revents = 0;
            count++;
        }
        int timeout = (int)purp_min_i32(50, (int32_t)(deadline - now));
        int polled;
        do polled = poll(fds, count, timeout); while (polled < 0 && errno == EINTR);
        if (polled <= 0) continue;
        for (nfds_t i = 0; i < count; ++i) {
            if (!fds[i].revents) continue;
            bool is_out = fds[i].fd == out_fd;
            for (;;) {
                char chunk[4096];
                ssize_t got = read(fds[i].fd, chunk, sizeof(chunk));
                if (got > 0) {
                    if (is_out && allocation_ok && len < BAR_COMMAND_OUTPUT_LIMIT) {
                        size_t take = (size_t)got;
                        if (take > BAR_COMMAND_OUTPUT_LIMIT - len) take = BAR_COMMAND_OUTPUT_LIMIT - len;
                        if (len + take + 1 > capacity) {
                            size_t next = capacity ? capacity * 2 : 4096;
                            while (next < len + take + 1 && next <= BAR_COMMAND_OUTPUT_LIMIT + 1) next *= 2;
                            if (next > BAR_COMMAND_OUTPUT_LIMIT + 1) next = BAR_COMMAND_OUTPUT_LIMIT + 1;
                            char *grown = realloc(bytes, next);
                            if (!grown) allocation_ok = false;
                            else {
                                bytes = grown;
                                capacity = next;
                            }
                        }
                        if (allocation_ok) {
                            memcpy(bytes + len, chunk, take);
                            len += take;
                        }
                    }
                    continue;
                }
                if (got == 0) {
                    close(fds[i].fd);
                    if (is_out) out_fd = -1;
                    else err_fd = -1;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    close(fds[i].fd);
                    if (is_out) out_fd = -1;
                    else err_fd = -1;
                }
                break;
            }
        }
    }
    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);
    if (timed_out || !allocation_ok || !child_done) {
        free(bytes);
        return false;
    }
    if (!bytes) {
        bytes = malloc(1);
        if (!bytes) return false;
    } else if (len + 1 > capacity) {
        char *grown = realloc(bytes, len + 1);
        if (!grown) {
            free(bytes);
            return false;
        }
        bytes = grown;
    }
    bytes[len] = '\0';
    *output = bytes;
    return true;
}

static pthread_mutex_t cpu_lock = PTHREAD_MUTEX_INITIALIZER;
static bool cpu_have_previous;
static uint64_t cpu_previous_idle, cpu_previous_total;

static bool cpu_usage_percent(unsigned int *percent) {
    FILE *file = fopen("/proc/stat", "r");
    if (!file) return false;
    char line[1024];
    bool ok = fgets(line, sizeof(line), file) != NULL;
    fclose(file);
    if (!ok || strncmp(line, "cpu", 3) != 0) return false;
    char *p = line + 3;
    uint64_t values[8] = {0};
    size_t count = 0;
    while (count < PURP_ARRAY_LEN(values)) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || !isdigit((unsigned char)*p)) break;
        char *end;
        errno = 0;
        unsigned long long value = strtoull(p, &end, 10);
        if (end == p || errno == ERANGE) break;
        values[count++] = (uint64_t)value;
        p = end;
    }
    if (count < 4) return false;
    uint64_t idle = values[3] + (count > 4 ? values[4] : 0);
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) total += values[i];
    bool have = false;
    unsigned int result = 0;
    pthread_mutex_lock(&cpu_lock);
    if (cpu_have_previous && total >= cpu_previous_total && idle >= cpu_previous_idle) {
        uint64_t total_delta = total - cpu_previous_total;
        uint64_t idle_delta = idle - cpu_previous_idle;
        if (total_delta > 0) {
            if (idle_delta > total_delta) idle_delta = total_delta;
            result = (unsigned int)((total_delta - idle_delta) * 100 / total_delta);
            have = true;
        }
    }
    cpu_previous_idle = idle;
    cpu_previous_total = total;
    cpu_have_previous = true;
    pthread_mutex_unlock(&cpu_lock);
    if (have) *percent = result;
    return have;
}

static bool memory_used_label(char *out, size_t out_len) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) return false;
    char line[256];
    uint64_t total = 0, available = 0;
    while (fgets(line, sizeof(line), file)) {
        uint64_t *target = NULL;
        char *p = NULL;
        if (strncmp(line, "MemTotal:", 9) == 0) {
            target = &total;
            p = line + 9;
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            target = &available;
            p = line + 13;
        }
        if (target) {
            while (*p && isspace((unsigned char)*p)) p++;
            *target = strtoull(p, NULL, 10);
        }
    }
    fclose(file);
    if (total == 0) return false;
    double used = (double)(total - (available < total ? available : total)) / (1024.0 * 1024.0);
    snprintf(out, out_len, "%.1fG", used);
    return true;
}

static bool read_u64_file(const char *path, uint64_t *value) {
    char text[128];
    if (!read_small_file(path, text, sizeof(text))) return false;
    char *p = trim(text);
    if (!isdigit((unsigned char)*p)) return false;
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(p, &end, 10);
    if (end == p || errno == ERANGE) return false;
    *value = (uint64_t)parsed;
    return true;
}

static void battery_prefix(char *out, size_t out_len) {
    if (out_len == 0) return;
    out[0] = '\0';
    DIR *dir = opendir("/sys/class/power_supply");
    if (!dir) return;
    unsigned __int128 total_now = 0, total_full = 0, capacity_sum = 0, capacity_count = 0;
    bool charging = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char base[PATH_MAX - 16], path[PATH_MAX], text[128];
        int n = snprintf(base, sizeof(base), "/sys/class/power_supply/%s", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(base)) continue;
        snprintf(path, sizeof(path), "%s/type", base);
        bool battery = read_small_file(path, text, sizeof(text)) && strcasecmp(trim(text), "Battery") == 0;
        if (!battery && strncasecmp(entry->d_name, "bat", 3) != 0) continue;
        snprintf(path, sizeof(path), "%s/status", base);
        if (read_small_file(path, text, sizeof(text)) && strcasecmp(trim(text), "charging") == 0)
            charging = true;
        uint64_t now = 0, full = 0, capacity = 0;
        snprintf(path, sizeof(path), "%s/energy_now", base);
        bool have_now = read_u64_file(path, &now);
        snprintf(path, sizeof(path), "%s/energy_full", base);
        bool have_full = read_u64_file(path, &full);
        if (!have_now || !have_full) {
            snprintf(path, sizeof(path), "%s/charge_now", base);
            have_now = read_u64_file(path, &now);
            snprintf(path, sizeof(path), "%s/charge_full", base);
            have_full = read_u64_file(path, &full);
        }
        if (have_now && have_full && full > 0) {
            total_now += now;
            total_full += full;
        } else {
            snprintf(path, sizeof(path), "%s/capacity", base);
            if (read_u64_file(path, &capacity)) {
                capacity_sum += capacity;
                capacity_count++;
            }
        }
    }
    closedir(dir);
    uint64_t percent;
    if (total_full > 0) percent = (uint64_t)(total_now * 100 / total_full);
    else if (capacity_count > 0) percent = (uint64_t)(capacity_sum / capacity_count);
    else return;
    if (percent > 100) percent = 100;
    snprintf(out, out_len, "BAT %s%llu%%  |  ", charging ? "+" : "", (unsigned long long)percent);
}

static void collect_fast_metrics(char out[PURP_BAR_METRICS_MAX]) {
    unsigned int cpu;
    char cpu_label[16];
    if (cpu_usage_percent(&cpu)) snprintf(cpu_label, sizeof(cpu_label), "%2u%%", cpu);
    else copy_string(cpu_label, sizeof(cpu_label), "--%");
    char memory[32];
    if (!memory_used_label(memory, sizeof(memory))) copy_string(memory, sizeof(memory), "N/A");
    char battery[64], clock[32];
    battery_prefix(battery, sizeof(battery));
    purp_format_local_time(clock, sizeof(clock));
    snprintf(out, PURP_BAR_METRICS_MAX, "%sCPU %s  |  MEM %s  |  %s", battery, cpu_label, memory, clock);
}

static void query_volume(char *label, size_t label_len, bool *unmuted) {
    static const char *const args[] = { "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    char *out = NULL;
    if (!command_output("wpctl", args, &out)) {
        purp_bar_volume_label_from_wpctl(NULL, label, label_len, unmuted);
        return;
    }
    purp_bar_volume_label_from_wpctl(out, label, label_len, unmuted);
    free(out);
}

static void query_wifi(char *label, size_t label_len, bool *connected) {
    if (rfkill_soft_blocked("wlan")) {
        copy_string(label, label_len, "WIFI: Off");
        *connected = false;
        return;
    }
    static const char *const args[] = { "-t", "-f", "TYPE,STATE,CONNECTION", "device", "status", NULL };
    char *out = NULL;
    if (!command_output("nmcli", args, &out)) {
        purp_bar_wifi_label_from_nmcli(NULL, label, label_len, connected);
        return;
    }
    purp_bar_wifi_label_from_nmcli(out, label, label_len, connected);
    free(out);
}

static void query_bluetooth(char *label, size_t label_len, bool *active) {
    *active = false;
    if (rfkill_soft_blocked("bluetooth")) {
        copy_string(label, label_len, "BT: Off");
        return;
    }
    char *show = NULL;
    static const char *const show_args[] = { "show", NULL };
    if (!command_output("bluetoothctl", show_args, &show)) {
        copy_string(label, label_len, "BT: N/A");
        return;
    }
    bool powered = false;
    char *save = NULL;
    for (char *line = strtok_r(show, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (strcasecmp(trim(line), "Powered: yes") == 0) powered = true;
    free(show);
    if (!powered) {
        copy_string(label, label_len, "BT: Off");
        return;
    }
    *active = true;
    static const char *const devices_args[] = { "devices", "Connected", NULL };
    char *devices = NULL;
    if (command_output("bluetoothctl", devices_args, &devices)) {
        char *newline = strchr(devices, '\n');
        if (newline) *newline = '\0';
        char *first = strchr(devices, ' ');
        char *second = first ? strchr(first + 1, ' ') : NULL;
        if (second) {
            char name[12 * 4 + 1];
            purp_utf8_truncate(name, sizeof(name), trim(second + 1), 12);
            if (*name) {
                snprintf(label, label_len, "BT: %s", name);
                free(devices);
                return;
            }
        }
        free(devices);
    }
    copy_string(label, label_len, "BT: On");
}

static void refresh_fast(purp_status_snapshot_t *snapshot) {
    query_volume(snapshot->volume_label, sizeof(snapshot->volume_label), &snapshot->volume_unmuted);
    collect_fast_metrics(snapshot->sys_metrics);
}

static void refresh_radios(purp_status_snapshot_t *snapshot) {
    query_wifi(snapshot->wifi_label, sizeof(snapshot->wifi_label), &snapshot->wifi_connected);
    query_bluetooth(snapshot->bt_label, sizeof(snapshot->bt_label), &snapshot->bt_active);
}

static void publish_fast(purp_bar_t *bar, const purp_status_snapshot_t *snapshot) {
    copy_string(bar->shared.volume_label, sizeof(bar->shared.volume_label), snapshot->volume_label);
    bar->shared.volume_unmuted = snapshot->volume_unmuted;
    copy_string(bar->shared.sys_metrics, sizeof(bar->shared.sys_metrics), snapshot->sys_metrics);
    bar->shared_changed = true;
}

static void publish_radios(purp_bar_t *bar, const purp_status_snapshot_t *snapshot) {
    copy_string(bar->shared.wifi_label, sizeof(bar->shared.wifi_label), snapshot->wifi_label);
    bar->shared.wifi_connected = snapshot->wifi_connected;
    copy_string(bar->shared.bt_label, sizeof(bar->shared.bt_label), snapshot->bt_label);
    bar->shared.bt_active = snapshot->bt_active;
    bar->shared_changed = true;
}

static void *bar_status_thread(void *userdata) {
    purp_bar_t *bar = userdata;
    uint64_t tick = 0;
    uint64_t next_regular = purp_now_ms();
    pthread_mutex_lock(&bar->lock);
    while (!bar->stop) {
        uint64_t now = purp_now_ms();
        bool regular = now >= next_regular;
        bool immediate = bar->immediate_pending && now >= bar->immediate_due_ms;
        if (!regular && !immediate) {
            uint64_t wake = next_regular;
            if (bar->immediate_pending && bar->immediate_due_ms < wake) wake = bar->immediate_due_ms;
            struct timespec ts = {
                .tv_sec = (time_t)(wake / 1000),
                .tv_nsec = (long)((wake % 1000) * 1000000),
            };
            pthread_cond_timedwait(&bar->cond, &bar->lock, &ts);
            continue;
        }
        if (immediate) bar->immediate_pending = false;
        purp_status_snapshot_t snapshot = bar->shared;
        pthread_mutex_unlock(&bar->lock);

        refresh_fast(&snapshot);
        pthread_mutex_lock(&bar->lock);
        publish_fast(bar, &snapshot);
        bool radios = immediate || tick % PURP_BAR_RADIO_EVERY_N_TICKS == 0;
        pthread_mutex_unlock(&bar->lock);
        if (radios) {
            refresh_radios(&snapshot);
            pthread_mutex_lock(&bar->lock);
            publish_radios(bar, &snapshot);
        } else {
            pthread_mutex_lock(&bar->lock);
        }
        tick++;
        if (regular) {
            next_regular += PURP_BAR_STATUS_INTERVAL_MS;
            uint64_t after = purp_now_ms();
            if (next_regular <= after) next_regular = after + PURP_BAR_STATUS_INTERVAL_MS;
        }
    }
    pthread_mutex_unlock(&bar->lock);
    return NULL;
}

static void slot_configure(purp_bar_slot_t *slot, double scale) {
    slot->in_use = true;
    slot->scale = scale;
    slot->height = purp_max_i32(bar_px(PURP_BAR_HEIGHT, scale), 1);
    slot->font_px = PURP_BAR_FONT_SIZE * (float)scale;
    slot->tag_cell_width = purp_max_i32(bar_px(PURP_BAR_TAG_CELL_WIDTH, scale), 1);
    slot->layout_cell_end = slot->tag_cell_width * PURP_TAG_COUNT +
                            bar_px(PURP_BAR_LAYOUT_CELL_EXTRA, scale);
    slot->dirty = true;
}

bool purp_bar_init(purp_bar_t *bar, purp_font_t *font, bool start_poller) {
    if (!bar) return false;
    memset(bar, 0, sizeof(*bar));
    bar->font = font;
    if (pthread_mutex_init(&bar->lock, NULL) != 0) return false;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&bar->lock);
        return false;
    }
    bool clock_ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0;
    bool cond_ok = clock_ok && pthread_cond_init(&bar->cond, &attr) == 0;
    pthread_condattr_destroy(&attr);
    if (!cond_ok) {
        pthread_mutex_destroy(&bar->lock);
        return false;
    }
    copy_string(bar->current.wifi_label, sizeof(bar->current.wifi_label), "WIFI: ...");
    copy_string(bar->current.bt_label, sizeof(bar->current.bt_label), "BT: ...");
    copy_string(bar->current.volume_label, sizeof(bar->current.volume_label), "VOL ...");
    collect_fast_metrics(bar->current.sys_metrics);
    bar->shared = bar->current;
    bar->status_gen = 1;
    atomic_init(&bar->pending_redraw, true);
    bar->slots[0].width = 1280;
    slot_configure(&bar->slots[0], 1.0);
    if (start_poller) {
        if (pthread_create(&bar->thread, NULL, bar_status_thread, bar) != 0) {
            for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) purp_pixel_buf_free(&bar->slots[i].buffer);
            pthread_cond_destroy(&bar->cond);
            pthread_mutex_destroy(&bar->lock);
            return false;
        }
        bar->thread_started = true;
    }
    return true;
}

void purp_bar_finish(purp_bar_t *bar) {
    if (!bar) return;
    pthread_mutex_lock(&bar->lock);
    bar->stop = true;
    bar->immediate_pending = false;
    pthread_cond_signal(&bar->cond);
    pthread_mutex_unlock(&bar->lock);
    if (bar->thread_started) pthread_join(bar->thread, NULL);
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) purp_pixel_buf_free(&bar->slots[i].buffer);
    pthread_cond_destroy(&bar->cond);
    pthread_mutex_destroy(&bar->lock);
    memset(bar, 0, sizeof(*bar));
}

bool purp_bar_is_dirty(purp_bar_t *bar) {
    return bar && atomic_exchange_explicit(&bar->pending_redraw, false, memory_order_relaxed);
}

void purp_bar_mark_dirty(purp_bar_t *bar) {
    if (!bar) return;
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i)
        if (bar->slots[i].in_use) bar->slots[i].dirty = true;
    atomic_store_explicit(&bar->pending_redraw, true, memory_order_relaxed);
}

void purp_bar_set_monitor_scale(purp_bar_t *bar, size_t monitor, double scale) {
    if (!bar || monitor >= PURP_MAX_MONITORS) return;
    if (!isfinite(scale) || scale <= 0) scale = 1.0;
    purp_bar_slot_t *slot = &bar->slots[monitor];
    if (!slot->in_use) {
        slot->width = 1280;
        slot_configure(slot, scale);
        atomic_store_explicit(&bar->pending_redraw, true, memory_order_relaxed);
    } else if (slot->scale != scale) {
        slot_configure(slot, scale);
        atomic_store_explicit(&bar->pending_redraw, true, memory_order_relaxed);
    }
}

void purp_bar_prune_monitors(purp_bar_t *bar, size_t count) {
    if (!bar) return;
    size_t keep = count > 0 ? count : 1;
    if (keep > PURP_MAX_MONITORS) keep = PURP_MAX_MONITORS;
    for (size_t i = keep; i < PURP_MAX_MONITORS; ++i) {
        purp_pixel_buf_free(&bar->slots[i].buffer);
        memset(&bar->slots[i], 0, sizeof(bar->slots[i]));
    }
}

bool purp_bar_request_immediate_refresh(purp_bar_t *bar) {
    if (!bar) return false;
    bool armed = false;
    pthread_mutex_lock(&bar->lock);
    if (!bar->stop && !bar->immediate_pending) {
        bar->immediate_pending = true;
        bar->immediate_due_ms = purp_now_ms() + PURP_BAR_IMMEDIATE_DEBOUNCE_MS;
        pthread_cond_signal(&bar->cond);
        armed = true;
    }
    pthread_mutex_unlock(&bar->lock);
    return armed;
}

static bool snapshots_equal(const purp_status_snapshot_t *a, const purp_status_snapshot_t *b) {
    return a->wifi_connected == b->wifi_connected && a->bt_active == b->bt_active &&
           a->volume_unmuted == b->volume_unmuted &&
           strcmp(a->wifi_label, b->wifi_label) == 0 && strcmp(a->bt_label, b->bt_label) == 0 &&
           strcmp(a->volume_label, b->volume_label) == 0 && strcmp(a->sys_metrics, b->sys_metrics) == 0;
}

bool purp_bar_tick_status(purp_bar_t *bar) {
    if (!bar) return false;
    uint64_t now = purp_now_ms();
    if (bar->last_status_check_ms != 0 && now - bar->last_status_check_ms < PURP_BAR_STATUS_CHECK_MS)
        return false;
    bar->last_status_check_ms = now;
    bool changed = false;
    pthread_mutex_lock(&bar->lock);
    if (bar->shared_changed) {
        purp_status_snapshot_t snapshot = bar->shared;
        bar->shared_changed = false;
        if (!snapshots_equal(&snapshot, &bar->current)) {
            bar->current = snapshot;
            bar->status_gen++;
            changed = true;
        }
    }
    pthread_mutex_unlock(&bar->lock);
    if (changed) purp_bar_mark_dirty(bar);
    return changed;
}

static bool range_contains(purp_bar_range_t range, int32_t x) {
    return range.end > range.start && x >= range.start && x < range.end;
}

purp_bar_action_t purp_bar_hit_test(const purp_bar_t *bar, size_t monitor, int32_t local_x) {
    purp_bar_action_t action = { .kind = PURP_BAR_ACTION_FOCUS_NEXT, .tag = 0 };
    if (!bar) return action;
    if (monitor >= PURP_MAX_MONITORS || !bar->slots[monitor].in_use) monitor = 0;
    const purp_bar_slot_t *slot = &bar->slots[monitor];
    if (!slot->in_use) return action;
    int32_t x = (int32_t)((double)local_x * slot->scale);
    int32_t tags_end = slot->tag_cell_width * PURP_TAG_COUNT;
    if (x >= 0 && x < tags_end) {
        action.kind = PURP_BAR_ACTION_TAG;
        action.tag = purp_min_i32(x / slot->tag_cell_width, PURP_TAG_COUNT - 1);
    } else if (x >= tags_end && x < slot->layout_cell_end) {
        action.kind = PURP_BAR_ACTION_CYCLE_LAYOUT;
    } else if (range_contains(slot->wifi, x)) {
        action.kind = PURP_BAR_ACTION_WIFI;
    } else if (range_contains(slot->bt, x)) {
        action.kind = PURP_BAR_ACTION_BLUETOOTH;
    } else if (range_contains(slot->volume, x)) {
        action.kind = PURP_BAR_ACTION_VOLUME;
    } else if (range_contains(slot->metrics, x)) {
        action.kind = PURP_BAR_ACTION_SYSTEM_MONITOR;
    } else if (range_contains(slot->flameshot, x)) {
        action.kind = PURP_BAR_ACTION_FLAMESHOT;
    }
    return action;
}

static const char *bar_layout_symbol(purp_layout_t layout) {
    static const char *const symbols[] = { "[]=", "|M|", "###", "[M]", "><>" };
    return layout >= 0 && layout < PURP_LAYOUT_COUNT ? symbols[layout] : symbols[0];
}

static int32_t draw_bar_text(purp_bar_t *bar, purp_pixel_buf_t *buffer, const char *text, int32_t x,
                             int32_t top, float px, purp_rgba_t color) {
    return purp_font_draw(bar->font, buffer, text, x, top + (int32_t)px - 2, px, color);
}

static int32_t draw_bar_text_clipped(purp_bar_t *bar, purp_pixel_buf_t *buffer, const char *text,
                                     int32_t x, int32_t top, float px, int32_t max_w,
                                     purp_rgba_t color) {
    return purp_font_draw_clipped(bar->font, buffer, text, x, top + (int32_t)px - 2, px, max_w, color);
}

static void bar_redraw(purp_bar_t *bar, purp_bar_slot_t *slot, const purp_bar_inputs_t *inputs,
                       const char *title) {
    purp_pixel_buf_t *buffer = &slot->buffer;
    int32_t w = slot->width, h = slot->height;
    double scale = slot->scale;
    float font_px = slot->font_px;
    int32_t text_y = purp_max_i32((h - (int32_t)font_px) / 2, bar_px(4, scale));
    int32_t tag_w = slot->tag_cell_width;
    const purp_status_snapshot_t *snapshot = &bar->current;
    purp_fill_rect(buffer, 0, 0, w, h, PURP_BAR_BG_RGBA);
    purp_fill_rect(buffer, 0, h - 1, w, 1, PURP_BAR_ACCENT_LINE_RGBA);

    int32_t cursor = 0;
    for (size_t i = 0; i < PURP_TAG_COUNT; ++i) {
        uint16_t mask = (uint16_t)(1u << i);
        bool selected = (inputs->selected_tags & mask) != 0;
        bool occupied = (inputs->occupied_tags & mask) != 0;
        bool focused = (inputs->focused_tags & mask) != 0;
        bool urgent = (inputs->urgent_tags & mask) != 0;
        if (urgent) purp_fill_rect(buffer, cursor, 0, tag_w, h - 1, PURP_BAR_URGENT_RGBA);
        else if (selected) purp_fill_rect(buffer, cursor, 0, tag_w, h - 1, PURP_BAR_TAG_ACTIVE_RGBA);
        if (occupied) {
            purp_rgba_t color = selected || urgent ? PURP_BAR_TEXT_FG_RGBA : PURP_BAR_ACCENT_RGBA;
            if (focused) {
                purp_fill_rect(buffer, cursor + bar_px(4, scale), bar_px(4, scale),
                               bar_px(5, scale), bar_px(5, scale), color);
            } else {
                purp_fill_rect(buffer, cursor + bar_px(4, scale), bar_px(4, scale),
                               bar_px(5, scale), bar_px(1, scale), color);
                purp_fill_rect(buffer, cursor + bar_px(4, scale), bar_px(8, scale),
                               bar_px(5, scale), bar_px(1, scale), color);
                purp_fill_rect(buffer, cursor + bar_px(4, scale), bar_px(4, scale),
                               bar_px(1, scale), bar_px(5, scale), color);
                purp_fill_rect(buffer, cursor + bar_px(8, scale), bar_px(4, scale),
                               bar_px(1, scale), bar_px(5, scale), color);
            }
        }
        purp_rgba_t color = selected || occupied || urgent ? PURP_BAR_TEXT_FG_RGBA : PURP_BAR_TEXT_DIM_RGBA;
        int32_t text_w = purp_font_measure(bar->font, PURP_TAG_NAMES[i], font_px);
        draw_bar_text(bar, buffer, PURP_TAG_NAMES[i], cursor + (tag_w - text_w) / 2, text_y, font_px, color);
        cursor += tag_w;
    }

    const char *symbol = bar_layout_symbol(inputs->layout);
    int32_t symbol_w = purp_font_measure(bar->font, symbol, font_px) + bar_px(20, scale);
    purp_fill_rect(buffer, cursor, 0, symbol_w, h - 1, PURP_BAR_LAYOUT_BG_RGBA);
    draw_bar_text(bar, buffer, symbol, cursor + bar_px(10, scale), text_y, font_px, PURP_BAR_ACCENT_RGBA);
    cursor += symbol_w;
    slot->layout_cell_end = cursor;
    slot->wifi = slot->bt = slot->volume = slot->flameshot = slot->metrics = (purp_bar_range_t){0};

    int32_t metrics_w = purp_font_measure(bar->font, snapshot->sys_metrics, font_px) + bar_px(18, scale);
    int32_t right = w - metrics_w;
    draw_bar_text(bar, buffer, snapshot->sys_metrics, right + bar_px(6, scale), text_y, font_px,
                  PURP_BAR_TEXT_FG_RGBA);
    slot->metrics = (purp_bar_range_t){ right, w };

    int32_t pill_w = purp_font_measure(bar->font, snapshot->volume_label, font_px) + bar_px(20, scale);
    right -= pill_w + bar_px(8, scale);
    if (right > cursor + bar_px(40, scale)) {
        purp_fill_rect(buffer, right, bar_px(3, scale), pill_w, h - bar_px(7, scale), PURP_BAR_LAYOUT_BG_RGBA);
        draw_bar_text(bar, buffer, snapshot->volume_label, right + bar_px(10, scale), text_y, font_px,
                      snapshot->volume_unmuted ? PURP_BAR_TEXT_FG_RGBA : PURP_BAR_TEXT_DIM_RGBA);
        slot->volume = (purp_bar_range_t){ right, right + pill_w };
    }

    pill_w = purp_font_measure(bar->font, snapshot->bt_label, font_px) + bar_px(20, scale);
    right -= pill_w + bar_px(8, scale);
    if (right > cursor + bar_px(40, scale)) {
        purp_fill_rect(buffer, right, bar_px(3, scale), pill_w, h - bar_px(7, scale),
                       snapshot->bt_active ? PURP_BAR_BT_ACTIVE_RGBA : PURP_BAR_LAYOUT_BG_RGBA);
        draw_bar_text(bar, buffer, snapshot->bt_label, right + bar_px(10, scale), text_y, font_px,
                      snapshot->bt_active ? PURP_BAR_TEXT_FG_RGBA : PURP_BAR_TEXT_DIM_RGBA);
        slot->bt = (purp_bar_range_t){ right, right + pill_w };
    }

    pill_w = purp_font_measure(bar->font, snapshot->wifi_label, font_px) + bar_px(20, scale);
    right -= pill_w + bar_px(8, scale);
    if (right > cursor + bar_px(40, scale)) {
        purp_fill_rect(buffer, right, bar_px(3, scale), pill_w, h - bar_px(7, scale),
                       snapshot->wifi_connected ? PURP_BAR_TAG_ACTIVE_RGBA : PURP_BAR_LAYOUT_BG_RGBA);
        draw_bar_text(bar, buffer, snapshot->wifi_label, right + bar_px(10, scale), text_y, font_px,
                      snapshot->wifi_connected ? PURP_BAR_TEXT_FG_RGBA : PURP_BAR_TEXT_DIM_RGBA);
        slot->wifi = (purp_bar_range_t){ right, right + pill_w };
    }

    pill_w = purp_font_measure(bar->font, "Flameshot", font_px) + bar_px(20, scale);
    int32_t flameshot_x = right - pill_w - bar_px(8, scale);
    if (flameshot_x > cursor + bar_px(40, scale)) {
        right = flameshot_x;
        purp_fill_rect(buffer, right, bar_px(3, scale), pill_w, h - bar_px(7, scale), PURP_BAR_LAYOUT_BG_RGBA);
        draw_bar_text(bar, buffer, "Flameshot", right + bar_px(10, scale), text_y, font_px, PURP_BAR_ACCENT_RGBA);
        slot->flameshot = (purp_bar_range_t){ right, right + pill_w };
    }

    int32_t title_width = purp_max_i32(right - cursor - bar_px(24, scale), 0);
    if (title_width > bar_px(32, scale) && *title)
        draw_bar_text_clipped(bar, buffer, title, cursor + bar_px(12, scale), text_y, font_px,
                              title_width, PURP_BAR_TEXT_FG_RGBA);
    purp_pixel_buf_touch(buffer);
}

const purp_pixel_buf_t *purp_bar_render(purp_bar_t *bar, size_t monitor,
                                        int32_t output_width_logical, double scale,
                                        const purp_bar_inputs_t *inputs) {
    if (!bar || !inputs) return NULL;
    if (monitor >= PURP_MAX_MONITORS) monitor = 0;
    if (!isfinite(scale) || scale <= 0) scale = 1.0;
    purp_bar_slot_t *slot = &bar->slots[monitor];
    if (!slot->in_use) slot_configure(slot, scale);
    else if (slot->scale != scale) slot_configure(slot, scale);
    int32_t target_w = purp_round_i32((double)purp_max_i32(output_width_logical, 0) * scale);
    target_w = purp_max_i32(target_w, 320);
    if (slot->width != target_w) {
        slot->width = target_w;
        slot->dirty = true;
    }
    if (slot->last_status_gen != bar->status_gen) slot->dirty = true;
    const char *title = inputs->focused_title ? inputs->focused_title : "";
    bool inputs_changed = !slot->has_last_inputs || slot->last_inputs.selected_tags != inputs->selected_tags ||
                          slot->last_inputs.occupied_tags != inputs->occupied_tags ||
                          slot->last_inputs.focused_tags != inputs->focused_tags ||
                          slot->last_inputs.urgent_tags != inputs->urgent_tags ||
                          slot->last_inputs.layout != inputs->layout || strcmp(slot->last_title, title) != 0;
    if (inputs_changed) {
        purp_utf8_truncate(slot->last_title, sizeof(slot->last_title), title, SIZE_MAX);
        slot->last_inputs = *inputs;
        slot->last_inputs.focused_title = slot->last_title;
        slot->has_last_inputs = true;
        slot->dirty = true;
    }
    if (!slot->dirty) return &slot->buffer;
    if (!purp_pixel_buf_ensure(&slot->buffer, slot->width, slot->height)) return NULL;
    bar_redraw(bar, slot, inputs, title);
    slot->last_status_gen = bar->status_gen;
    slot->dirty = false;
    return &slot->buffer;
}
