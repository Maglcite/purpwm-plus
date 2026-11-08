/*
 * purpwm+ — JSON-line IPC, monitors.json persistence and the portal capture socket.
 *
 * Implemented by src/ipc.c (JSON, IPC server, monitors.json) and src/capture.c (capture server).
 * Pure header: no libwayland. Everything here is non-blocking and runs on the compositor thread;
 * no helper threads are used.
 *
 * IPC protocol ($PURPWM_IPC_SOCKET = $XDG_RUNTIME_DIR/purpwm+-ipc-<WAYLAND_DISPLAY>.sock, mode
 * 0600; symlinked as purpwm+-ipc.sock and purpwm-ipc.sock): a client writes ONE JSON object
 * terminated by '\n' and receives ONE JSON line back.
 *
 *   {"cmd":"get_monitors"}
 *   {"cmd":"apply_monitors","monitors":[{"name":S,"enabled":B?,"primary":B?,"width":N,
 *       "height":N,"refresh_mhz":N?,"scale":F,"x":N?,"y":N?},...]}
 *   {"cmd":"focus_monitor","delta":N?}            (delta defaults to 1)
 *   {"cmd":"move_window_to_monitor","delta":N?}   (delta defaults to 1)
 *
 *   -> {"ok":true,"monitors":[{"name":S,"description":S,"enabled":B,"primary":B,"width":U,
 *       "height":U,"refresh_mhz":I,"scale":F,"x":I,"y":I,"logical_width":I,"logical_height":I,
 *       "available_modes":[{"width":U,"height":U,"refresh_mhz":I,"preferred":B},...]},...]}
 *   -> {"ok":true}                                (focus/move)
 *   -> {"ok":false,"error":S}
 *
 * Bounds: <= 32 monitor entries; width/height 100..=32768; refresh_mhz 1000..=1000000 (missing or
 * null => 60000); scale 0.5..=4.0; x/y integers in -131072..=131072 (missing or null => 0, any
 * explicit invalid value is an error). Requests and monitors.json are capped at 64 KiB.
 */
#ifndef PURPWM_IPC_H
#define PURPWM_IPC_H

#include "types.h"

/* =========================================================================================== */
/* Limits and error strings                                                                   */
/* =========================================================================================== */

#define PURP_IPC_MAX_CLIENTS        8
#define PURP_IPC_MAX_REQUEST_BYTES  (64 * 1024)
#define PURP_IPC_READ_CHUNK         4096
#define PURP_IPC_READ_TIMEOUT_MS    2000   /* partial request is processed after this */
#define PURP_IPC_REPLY_TIMEOUT_MS   5000   /* no reply => "IPC response timed out" */
#define PURP_IPC_WRITE_TIMEOUT_MS   3000   /* reply not fully written => connection dropped */
#define PURP_IPC_MAX_MONITORS       32
#define PURP_IPC_MIN_DIMENSION      100u
#define PURP_IPC_MAX_DIMENSION      32768u
#define PURP_IPC_MIN_REFRESH_MHZ    1000
#define PURP_IPC_MAX_REFRESH_MHZ    1000000
#define PURP_IPC_DEFAULT_REFRESH_MHZ 60000
#define PURP_IPC_MIN_SCALE          0.5
#define PURP_IPC_MAX_SCALE          4.0
#define PURP_IPC_COORD_MIN          (-131072)
#define PURP_IPC_COORD_MAX          131072
#define PURP_JSON_MAX_DEPTH         32
#define PURP_IPC_ERROR_MAX          256     /* error message buffer size used by callers */

#define PURP_IPC_ERR_TOO_LARGE      "IPC request exceeds 64 KiB limit"
#define PURP_IPC_ERR_NOT_UTF8       "IPC request is not valid UTF-8"
#define PURP_IPC_ERR_LOCKED         "session is locked"
#define PURP_IPC_ERR_TIMEOUT        "IPC response timed out"
#define PURP_IPC_ERR_EMPTY          "empty IPC command"
#define PURP_IPC_ERR_NOT_OBJECT     "IPC command must be a JSON object"
#define PURP_IPC_ERR_NO_CMD         "missing string field 'cmd'"
#define PURP_IPC_ERR_NO_MONITORS    "'apply_monitors' requires a 'monitors' array"
/* apply_monitor_configs() errors (wm.c). */
#define PURP_IPC_ERR_EMPTY_CONFIG   "Monitor configuration list cannot be empty."
#define PURP_IPC_ERR_ALL_DISABLED   "At least one display must remain enabled."

