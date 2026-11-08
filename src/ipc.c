/* JSON-line IPC, monitor configuration persistence, and non-blocking clients. */
#include "ipc.h"
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static bool strbuf_reserve(purp_strbuf_t *sb, size_t extra) {
    if (sb->oom) return false;
    if (extra > SIZE_MAX - sb->len - 1) {
        sb->oom = true;
        return false;
    }
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) return true;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *data = realloc(sb->data, cap);
    if (!data) {
        sb->oom = true;
        return false;
    }
    sb->data = data;
    sb->cap = cap;
    sb->data[sb->len] = '\0';
    return true;
}

void purp_strbuf_append_len(purp_strbuf_t *sb, const char *s, size_t len) {
    if (!strbuf_reserve(sb, len)) return;
    if (len) memcpy(sb->data + sb->len, s, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void purp_strbuf_append(purp_strbuf_t *sb, const char *s) {
    purp_strbuf_append_len(sb, s, strlen(s));
}

void purp_strbuf_appendf(purp_strbuf_t *sb, const char *fmt, ...) {
    if (sb->oom) return;
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (len < 0) sb->oom = true;
    else if (strbuf_reserve(sb, (size_t)len)) {
        vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
        sb->len += (size_t)len;
    }
    va_end(ap);
}

static void append_json_string_len(purp_strbuf_t *sb, const char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";
    purp_strbuf_append_len(sb, "\"", 1);
    size_t start = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        purp_strbuf_append_len(sb, s + start, i - start);
        switch (c) {
        case '"': purp_strbuf_append(sb, "\\\""); break;
        case '\\': purp_strbuf_append(sb, "\\\\"); break;
        case '\n': purp_strbuf_append(sb, "\\n"); break;
        case '\r': purp_strbuf_append(sb, "\\r"); break;
        case '\t': purp_strbuf_append(sb, "\\t"); break;
        default: {
            char escape[] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
            purp_strbuf_append_len(sb, escape, sizeof escape);
            break;
        }
        }
        start = i + 1;
    }
    purp_strbuf_append_len(sb, s + start, len - start);
    purp_strbuf_append_len(sb, "\"", 1);
}

void purp_strbuf_append_json_string(purp_strbuf_t *sb, const char *s) {
    append_json_string_len(sb, s, strlen(s));
}

void purp_strbuf_free(purp_strbuf_t *sb) {
    free(sb->data);
    memset(sb, 0, sizeof *sb);
}

static void set_error(char *err, size_t err_len, const char *fmt, ...) PURP_PRINTF(3, 4);
static void set_error(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || !err_len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* Decode one canonical UTF-8 scalar; reject overlong sequences and surrogates. */
static bool utf8_next(const char *text, size_t len, size_t *pos, uint32_t *scalar) {
    size_t at = *pos;
    if (at >= len) return false;
    unsigned char first = (unsigned char)text[at++];
    uint32_t value;
    unsigned int remaining;
    uint32_t minimum;
    if (first < 0x80) {
        *pos = at;
        *scalar = first;
        return true;
    }
    if (first >= 0xc2 && first <= 0xdf) {
        value = first & 0x1f;
        remaining = 1;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        value = first & 0x0f;
        remaining = 2;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        value = first & 7;
        remaining = 3;
        minimum = 0x10000;
    } else return false;
    if (remaining > len - at) return false;
    while (remaining--) {
        unsigned char c = (unsigned char)text[at++];
        if ((c & 0xc0) != 0x80) return false;
        value = (value << 6) | (c & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return false;
    *pos = at;
    *scalar = value;
    return true;
}

static bool valid_utf8(const char *text, size_t len) {
    size_t pos = 0;
    uint32_t scalar;
    while (pos < len) {
        if (!utf8_next(text, len, &pos, &scalar)) return false;
    }
    return true;
}

typedef struct json_parser {
    const char *text;
    size_t len;
    size_t pos;
    char *err;
    size_t err_len;
    locale_t number_locale;
} json_parser_t;

static bool json_error(json_parser_t *parser, const char *message) {
    set_error(parser->err, parser->err_len, "%s", message);
    return false;
}

static bool json_error_at(json_parser_t *parser, const char *message, size_t pos) {
    set_error(parser->err, parser->err_len, "%s at byte %zu", message, pos);
    return false;
}

static void json_skip_space(json_parser_t *parser) {
    while (parser->pos < parser->len) {
        char c = parser->text[parser->pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++parser->pos;
    }
}

void purp_json_free(purp_json_value_t *v) {
    if (!v) return;
    switch (v->type) {
    case PURP_JSON_STRING:
        free(v->u.string.data);
        break;
    case PURP_JSON_ARRAY:
        for (size_t i = 0; i < v->u.array.len; ++i) purp_json_free(&v->u.array.items[i]);
        free(v->u.array.items);
        break;
    case PURP_JSON_OBJECT:
        for (size_t i = 0; i < v->u.object.len; ++i) {
            free(v->u.object.members[i].key);
            purp_json_free(&v->u.object.members[i].value);
        }
        free(v->u.object.members);
        break;
    default: break;
    }
    memset(v, 0, sizeof *v);
}

static bool json_hex4(json_parser_t *parser, uint32_t *value) {
    if (parser->len - parser->pos < 4) return json_error(parser, "truncated \\u escape");
    uint32_t result = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)parser->text[parser->pos++];
        unsigned int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return json_error(parser, "invalid hex digit in \\u escape");
        result = (result << 4) | digit;
    }
    *value = result;
    return true;
}

static void append_utf8(purp_strbuf_t *out, uint32_t value) {
    char bytes[4];
    size_t len;
    if (value < 0x80) {
        bytes[0] = (char)value;
        len = 1;
    } else if (value < 0x800) {
        bytes[0] = (char)(0xc0 | (value >> 6));
        bytes[1] = (char)(0x80 | (value & 0x3f));
        len = 2;
    } else if (value < 0x10000) {
        bytes[0] = (char)(0xe0 | (value >> 12));
        bytes[1] = (char)(0x80 | ((value >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (value & 0x3f));
        len = 3;
    } else {
        bytes[0] = (char)(0xf0 | (value >> 18));
        bytes[1] = (char)(0x80 | ((value >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((value >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (value & 0x3f));
        len = 4;
    }
    purp_strbuf_append_len(out, bytes, len);
}

static bool json_string(json_parser_t *parser, char **data, size_t *len) {
    purp_strbuf_t string = {0};
    ++parser->pos; /* opening quote */
    size_t start = parser->pos;
    while (parser->pos < parser->len) {
        unsigned char c = (unsigned char)parser->text[parser->pos++];
        if (c == '"') {
            purp_strbuf_append_len(&string, parser->text + start, parser->pos - start - 1);
            if (string.oom) break;
            *data = string.data;
            *len = string.len;
            return true;
        }
        if (c < 0x20) {
            purp_strbuf_free(&string);
            return json_error(parser, "unescaped control character in JSON string");
        }
        if (c != '\\') continue;
        purp_strbuf_append_len(&string, parser->text + start, parser->pos - start - 1);
        if (parser->pos >= parser->len) {
            purp_strbuf_free(&string);
            return json_error(parser, "unterminated string escape");
        }
        c = (unsigned char)parser->text[parser->pos++];
        char escaped;
        switch (c) {
        case '"': escaped = '"'; break;
        case '\\': escaped = '\\'; break;
        case '/': escaped = '/'; break;
        case 'b': escaped = '\b'; break;
        case 'f': escaped = '\f'; break;
        case 'n': escaped = '\n'; break;
        case 'r': escaped = '\r'; break;
        case 't': escaped = '\t'; break;
        case 'u': {
            uint32_t value;
            if (!json_hex4(parser, &value)) {
                purp_strbuf_free(&string);
                return false;
            }
            if (value >= 0xd800 && value <= 0xdbff) {
                if (parser->len - parser->pos < 2 || parser->text[parser->pos] != '\\' ||
                    parser->text[parser->pos + 1] != 'u') {
                    purp_strbuf_free(&string);
                    return json_error(parser, "unpaired UTF-16 high surrogate");
                }
                parser->pos += 2;
                uint32_t low;
                if (!json_hex4(parser, &low)) {
                    purp_strbuf_free(&string);
                    return false;
                }
                if (low < 0xdc00 || low > 0xdfff) {
                    purp_strbuf_free(&string);
                    return json_error(parser, "invalid UTF-16 low surrogate");
                }
                value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
            } else if (value >= 0xdc00 && value <= 0xdfff) {
                purp_strbuf_free(&string);
                return json_error(parser, "invalid Unicode scalar");
            }
            append_utf8(&string, value);
            start = parser->pos;
            continue;
        }
        default:
            purp_strbuf_free(&string);
            set_error(parser->err, parser->err_len, "unsupported escape '\\%c'", c);
            return false;
        }
        purp_strbuf_append_len(&string, &escaped, 1);
        start = parser->pos;
    }
    bool oom = string.oom;
    purp_strbuf_free(&string);
    return json_error(parser, oom ? "out of memory" : "unterminated JSON string");
}

static bool json_value(json_parser_t *parser, purp_json_value_t *out, unsigned int depth);

static bool json_container(json_parser_t *parser, purp_json_value_t *out, unsigned int depth,
                           bool object) {
    if (depth >= PURP_JSON_MAX_DEPTH) return json_error(parser, "JSON nesting too deep");
    out->type = object ? PURP_JSON_OBJECT : PURP_JSON_ARRAY;
    ++parser->pos;
    json_skip_space(parser);
    char close = object ? '}' : ']';
    if (parser->pos < parser->len && parser->text[parser->pos] == close) {
        ++parser->pos;
        return true;
    }
    size_t cap = 0;
    for (;;) {
        size_t len = object ? out->u.object.len : out->u.array.len;
        if (len == cap) {
            size_t element_size = object ? sizeof(purp_json_member_t) : sizeof(purp_json_value_t);
            if (cap > SIZE_MAX / 2 / element_size) return json_error(parser, "out of memory");
            size_t next = cap ? cap * 2 : 4;
            void *items = realloc(object ? (void *)out->u.object.members :
                                          (void *)out->u.array.items, next * element_size);
            if (!items) return json_error(parser, "out of memory");
            if (object) out->u.object.members = items;
            else out->u.array.items = items;
            cap = next;
        }
        purp_json_value_t *value;
        if (object) {
            purp_json_member_t *member = &out->u.object.members[len];
            memset(member, 0, sizeof *member);
            out->u.object.len = len + 1;
            if (parser->pos >= parser->len || parser->text[parser->pos] != '"')
                return json_error_at(parser, "expected '\"'", parser->pos);
            if (!json_string(parser, &member->key, &member->key_len)) return false;
            json_skip_space(parser);
            if (parser->pos >= parser->len || parser->text[parser->pos] != ':')
                return json_error_at(parser, "expected ':'", parser->pos);
            ++parser->pos;
            value = &member->value;
        } else {
            value = &out->u.array.items[len];
            memset(value, 0, sizeof *value);
            out->u.array.len = len + 1;
        }
        if (!json_value(parser, value, depth + 1)) return false;
        json_skip_space(parser);
        if (parser->pos >= parser->len ||
            (parser->text[parser->pos] != close && parser->text[parser->pos] != ','))
            return json_error_at(parser, object ? "expected ',' or '}'" : "expected ',' or ']'",
                                 parser->pos);
        char c = parser->text[parser->pos++];
        if (c == close) return true;
        json_skip_space(parser);
    }
}

static bool json_digit(char c) { return c >= '0' && c <= '9'; }

static bool json_number(json_parser_t *parser, purp_json_value_t *out) {
    size_t start = parser->pos;
    if (parser->text[parser->pos] == '-') ++parser->pos;
    if (parser->pos >= parser->len || !json_digit(parser->text[parser->pos]))
        return json_error_at(parser, "invalid number", start);
    while (parser->pos < parser->len && json_digit(parser->text[parser->pos])) ++parser->pos;
    if (parser->pos < parser->len && parser->text[parser->pos] == '.') {
        ++parser->pos;
        size_t digits = parser->pos;
        while (parser->pos < parser->len && json_digit(parser->text[parser->pos])) ++parser->pos;
        if (digits == parser->pos) return json_error_at(parser, "invalid fraction", start);
    }
    if (parser->pos < parser->len && (parser->text[parser->pos] == 'e' ||
                                    parser->text[parser->pos] == 'E')) {
        ++parser->pos;
        if (parser->pos < parser->len && (parser->text[parser->pos] == '+' ||
                                         parser->text[parser->pos] == '-')) ++parser->pos;
        size_t digits = parser->pos;
        while (parser->pos < parser->len && json_digit(parser->text[parser->pos])) ++parser->pos;
        if (digits == parser->pos) return json_error_at(parser, "invalid exponent", start);
    }
    size_t len = parser->pos - start;
    char local[128];
    char *number = len < sizeof local ? local : malloc(len + 1);
    if (!number) return json_error(parser, "out of memory");
    memcpy(number, parser->text + start, len);
    number[len] = '\0';
    double value = strtod_l(number, NULL, parser->number_locale);
    if (number != local) free(number);
    if (!isfinite(value)) return json_error(parser, "JSON number out of range");
    out->type = PURP_JSON_NUMBER;
    out->u.number = value;
    return true;
}

static bool json_value(json_parser_t *parser, purp_json_value_t *out, unsigned int depth) {
    json_skip_space(parser);
    if (parser->pos >= parser->len) return json_error(parser, "unexpected end of JSON");
    char c = parser->text[parser->pos];
    if (c == '{' || c == '[') return json_container(parser, out, depth, c == '{');
    if (c == '"') {
        out->type = PURP_JSON_STRING;
        return json_string(parser, &out->u.string.data, &out->u.string.len);
    }
    if (c == '-' || json_digit(c)) return json_number(parser, out);
    const char *literal;
    size_t literal_len;
    if (c == 't') { literal = "true"; literal_len = 4; }
    else if (c == 'f') { literal = "false"; literal_len = 5; }
    else if (c == 'n') { literal = "null"; literal_len = 4; }
    else {
        unsigned char byte = (unsigned char)c;
        if (byte == 0) set_error(parser->err, parser->err_len, "unexpected byte '\\0' at %zu", parser->pos);
        else if (byte < 0x20 || byte == 0x7f)
            set_error(parser->err, parser->err_len, "unexpected byte '\\u{%x}' at %zu", byte, parser->pos);
        else set_error(parser->err, parser->err_len, "unexpected byte '%c' at %zu", c, parser->pos);
        return false;
    }
    if (parser->len - parser->pos < literal_len ||
        memcmp(parser->text + parser->pos, literal, literal_len) != 0)
        return json_error_at(parser, "invalid literal", parser->pos);
    parser->pos += literal_len;
    if (c != 'n') {
        out->type = PURP_JSON_BOOL;
        out->u.boolean = c == 't';
    }
    return true;
}

bool purp_json_parse(const char *text, size_t len, purp_json_value_t *out, char *err,
                     size_t err_len) {
    memset(out, 0, sizeof *out);
    if (err && err_len) err[0] = '\0';
    if (!valid_utf8(text, len)) {
        set_error(err, err_len, "invalid UTF-8");
        return false;
    }
    locale_t number_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!number_locale) {
        set_error(err, err_len, "out of memory");
        return false;
    }
    json_parser_t parser = {
        .text = text, .len = len, .err = err, .err_len = err_len,
        .number_locale = number_locale,
    };
    bool ok = json_value(&parser, out, 0);
    if (ok) {
        json_skip_space(&parser);
        if (parser.pos != len) {
            set_error(err, err_len, "trailing characters at byte %zu", parser.pos);
            ok = false;
        }
    }
    freelocale(number_locale);
    if (!ok) purp_json_free(out);
    return ok;
}

const purp_json_value_t *purp_json_get(const purp_json_value_t *obj, const char *key) {
    if (!obj || obj->type != PURP_JSON_OBJECT) return NULL;
    size_t len = strlen(key);
    for (size_t i = obj->u.object.len; i > 0; --i) {
        const purp_json_member_t *member = &obj->u.object.members[i - 1];
        if (member->key_len == len && memcmp(member->key, key, len) == 0) return &member->value;
    }
    return NULL;
}

bool purp_json_as_bool(const purp_json_value_t *v, bool *out) {
    if (!v || v->type != PURP_JSON_BOOL) return false;
    *out = v->u.boolean;
    return true;
}

bool purp_json_as_i32(const purp_json_value_t *v, int32_t *out) {
    if (!v || v->type != PURP_JSON_NUMBER || !isfinite(v->u.number) ||
        v->u.number < INT32_MIN || v->u.number > INT32_MAX || trunc(v->u.number) != v->u.number)
        return false;
    *out = (int32_t)v->u.number;
    return true;
}

bool purp_json_as_u32(const purp_json_value_t *v, uint32_t *out) {
    if (!v || v->type != PURP_JSON_NUMBER || !isfinite(v->u.number) ||
        v->u.number < 0 || v->u.number > UINT32_MAX) return false;
    double rounded = round(v->u.number);
    if (fabs(v->u.number - rounded) > 1e-6) return false;
    *out = (uint32_t)rounded;
    return true;
}

bool purp_json_as_f64(const purp_json_value_t *v, double *out) {
    if (!v || v->type != PURP_JSON_NUMBER || !isfinite(v->u.number)) return false;
    *out = v->u.number;
    return true;
}

static void json_serialize_value(const purp_json_value_t *v, purp_strbuf_t *out) {
    switch (v->type) {
    case PURP_JSON_NULL: purp_strbuf_append(out, "null"); break;
    case PURP_JSON_BOOL: purp_strbuf_append(out, v->u.boolean ? "true" : "false"); break;
    case PURP_JSON_NUMBER: purp_strbuf_appendf(out, "%.17g", v->u.number); break;
    case PURP_JSON_STRING:
        append_json_string_len(out, v->u.string.data, v->u.string.len);
        break;
    case PURP_JSON_ARRAY:
        purp_strbuf_append(out, "[");
        for (size_t i = 0; i < v->u.array.len; ++i) {
            if (i) purp_strbuf_append(out, ",");
            json_serialize_value(&v->u.array.items[i], out);
        }
        purp_strbuf_append(out, "]");
        break;
    case PURP_JSON_OBJECT:
        purp_strbuf_append(out, "{");
        for (size_t i = 0; i < v->u.object.len; ++i) {
            const purp_json_member_t *member = &v->u.object.members[i];
            if (i) purp_strbuf_append(out, ",");
            append_json_string_len(out, member->key, member->key_len);
            purp_strbuf_append(out, ":");
            json_serialize_value(&member->value, out);
        }
        purp_strbuf_append(out, "}");
        break;
    }
}

void purp_json_serialize(const purp_json_value_t *v, purp_strbuf_t *out) {
    locale_t number_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!number_locale) { out->oom = true; return; }
    locale_t previous = uselocale(number_locale);
    json_serialize_value(v, out);
    uselocale(previous);
    freelocale(number_locale);
}

static bool unicode_space(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
           c == 0x205f || c == 0x3000;
}

static void trim_text(const char *text, size_t len, size_t *start, size_t *end) {
    size_t pos = 0;
    bool found = false;
    *start = *end = len;
    while (pos < len) {
        size_t before = pos;
        uint32_t scalar;
        if (!utf8_next(text, len, &pos, &scalar)) {
            /* Environment paths need not themselves be valid Unicode. */
            pos = before + 1;
            scalar = (unsigned char)text[before];
        }
        if (!unicode_space(scalar)) {
            if (!found) { *start = before; found = true; }
            *end = pos;
        }
    }
}

bool purp_ipc_parse_monitor_entries(const purp_json_value_t *array, purp_monitor_config_t *out,
                                    size_t *count, char *err, size_t err_len) {
    *count = 0;
    if (!array || array->type != PURP_JSON_ARRAY) {
        set_error(err, err_len, "%s", PURP_IPC_ERR_NO_MONITORS);
        return false;
    }
    if (array->u.array.len > PURP_IPC_MAX_MONITORS) {
        set_error(err, err_len, "monitors array must contain at most 32 entries");
        return false;
    }
    for (size_t i = 0; i < array->u.array.len; ++i) {
        const purp_json_value_t *entry = &array->u.array.items[i];
        if (entry->type != PURP_JSON_OBJECT) {
            set_error(err, err_len, "monitors[%zu] must be an object", i);
            return false;
        }
        purp_monitor_config_t config = { .enabled = true, .refresh_mhz = 60000 };
        const purp_json_value_t *name = purp_json_get(entry, "name");
        if (!name || name->type != PURP_JSON_STRING || !name->u.string.len) {
            set_error(err, err_len, "monitors[%zu].name must be a non-empty string", i);
            return false;
        }
        size_t name_len = name->u.string.len;
        if (name_len >= sizeof config.name) name_len = sizeof config.name - 1;
        /* Fixed-size records retain only complete UTF-8 scalars. */
        while (name_len && name_len < name->u.string.len &&
               ((unsigned char)name->u.string.data[name_len] & 0xc0) == 0x80) --name_len;
        memcpy(config.name, name->u.string.data, name_len);
        purp_json_as_bool(purp_json_get(entry, "enabled"), &config.enabled);
        purp_json_as_bool(purp_json_get(entry, "primary"), &config.primary);
        if (!purp_json_as_u32(purp_json_get(entry, "width"), &config.width) ||
            config.width < PURP_IPC_MIN_DIMENSION || config.width > PURP_IPC_MAX_DIMENSION) {
            set_error(err, err_len, "monitors[%zu].width must be 100..=32768", i);
            return false;
        }
        if (!purp_json_as_u32(purp_json_get(entry, "height"), &config.height) ||
            config.height < PURP_IPC_MIN_DIMENSION || config.height > PURP_IPC_MAX_DIMENSION) {
            set_error(err, err_len, "monitors[%zu].height must be 100..=32768", i);
            return false;
        }
        const purp_json_value_t *refresh = purp_json_get(entry, "refresh_mhz");
        if (refresh && refresh->type != PURP_JSON_NULL &&
            (!purp_json_as_i32(refresh, &config.refresh_mhz) ||
             config.refresh_mhz < PURP_IPC_MIN_REFRESH_MHZ ||
             config.refresh_mhz > PURP_IPC_MAX_REFRESH_MHZ)) {
            set_error(err, err_len, "monitors[%zu].refresh_mhz must be an integer in 1000..=1000000", i);
            return false;
        }
        if (!purp_json_as_f64(purp_json_get(entry, "scale"), &config.scale) ||
            config.scale < PURP_IPC_MIN_SCALE || config.scale > PURP_IPC_MAX_SCALE) {
            set_error(err, err_len, "monitors[%zu].scale must be 0.5..=4.0", i);
            return false;
        }
        const char *const axes[] = { "x", "y" };
        int32_t *const coordinates[] = { &config.x, &config.y };
        for (size_t axis = 0; axis < PURP_ARRAY_LEN(axes); ++axis) {
            const purp_json_value_t *coordinate = purp_json_get(entry, axes[axis]);
            if (coordinate && coordinate->type != PURP_JSON_NULL &&
                (!purp_json_as_i32(coordinate, coordinates[axis]) ||
                 *coordinates[axis] < PURP_IPC_COORD_MIN ||
                 *coordinates[axis] > PURP_IPC_COORD_MAX)) {
                set_error(err, err_len, "monitors[%zu].%s must be an integer in -131072..=131072", i, axes[axis]);
                return false;
            }
        }
        out[i] = config;
    }
    *count = array->u.array.len;
    return true;
}

static bool json_string_equal(const purp_json_value_t *value, const char *text) {
    size_t len = strlen(text);
    return value->u.string.len == len && memcmp(value->u.string.data, text, len) == 0;
}

bool purp_ipc_parse_command(const char *line, size_t len, purp_ipc_command_t *out, char *err,
                            size_t err_len) {
    memset(out, 0, sizeof *out);
    out->delta = 1;
    if (len > PURP_IPC_MAX_REQUEST_BYTES) {
        set_error(err, err_len, "%s", PURP_IPC_ERR_TOO_LARGE);
        return false;
    }
    if (!valid_utf8(line, len)) {
        set_error(err, err_len, "%s", PURP_IPC_ERR_NOT_UTF8);
        return false;
    }
    size_t start, end;
    trim_text(line, len, &start, &end);
    if (start == end) {
        set_error(err, err_len, "%s", PURP_IPC_ERR_EMPTY);
        return false;
    }
    purp_json_value_t root;
    if (!purp_json_parse(line + start, end - start, &root, err, err_len)) return false;
    bool ok = false;
    if (root.type != PURP_JSON_OBJECT) set_error(err, err_len, "%s", PURP_IPC_ERR_NOT_OBJECT);
    else {
        const purp_json_value_t *cmd = purp_json_get(&root, "cmd");
        if (!cmd || cmd->type != PURP_JSON_STRING) set_error(err, err_len, "%s", PURP_IPC_ERR_NO_CMD);
        else if (json_string_equal(cmd, "get_monitors")) {
            out->kind = PURP_IPC_GET_MONITORS;
            ok = true;
        } else if (json_string_equal(cmd, "apply_monitors")) {
            out->kind = PURP_IPC_APPLY_MONITORS;
            ok = purp_ipc_parse_monitor_entries(purp_json_get(&root, "monitors"), out->monitors,
                                               &out->monitor_count, err, err_len);
        } else if (json_string_equal(cmd, "focus_monitor") ||
                   json_string_equal(cmd, "move_window_to_monitor")) {
            out->kind = json_string_equal(cmd, "focus_monitor") ? PURP_IPC_FOCUS_MONITOR :
                                                                  PURP_IPC_MOVE_WINDOW_TO_MONITOR;
            purp_json_as_i32(purp_json_get(&root, "delta"), &out->delta);
            ok = true;
        } else set_error(err, err_len, "unknown IPC command '%.*s'", (int)cmd->u.string.len,
                         cmd->u.string.data);
    }
    purp_json_free(&root);
    return ok;
}

void purp_ipc_format_scale(double scale, char *out, size_t out_len) {
    if (!out_len) return;
    locale_t number_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!number_locale) { out[0] = '\0'; return; }
    locale_t previous = uselocale(number_locale);
    snprintf(out, out_len, "%.4f", round(scale * 10000.0) / 10000.0);
    uselocale(previous);
    freelocale(number_locale);
    char *point = strchr(out, '.');
    if (!point) return;
    size_t len = strlen(out);
    while (len > (size_t)(point - out) + 2 && out[len - 1] == '0') out[--len] = '\0';
}

void purp_ipc_serialize_monitors_response(const purp_monitor_info_t *monitors, size_t count,
                                          purp_strbuf_t *out) {
    purp_strbuf_append(out, "{\"ok\":true,\"monitors\":[");
    for (size_t i = 0; i < count; ++i) {
        const purp_monitor_info_t *m = &monitors[i];
        char scale[64];
        purp_ipc_format_scale(m->scale, scale, sizeof scale);
        if (i) purp_strbuf_append(out, ",");
        purp_strbuf_append(out, "{\"name\":");
        purp_strbuf_append_json_string(out, m->name);
        purp_strbuf_append(out, ",\"description\":");
        purp_strbuf_append_json_string(out, m->description);
        purp_strbuf_appendf(out, ",\"enabled\":%s,\"primary\":%s,\"width\":%" PRIu32
            ",\"height\":%" PRIu32 ",\"refresh_mhz\":%" PRId32 ",\"scale\":%s,\"x\":%" PRId32
            ",\"y\":%" PRId32 ",\"logical_width\":%" PRId32 ",\"logical_height\":%" PRId32
            ",\"available_modes\":[", m->enabled ? "true" : "false", m->primary ? "true" : "false",
            m->width, m->height, m->refresh_mhz, scale, m->x, m->y, m->logical_width, m->logical_height);
        for (size_t j = 0; j < m->mode_count && j < PURP_MAX_MODES; ++j) {
            const purp_display_mode_t *mode = &m->modes[j];
            if (j) purp_strbuf_append(out, ",");
            purp_strbuf_appendf(out, "{\"width\":%" PRIu32 ",\"height\":%" PRIu32
                ",\"refresh_mhz\":%" PRId32 ",\"preferred\":%s}", mode->width, mode->height,
                mode->refresh_mhz, mode->preferred ? "true" : "false");
        }
        purp_strbuf_append(out, "]}");
    }
    purp_strbuf_append(out, "]}\n");
}

void purp_ipc_serialize_error_response(const char *error, purp_strbuf_t *out) {
    purp_strbuf_append(out, "{\"ok\":false,\"error\":");
    purp_strbuf_append_json_string(out, error);
    purp_strbuf_append(out, "}\n");
}

void purp_ipc_serialize_ok_response(purp_strbuf_t *out) {
    purp_strbuf_append(out, "{\"ok\":true}\n");
}

static bool config_path(char *out, size_t out_len, const char *directory) {
    const char *base = getenv("XDG_CONFIG_HOME");
    int written;
    if (base && *base) written = snprintf(out, out_len, "%s/%s/%s", base, directory, PURP_MONITORS_FILE_NAME);
    else {
        base = getenv("HOME");
        if (!base || !*base) return false;
        written = snprintf(out, out_len, "%s/.config/%s/%s", base, directory, PURP_MONITORS_FILE_NAME);
    }
    if (written < 0 || (size_t)written >= out_len) { errno = ENAMETOOLONG; return false; }
    return true;
}

bool purp_monitors_config_path(char *out, size_t out_len) {
    const char *override = getenv(PURP_ENV_MONITORS_CONFIG);
    if (override) {
        size_t start, end;
        trim_text(override, strlen(override), &start, &end);
        if (start != end) {
            if (end - start >= out_len) { errno = ENAMETOOLONG; return false; }
            memcpy(out, override + start, end - start);
            out[end - start] = '\0';
            return true;
        }
    }
    return config_path(out, out_len, PURP_CONFIG_DIR_NAME);
}

bool purp_monitors_legacy_config_path(char *out, size_t out_len) {
    if (getenv(PURP_ENV_MONITORS_CONFIG)) return false;
    return config_path(out, out_len, PURP_LEGACY_CONFIG_DIR_NAME);
}

bool purp_parse_monitors_config_json(const char *text, size_t len, purp_monitor_config_t *out,
                                     size_t *count, char *err, size_t err_len) {
    *count = 0;
    purp_json_value_t root;
    if (!purp_json_parse(text, len, &root, err, err_len)) return false;
    const purp_json_value_t *entries = root.type == PURP_JSON_ARRAY ? &root : purp_json_get(&root, "monitors");
    bool ok;
    if (!entries || entries->type != PURP_JSON_ARRAY) {
        set_error(err, err_len, "monitors.json must be a JSON array or {\"monitors\": [...]}");
        ok = false;
    } else ok = purp_ipc_parse_monitor_entries(entries, out, count, err, err_len);
    purp_json_free(&root);
    return ok;
}

size_t purp_load_saved_monitors_from(const char *path, purp_monitor_config_t *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) PURP_WARN("Ignoring invalid monitors.json");
        return 0;
    }
    size_t capacity = PURP_IPC_MAX_REQUEST_BYTES + 1;
    char *text = malloc(capacity);
    if (!text) { close(fd); PURP_WARN("Ignoring invalid monitors.json"); return 0; }
    size_t len = 0;
    bool failed = false;
    while (len < capacity) {
        ssize_t got = read(fd, text + len, capacity - len);
        if (got > 0) len += (size_t)got;
        else if (got == 0) break;
        else if (errno != EINTR) { failed = true; break; }
    }
    close(fd);
    size_t count = 0;
    if (len > PURP_IPC_MAX_REQUEST_BYTES) PURP_WARN("Ignoring monitors.json exceeding 64 KiB limit");
    else if (failed || !purp_parse_monitors_config_json(text, len, out, &count, NULL, 0))
        PURP_WARN("Ignoring invalid monitors.json");
    free(text);
    return count;
}

size_t purp_load_saved_monitors(purp_monitor_config_t *out) {
    char path[PATH_MAX];
    if (!purp_monitors_config_path(path, sizeof path)) return 0;
    struct stat st;
    if (stat(path, &st) < 0 && errno == ENOENT) {
        char legacy[PATH_MAX];
        if (purp_monitors_legacy_config_path(legacy, sizeof legacy))
            return purp_load_saved_monitors_from(legacy, out);
    }
    return purp_load_saved_monitors_from(path, out);
}

void purp_serialize_monitors_config_json(const purp_monitor_config_t *entries, size_t count,
                                         purp_strbuf_t *out) {
    purp_strbuf_append(out, "{\n  \"monitors\": [\n");
    for (size_t i = 0; i < count; ++i) {
        const purp_monitor_config_t *m = &entries[i];
        char scale[64];
        purp_ipc_format_scale(m->scale, scale, sizeof scale);
        purp_strbuf_append(out, "    {\"name\": ");
        purp_strbuf_append_json_string(out, m->name);
        purp_strbuf_appendf(out, ", \"enabled\": %s, \"primary\": %s, \"width\": %" PRIu32
            ", \"height\": %" PRIu32 ", \"refresh_mhz\": %" PRId32 ", \"scale\": %s, \"x\": %" PRId32
            ", \"y\": %" PRId32 "}%s\n", m->enabled ? "true" : "false", m->primary ? "true" : "false",
            m->width, m->height, m->refresh_mhz, scale, m->x, m->y, i + 1 < count ? "," : "");
    }
    purp_strbuf_append(out, "  ]\n}\n");
}

static bool make_parent_directories(char *path) {
    for (char *p = path + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        int result = mkdir(path, 0700);
        int saved = errno;
        *p = '/';
        if (result < 0 && saved != EEXIST) { errno = saved; return false; }
    }
    return true;
}

bool purp_save_monitors_config_to(const char *path, const purp_monitor_config_t *entries,
                                  size_t count) {
    if (!path || !*path) { errno = EINVAL; return false; }
    purp_strbuf_t text = {0};
    purp_serialize_monitors_config_json(entries, count, &text);
    if (text.oom) { purp_strbuf_free(&text); errno = ENOMEM; return false; }
    const char *base = strrchr(path, '/');
    size_t parent_len = base ? (size_t)(base - path + 1) : 0;
    base = base ? base + 1 : path;
    purp_strbuf_t temporary = {0};
    purp_strbuf_append_len(&temporary, path, parent_len);
    purp_strbuf_appendf(&temporary, ".%s.tmp.%ld", base, (long)getpid());
    if (temporary.oom) {
        purp_strbuf_free(&text);
        purp_strbuf_free(&temporary);
        errno = ENOMEM;
        return false;
    }
    if (!make_parent_directories(temporary.data)) {
        int saved = errno;
        purp_strbuf_free(&text);
        purp_strbuf_free(&temporary);
        errno = saved;
        return false;
    }
    int fd;
    do fd = open(temporary.data, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    while (fd < 0 && errno == EINTR);
    bool ok = fd >= 0;
    int saved = errno;
    size_t offset = 0;
    while (ok && offset < text.len) {
        ssize_t sent = write(fd, text.data + offset, text.len - offset);
        if (sent > 0) offset += (size_t)sent;
        else if (sent < 0 && errno == EINTR) continue;
        else { saved = sent == 0 ? EIO : errno; ok = false; }
    }
    if (fd >= 0 && close(fd) < 0 && ok) { saved = errno; ok = false; }
    if (ok && rename(temporary.data, path) < 0) { saved = errno; ok = false; }
    if (!ok && fd >= 0) unlink(temporary.data);
    purp_strbuf_free(&text);
    purp_strbuf_free(&temporary);
    if (!ok) errno = saved;
    return ok;
}

bool purp_save_monitors_config(const purp_monitor_config_t *entries, size_t count) {
    char path[PATH_MAX];
    if (!purp_monitors_config_path(path, sizeof path)) return true;
    return purp_save_monitors_config_to(path, entries, count);
}

static void ipc_client_close(purp_ipc_client_t *client) {
    if (client->state != PURP_IPC_CLIENT_FREE && client->fd >= 0) close(client->fd);
    free(client->in);
    free(client->out);
    memset(client, 0, sizeof *client);
    client->fd = -1;
}

/* A pending nonblocking connect (including a full accept queue) is also a live socket. */
static bool socket_connects(const char *path) {
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return true; /* Never remove a socket when its liveness cannot be checked. */
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    memcpy(address.sun_path, path, strlen(path) + 1);
    int result;
    do result = connect(fd, (struct sockaddr *)&address, sizeof address);
    while (result < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    return result == 0 || (saved != ECONNREFUSED && saved != ENOENT);
}

static bool refresh_ipc_link(purp_ipc_server_t *srv, size_t slot, const char *runtime_dir,
                             const char *name, bool force) {
    char path[108];
    int len = snprintf(path, sizeof path, "%s/%s", runtime_dir, name);
    if (len < 0 || (size_t)len >= sizeof path || strcmp(path, srv->path) == 0) return false;
    if (!force && socket_connects(path)) return false;
    char temporary[160];
    len = snprintf(temporary, sizeof temporary, "%s.tmp.%ld", path, (long)getpid());
    if (len < 0 || (size_t)len >= sizeof temporary) return false;
    if (unlink(temporary) < 0 && errno != ENOENT) return false;
    if (symlink(srv->path, temporary) < 0) return false;
    if (rename(temporary, path) < 0) { unlink(temporary); return false; }
    memcpy(srv->link_paths[slot], path, strlen(path) + 1);
    return true;
}

bool purp_ipc_server_bind(purp_ipc_server_t *srv, const char *path, const char *runtime_dir,
                          bool is_nested, char *err, size_t err_len) {
    memset(srv, 0, sizeof *srv);
    srv->listen_fd = -1;
    for (size_t i = 0; i < PURP_IPC_MAX_CLIENTS; ++i) srv->clients[i].fd = -1;
    if (!path || !*path || strlen(path) >= sizeof srv->path) {
        errno = path && *path ? ENAMETOOLONG : EINVAL;
        set_error(err, err_len, "cannot bind IPC socket: %s", strerror(errno));
        return false;
    }
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            errno = EEXIST;
            set_error(err, err_len, "IPC path is not a socket");
            return false;
        }
        if (socket_connects(path)) {
            set_error(err, err_len, "ipc socket is live");
            errno = EADDRINUSE;
            return false;
        }
        if (unlink(path) < 0) {
            set_error(err, err_len, "cannot remove stale IPC socket: %s", strerror(errno));
            return false;
        }
    } else if (errno != ENOENT) {
        set_error(err, err_len, "cannot inspect IPC socket: %s", strerror(errno));
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { set_error(err, err_len, "cannot create IPC socket: %s", strerror(errno)); return false; }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    memcpy(address.sun_path, path, strlen(path) + 1);
    mode_t previous = umask(0177);
    int bound;
    do bound = bind(fd, (struct sockaddr *)&address, sizeof address);
    while (bound < 0 && errno == EINTR);
    int saved = errno;
    umask(previous);
    if (bound < 0 || chmod(path, 0600) < 0 || listen(fd, PURP_IPC_MAX_CLIENTS) < 0 ||
        setenv(PURP_ENV_IPC_SOCKET, path, 1) < 0) {
        if (bound == 0) saved = errno;
        close(fd);
        if (bound == 0) unlink(path);
        errno = saved;
        set_error(err, err_len, "cannot bind IPC socket: %s", strerror(saved));
        return false;
    }
    srv->listen_fd = fd;
    memcpy(srv->path, path, strlen(path) + 1);
    const char *update = getenv(PURP_ENV_UPDATE_IPC_LINK);
    bool force = !is_nested || (update && (!strcmp(update, "1") || !strcasecmp(update, "true")));
    if (runtime_dir && *runtime_dir) {
        refresh_ipc_link(srv, 0, runtime_dir, PURP_IPC_LINK_NAME, force);
        refresh_ipc_link(srv, 1, runtime_dir, PURP_IPC_LEGACY_LINK_NAME, force);
    }
    return true;
}

static void ipc_take_reply(purp_ipc_client_t *client, char *reply, size_t len, uint64_t now) {
    free(client->in);
    client->in = NULL;
    client->in_len = 0;
    free(client->out);
    client->out = reply;
    client->out_len = len;
    client->out_off = 0;
    client->state = PURP_IPC_CLIENT_WRITING;
    client->deadline_ms = now + PURP_IPC_WRITE_TIMEOUT_MS;
}

static void ipc_queue_reply(purp_ipc_client_t *client, const char *line, size_t len,
                            uint64_t now) {
    bool newline = len && line[len - 1] == '\n';
    if (len > SIZE_MAX - 2) { ipc_client_close(client); return; }
    char *reply = malloc(len + (newline ? 1 : 2));
    if (!reply) { ipc_client_close(client); return; }
    if (len) memcpy(reply, line, len);
    if (!newline) reply[len++] = '\n';
    reply[len] = '\0';
    ipc_take_reply(client, reply, len, now);
}

static void ipc_queue_error(purp_ipc_client_t *client, const char *error, uint64_t now) {
    purp_strbuf_t reply = {0};
    purp_ipc_serialize_error_response(error, &reply);
    if (reply.oom) ipc_client_close(client);
    else {
        ipc_take_reply(client, reply.data, reply.len, now);
        reply.data = NULL;
    }
    purp_strbuf_free(&reply);
}

static void ipc_write_client(purp_ipc_client_t *client, uint64_t now) {
    if (now >= client->deadline_ms) { ipc_client_close(client); return; }
    while (client->out_off < client->out_len) {
        if (purp_now_ms() >= client->deadline_ms) { ipc_client_close(client); return; }
        ssize_t sent = send(client->fd, client->out + client->out_off,
                            client->out_len - client->out_off, MSG_NOSIGNAL);
        if (sent > 0) { client->out_off += (size_t)sent; continue; }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        ipc_client_close(client);
        return;
    }
    ipc_client_close(client);
}

void purp_ipc_server_flush(purp_ipc_server_t *srv) {
    uint64_t now = purp_now_ms();
    for (size_t i = 0; i < PURP_IPC_MAX_CLIENTS; ++i) {
        purp_ipc_client_t *client = &srv->clients[i];
        if (client->state == PURP_IPC_CLIENT_AWAITING && now >= client->deadline_ms)
            ipc_queue_error(client, PURP_IPC_ERR_TIMEOUT, now);
        if (client->state == PURP_IPC_CLIENT_WRITING) ipc_write_client(client, now);
    }
}

static void ipc_accept_clients(purp_ipc_server_t *srv, uint64_t now) {
    if (srv->listen_fd < 0) return;
    for (size_t accepted = 0; accepted < 2 * PURP_IPC_MAX_CLIENTS; ++accepted) {
        int fd = accept4(srv->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return;
        }
        size_t slot;
        for (slot = 0; slot < PURP_IPC_MAX_CLIENTS; ++slot)
            if (srv->clients[slot].state == PURP_IPC_CLIENT_FREE) break;
        if (slot == PURP_IPC_MAX_CLIENTS) { close(fd); continue; }
        purp_ipc_client_t *client = &srv->clients[slot];
        memset(client, 0, sizeof *client);
        client->fd = fd;
        client->state = PURP_IPC_CLIENT_READING;
        if (++srv->next_serial == 0) ++srv->next_serial;
        client->serial = srv->next_serial;
        client->since_ms = now;
    }
}

static bool ipc_read_client(purp_ipc_client_t *client, uint64_t now) {
    /* deadline_ms is unused while READING; a nonzero value latches line/EOF completion. */
    if (client->overflowed || client->deadline_ms) return true;
    uint8_t chunk[PURP_IPC_READ_CHUNK];
    for (;;) {
        ssize_t got = recv(client->fd, chunk, sizeof chunk, 0);
        if (got > 0) {
            size_t len = (size_t)got;
            const uint8_t *newline = memchr(chunk, '\n', len);
            if (newline) len = (size_t)(newline - chunk);
            if (len > PURP_IPC_MAX_REQUEST_BYTES - client->in_len) {
                client->overflowed = true;
                return true;
            }
            if (len) {
                /* The header has no capacity member: allocate the bounded request once. */
                if (!client->in) client->in = malloc(PURP_IPC_MAX_REQUEST_BYTES);
                if (!client->in) { ipc_client_close(client); return false; }
                memcpy(client->in + client->in_len, chunk, len);
                client->in_len += len;
            }
            if (newline) {
                client->deadline_ms = 1;
                return true;
            }
            continue;
        }
        if (got == 0) { client->deadline_ms = 1; return true; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (now - client->since_ms < PURP_IPC_READ_TIMEOUT_MS) return false;
            client->deadline_ms = 1;
            return true;
        }
        ipc_client_close(client);
        return false;
    }
}

size_t purp_ipc_server_poll(purp_ipc_server_t *srv, bool locked, purp_ipc_request_t *out,
                            size_t max) {
    purp_ipc_server_flush(srv);
    uint64_t now = purp_now_ms();
    ipc_accept_clients(srv, now);
    size_t count = 0;
    for (size_t i = 0; i < PURP_IPC_MAX_CLIENTS; ++i) {
        purp_ipc_client_t *client = &srv->clients[i];
        if (client->state != PURP_IPC_CLIENT_READING || !ipc_read_client(client, now)) continue;
        purp_ipc_command_t pending;
        purp_ipc_command_t *command = count < max ? &out[count].command : &pending;
        char error[PURP_IPC_ERROR_MAX];
        bool parsed;
        if (client->overflowed) {
            set_error(error, sizeof error, "%s", PURP_IPC_ERR_TOO_LARGE);
            parsed = false;
        } else {
            parsed = purp_ipc_parse_command((const char *)client->in, client->in_len,
                                            command, error, sizeof error);
        }
        if (!parsed) ipc_queue_error(client, error, now);
        else if (locked && command->kind != PURP_IPC_GET_MONITORS)
            ipc_queue_error(client, PURP_IPC_ERR_LOCKED, now);
        else if (count < max) {
            out[count].client_slot = i;
            out[count].client_serial = client->serial;
            ++count;
            client->state = PURP_IPC_CLIENT_AWAITING;
            client->deadline_ms = now + PURP_IPC_REPLY_TIMEOUT_MS;
            free(client->in);
            client->in = NULL;
            client->in_len = 0;
        }
        if (client->state == PURP_IPC_CLIENT_WRITING) ipc_write_client(client, now);
    }
    return count;
}

void purp_ipc_server_respond(purp_ipc_server_t *srv, const purp_ipc_request_t *req,
                             const char *json_line) {
    if (req->client_slot >= PURP_IPC_MAX_CLIENTS) return;
    purp_ipc_client_t *client = &srv->clients[req->client_slot];
    if (client->state != PURP_IPC_CLIENT_AWAITING || client->serial != req->client_serial) return;
    uint64_t now = purp_now_ms();
    if (now >= client->deadline_ms) ipc_queue_error(client, PURP_IPC_ERR_TIMEOUT, now);
    else ipc_queue_reply(client, json_line, strlen(json_line), now);
    if (client->state == PURP_IPC_CLIENT_WRITING) ipc_write_client(client, now);
}

int purp_ipc_server_fd(const purp_ipc_server_t *srv) {
    return srv->listen_fd;
}

void purp_ipc_server_close(purp_ipc_server_t *srv) {
    for (size_t i = 0; i < PURP_IPC_MAX_CLIENTS; ++i) ipc_client_close(&srv->clients[i]);
    if (srv->listen_fd >= 0 && srv->path[0]) close(srv->listen_fd);
    srv->listen_fd = -1;
    for (size_t i = 0; i < PURP_ARRAY_LEN(srv->link_paths); ++i) {
        if (!srv->link_paths[i][0]) continue;
        char target[108];
        ssize_t len = readlink(srv->link_paths[i], target, sizeof target);
        if (len >= 0 && (size_t)len == strlen(srv->path) &&
            memcmp(target, srv->path, (size_t)len) == 0) unlink(srv->link_paths[i]);
        srv->link_paths[i][0] = '\0';
    }
    if (srv->path[0]) unlink(srv->path);
    const char *exported = getenv(PURP_ENV_IPC_SOCKET);
    if (exported && strcmp(exported, srv->path) == 0) unsetenv(PURP_ENV_IPC_SOCKET);
    srv->path[0] = '\0';
}