/* =========================================================================================== */
/* Monitor configuration records                                                              */
/* =========================================================================================== */

/* One entry of `apply_monitors` / monitors.json (keys: name, enabled, primary, width, height,
 * refresh_mhz, scale, x, y). Names may carry a "purpwm+-" or "purpwm-" prefix (nested outputs);
 * the matcher tolerates either. */
typedef struct purp_monitor_config {
    char name[PURP_MONITOR_NAME_MAX];
    bool enabled;
    bool primary;
    uint32_t width;
    uint32_t height;
    int32_t refresh_mhz;
    double scale;
    int32_t x;
    int32_t y;
} purp_monitor_config_t;

/* One `get_monitors` reply entry (MonitorState::to_monitor_info). */
typedef struct purp_monitor_info {
    char name[PURP_MONITOR_NAME_MAX];
    char description[PURP_MONITOR_DESC_MAX];
    bool enabled;
    bool primary;
    uint32_t width;
    uint32_t height;
    int32_t refresh_mhz;
    double scale;
    int32_t x;
    int32_t y;
    int32_t logical_width;
    int32_t logical_height;
    purp_display_mode_t modes[PURP_MAX_MODES];
    size_t mode_count;
} purp_monitor_info_t;

/* =========================================================================================== */
/* Growable string buffer                                                                     */
/* =========================================================================================== */

/* NUL-terminated growable byte string. On allocation failure `oom` latches and further appends
 * become no-ops; callers check it once at the end. Zero-initialise ({0}) before first use. */
typedef struct purp_strbuf {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} purp_strbuf_t;

void purp_strbuf_append(purp_strbuf_t *sb, const char *s);
void purp_strbuf_append_len(purp_strbuf_t *sb, const char *s, size_t len);
void purp_strbuf_appendf(purp_strbuf_t *sb, const char *fmt, ...) PURP_PRINTF(2, 3);
/* Append a JSON string literal: `"` and `\` escaped, \n \r \t as escapes, other bytes < 0x20 as
 * \u00xx (lowercase hex), UTF-8 above 0x7F emitted raw. */
void purp_strbuf_append_json_string(purp_strbuf_t *sb, const char *s);
/* Release the storage and zero the struct. */
void purp_strbuf_free(purp_strbuf_t *sb);

/* =========================================================================================== */
/* Minimal RFC 8259 JSON (linear time)                                                        */
/* =========================================================================================== */

typedef enum purp_json_type {
    PURP_JSON_NULL = 0,
    PURP_JSON_BOOL,
    PURP_JSON_NUMBER,
    PURP_JSON_STRING,
    PURP_JSON_ARRAY,
    PURP_JSON_OBJECT,
} purp_json_type_t;

typedef struct purp_json_value purp_json_value_t;
typedef struct purp_json_member purp_json_member_t;

struct purp_json_value {
    purp_json_type_t type;
    union {
        bool boolean;
        double number;                                        /* always finite */
        struct { char *data; size_t len; } string;            /* UTF-8, NUL-terminated */
        struct { purp_json_value_t *items; size_t len; } array;
        struct { purp_json_member_t *members; size_t len; } object;
    } u;
};

struct purp_json_member {
    char *key;              /* UTF-8, NUL-terminated */
    size_t key_len;
    purp_json_value_t value;
};

/*
 * Parse exactly one JSON value (surrounding whitespace allowed: space, tab, CR, LF).
 * Linear in `len`; nesting deeper than PURP_JSON_MAX_DEPTH fails with "JSON nesting too deep".
 * Strings: escapes \" \\ \/ \b \f \n \r \t \uXXXX with surrogate pairs combined; lone or unpaired
 * surrogates, unescaped control bytes (< 0x20) and invalid UTF-8 are errors. Numbers are
 * `-?digits(.digits)?([eE][+-]?digits)?` parsed to a finite double ("JSON number out of range"
 * otherwise). Trailing data is an error ("trailing characters at byte N").
 * On failure returns false, writes a message to err (if non-NULL) and leaves *out as JSON null.
 */
bool purp_json_parse(const char *text, size_t len, purp_json_value_t *out, char *err,
                     size_t err_len);
/* Free everything owned by `v` (recursively) and reset it to JSON null. */
void purp_json_free(purp_json_value_t *v);
/* Object member lookup; duplicate keys resolve to the LAST occurrence. NULL if absent or `obj`
 * is not an object. */
const purp_json_value_t *purp_json_get(const purp_json_value_t *obj, const char *key);
/* Typed accessors returning false when the value has another type or is out of range:
 *   as_i32: number with no fractional part inside the int32 range;
 *   as_u32: number n >= 0, n <= UINT32_MAX and |n - round(n)| <= 1e-6 (rounded);
 *   as_f64: any (finite) number. */
bool purp_json_as_bool(const purp_json_value_t *v, bool *out);
bool purp_json_as_i32(const purp_json_value_t *v, int32_t *out);
bool purp_json_as_u32(const purp_json_value_t *v, uint32_t *out);
bool purp_json_as_f64(const purp_json_value_t *v, double *out);
/* Serialise any value compactly (object members in stored order). */
void purp_json_serialize(const purp_json_value_t *v, purp_strbuf_t *out);

/* =========================================================================================== */
/* IPC commands and replies                                                                   */
/* =========================================================================================== */

typedef enum purp_ipc_command_kind {
    PURP_IPC_GET_MONITORS = 0,
    PURP_IPC_APPLY_MONITORS,
    PURP_IPC_FOCUS_MONITOR,
    PURP_IPC_MOVE_WINDOW_TO_MONITOR,
} purp_ipc_command_kind_t;

typedef struct purp_ipc_command {
    purp_ipc_command_kind_t kind;
    int32_t delta;                                          /* focus/move; default 1 */
    purp_monitor_config_t monitors[PURP_IPC_MAX_MONITORS];  /* apply_monitors */
    size_t monitor_count;
} purp_ipc_command_t;

/* Parse one request line (surrounding whitespace trimmed). Errors use the exact Rust strings:
 * PURP_IPC_ERR_EMPTY, PURP_IPC_ERR_NOT_OBJECT, PURP_IPC_ERR_NO_CMD, PURP_IPC_ERR_NO_MONITORS,
 * "unknown IPC command '<cmd>'", plus the monitor-entry errors of
 * purp_ipc_parse_monitor_entries(). A non-integer `delta` silently becomes 1. */
bool purp_ipc_parse_command(const char *line, size_t len, purp_ipc_command_t *out, char *err,
                            size_t err_len);

/* Validate a JSON array of monitor entries into `out` (capacity PURP_IPC_MAX_MONITORS).
 * Errors: "monitors array must contain at most 32 entries", "monitors[i] must be an object",
 * "monitors[i].name must be a non-empty string", "monitors[i].width must be 100..=32768",
 * "monitors[i].height must be 100..=32768",
 * "monitors[i].refresh_mhz must be an integer in 1000..=1000000",
 * "monitors[i].scale must be 0.5..=4.0", "monitors[i].x must be an integer in -131072..=131072"
 * (and .y). Defaults: enabled true, primary false, refresh 60000, x/y 0. */
bool purp_ipc_parse_monitor_entries(const purp_json_value_t *array, purp_monitor_config_t *out,
                                    size_t *count, char *err, size_t err_len);

/* Scale as JSON number text: rounded to 4 decimals; integral values print as "1.0"/"2.0",
 * others with trailing zeros stripped ("1.25", "1.5", "1.3333"). `out` needs >= 32 bytes. */
void purp_ipc_format_scale(double scale, char *out, size_t out_len);

/* `{"ok":true,"monitors":[...]}\n` (exact key order documented at the top of this file). */
void purp_ipc_serialize_monitors_response(const purp_monitor_info_t *monitors, size_t count,
                                          purp_strbuf_t *out);
/* `{"ok":false,"error":"..."}\n` */
void purp_ipc_serialize_error_response(const char *error, purp_strbuf_t *out);
/* `{"ok":true}\n` */
void purp_ipc_serialize_ok_response(purp_strbuf_t *out);

/* =========================================================================================== */
/* monitors.json persistence                                                                  */
/* =========================================================================================== */

/* Write path: $PURPWM_MONITORS_CONFIG (trimmed, if non-empty), else
 * $XDG_CONFIG_HOME/purpwm+/monitors.json, else $HOME/.config/purpwm+/monitors.json.
 * Returns false when neither XDG_CONFIG_HOME nor HOME is set. */
bool purp_monitors_config_path(char *out, size_t out_len);
/* Read-only fallback: $XDG_CONFIG_HOME/purpwm/monitors.json (or $HOME/.config/purpwm/...).
 * Returns false when PURPWM_MONITORS_CONFIG is set (no fallback then) or no base dir exists. */
bool purp_monitors_legacy_config_path(char *out, size_t out_len);

/* Accept either a bare array of entries or {"monitors":[...]} (extra keys ignored); otherwise
 * "monitors.json must be a JSON array or {\"monitors\": [...]}". */
bool purp_parse_monitors_config_json(const char *text, size_t len, purp_monitor_config_t *out,
                                     size_t *count, char *err, size_t err_len);
/* Load from a path. Missing file => 0 silently; > 64 KiB, invalid UTF-8 or invalid JSON => 0 with
 * a warning ("Ignoring monitors.json exceeding 64 KiB limit" / "Ignoring invalid monitors.json").
 * A file of exactly 65536 bytes is accepted. `out` capacity: PURP_IPC_MAX_MONITORS. */
size_t purp_load_saved_monitors_from(const char *path, purp_monitor_config_t *out);
/* Load from purp_monitors_config_path(), falling back to the legacy purpwm path when the
 * purpwm+ file does not exist. */
size_t purp_load_saved_monitors(purp_monitor_config_t *out);
/* Pretty file format: "{\n  \"monitors\": [\n    {\"name\": S, \"enabled\": B, ...},\n  ]\n}\n"
 * with one entry per line, keys name, enabled, primary, width, height, refresh_mhz, scale, x, y. */
void purp_serialize_monitors_config_json(const purp_monitor_config_t *entries, size_t count,
                                         purp_strbuf_t *out);
/* Atomic save: mkdir -p the parent, write ".<file>.tmp.<pid>" next to the target, rename over
 * it (temp removed on failure). Returns false with errno set on failure. */
bool purp_save_monitors_config_to(const char *path, const purp_monitor_config_t *entries,
                                  size_t count);
/* Save to purp_monitors_config_path(); true (no-op) when no path can be resolved. */
bool purp_save_monitors_config(const purp_monitor_config_t *entries, size_t count);

/* =========================================================================================== */
/* Non-blocking IPC server                                                                    */
/* =========================================================================================== */

typedef enum purp_ipc_client_state {
    PURP_IPC_CLIENT_FREE = 0,
    PURP_IPC_CLIENT_READING,     /* collecting the request line */
    PURP_IPC_CLIENT_AWAITING,    /* request handed to the compositor, reply pending */
    PURP_IPC_CLIENT_WRITING,     /* reply queued, draining with non-blocking writes */
} purp_ipc_client_state_t;

typedef struct purp_ipc_client {
    purp_ipc_client_state_t state;
    int fd;
    uint64_t serial;             /* matches purp_ipc_request_t.client_serial */
    uint8_t *in;                 /* request bytes (<= PURP_IPC_MAX_REQUEST_BYTES) */
    size_t in_len;
    bool overflowed;
    char *out;                   /* reply bytes, always '\n'-terminated */
    size_t out_len;
    size_t out_off;
    uint64_t since_ms;           /* accept time (read deadline) */
    uint64_t deadline_ms;        /* reply deadline while AWAITING, write deadline while WRITING */
} purp_ipc_client_t;

typedef struct purp_ipc_server {
    int listen_fd;               /* -1 when not bound */
    char path[108];              /* sun_path of the bound socket */
    char link_paths[2][108];     /* purpwm+-ipc.sock, purpwm-ipc.sock we created ("" if not) */
    purp_ipc_client_t clients[PURP_IPC_MAX_CLIENTS];
    uint64_t next_serial;
} purp_ipc_server_t;

/* A parsed request awaiting purp_ipc_server_respond(). */
typedef struct purp_ipc_request {
    purp_ipc_command_t command;
    size_t client_slot;
    uint64_t client_serial;
} purp_ipc_request_t;

/*
 * Bind `path` (0600, non-blocking) and export PURPWM_IPC_SOCKET=path. A live socket at `path`
 * fails with "ipc socket is live"; a stale one is unlinked. Then refresh the convenience links
 * <runtime_dir>/purpwm+-ipc.sock and <runtime_dir>/purpwm-ipc.sock (symlink to a
 * ".tmp.<pid>" name, renamed over the link) when `!is_nested`, PURPWM_UPDATE_IPC_LINK is "1" or
 * "true" (case-insensitive), or the existing link does not connect.
 */
bool purp_ipc_server_bind(purp_ipc_server_t *srv, const char *path, const char *runtime_dir,
                          bool is_nested, char *err, size_t err_len);
/*
 * Accept (up to PURP_IPC_MAX_CLIENTS live connections; extras are closed immediately), read
 * request lines, and return complete parsed requests (at most `max`). A request completes on
 * '\n', EOF, overflow (> 64 KiB) or after PURP_IPC_READ_TIMEOUT_MS; only the first line is
 * parsed. Malformed requests are answered here with an error reply; while `locked`, everything
 * except get_monitors is answered with "session is locked". Also drives pending writes and
 * reply timeouts (equivalent to purp_ipc_server_flush). Never blocks.
 */
size_t purp_ipc_server_poll(purp_ipc_server_t *srv, bool locked, purp_ipc_request_t *out,
                            size_t max);
/* Queue `json_line` (a '\n' is appended if missing) as the reply to `req` and start writing.
 * Stale requests (client gone) are ignored. */
void purp_ipc_server_respond(purp_ipc_server_t *srv, const purp_ipc_request_t *req,
                             const char *json_line);
/* Continue non-blocking writes; drop clients past their write deadline; answer requests that
 * waited longer than PURP_IPC_REPLY_TIMEOUT_MS with "IPC response timed out". */
void purp_ipc_server_flush(purp_ipc_server_t *srv);
/* Listening fd for event-loop integration (-1 when unbound). */
int purp_ipc_server_fd(const purp_ipc_server_t *srv);
/* Close all clients and the listener, unlink the socket and any link that still points at it. */
void purp_ipc_server_close(purp_ipc_server_t *srv);

/* =========================================================================================== */
/* Portal screenshot capture socket (src/capture.c)                                           */
/* =========================================================================================== */

/*
 * $PURPWM_CAPTURE_SOCKET = $XDG_RUNTIME_DIR/purpwm+-capture-<WAYLAND_DISPLAY>.sock (0600).
 * A client sends exactly the 8 bytes "capture\n" and receives either
 *     "OK <png length>\n" followed by the raw PNG bytes of the focused monitor, or
 *     "ERR capture unavailable\n"
 * (invalid request, session locked, render failure or timeout all collapse to ERR).
 * Limits: 8 live clients, 16-byte request reads, 2 s read deadline, 3 s write timeout,
 * 10 s render wait.
 */
#define PURP_CAPTURE_MAX_CLIENTS     8
#define PURP_CAPTURE_REQUEST         "capture\n"
#define PURP_CAPTURE_MAX_REQUEST     16
#define PURP_CAPTURE_READ_TIMEOUT_MS 2000
#define PURP_CAPTURE_WRITE_TIMEOUT_MS 3000
#define PURP_CAPTURE_RENDER_TIMEOUT_MS 10000
#define PURP_CAPTURE_ERR_REPLY       "ERR capture unavailable\n"

/* Produce a PNG of the current desktop (the compositor renders the focused monitor offscreen
 * with the cursor hidden). Returns a new blob reference (caller unrefs) or NULL on failure. */
typedef purp_blob_t *(*purp_capture_fn)(void *data);

typedef enum purp_capture_client_state {
    PURP_CAPTURE_CLIENT_FREE = 0,
    PURP_CAPTURE_CLIENT_READING,
    PURP_CAPTURE_CLIENT_WRITING,
} purp_capture_client_state_t;

typedef struct purp_capture_client {
    purp_capture_client_state_t state;
    int fd;
    uint8_t in[PURP_CAPTURE_MAX_REQUEST];
    size_t in_len;
    char header[48];             /* "OK <len>\n" or the ERR line */
    size_t header_len;
    size_t header_off;
    purp_blob_t *png;            /* body (NULL for ERR) */
    size_t body_off;
    uint64_t since_ms;
    uint64_t deadline_ms;
} purp_capture_client_t;

typedef struct purp_capture_server {
    int listen_fd;
    char path[108];
    purp_capture_client_t clients[PURP_CAPTURE_MAX_CLIENTS];
} purp_capture_server_t;

/* Bind (0600, non-blocking). A live socket fails with "capture socket is live"; stale ones are
 * unlinked. */
bool purp_capture_server_bind(purp_capture_server_t *srv, const char *path, char *err,
                              size_t err_len);
/* Accept/read/write without blocking. When at least one valid request is complete and the session
 * is not `locked`, `capture` is called ONCE and its PNG answers every waiting client; otherwise
 * clients get PURP_CAPTURE_ERR_REPLY. Returns true when a capture was taken (the caller should
 * repaint, since an offscreen frame was rendered). */
bool purp_capture_server_poll(purp_capture_server_t *srv, bool locked, purp_capture_fn capture,
                              void *data);
int purp_capture_server_fd(const purp_capture_server_t *srv);
void purp_capture_server_close(purp_capture_server_t *srv);

#endif /* PURPWM_IPC_H */
