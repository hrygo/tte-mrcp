#include "tts_websocket_json.h"

#include <limits.h>
#include <string.h>

#define TTS_WEBSOCKET_JSON_MAX_NESTING 64

static size_t skip_whitespace(const char *json, size_t len, size_t pos)
{
    while (pos < len && (json[pos] == ' ' || json[pos] == '\t' ||
        json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }
    return pos;
}

static int skip_string(const char *json, size_t len, size_t *pos)
{
    size_t i = *pos;

    if (i >= len || json[i++] != '"') {
        return 0;
    }
    while (i < len) {
        if (json[i] == '\\') {
            size_t hex_count;
            if (++i >= len) {
                return 0;
            }
            if (json[i] == 'u') {
                for (hex_count = 0; hex_count < 4; hex_count++) {
                    i++;
                    if (i >= len || !((json[i] >= '0' && json[i] <= '9') ||
                        (json[i] >= 'a' && json[i] <= 'f') ||
                        (json[i] >= 'A' && json[i] <= 'F'))) {
                        return 0;
                    }
                }
            }
            else if (json[i] != '"' && json[i] != '\\' && json[i] != '/' &&
                json[i] != 'b' && json[i] != 'f' && json[i] != 'n' &&
                json[i] != 'r' && json[i] != 't') {
                return 0;
            }
        }
        else if (json[i] == '"') {
            *pos = i + 1;
            return 1;
        }
        else if ((unsigned char)json[i] < 0x20) {
            return 0;
        }
        i++;
    }
    return 0;
}

static int skip_value(const char *json, size_t len, size_t *pos, unsigned int depth);

static int skip_object(const char *json, size_t len, size_t *pos, unsigned int depth)
{
    size_t i = *pos + 1;

    i = skip_whitespace(json, len, i);
    if (i < len && json[i] == '}') {
        *pos = i + 1;
        return 1;
    }
    for (;;) {
        if (i >= len || json[i] != '"' || !skip_string(json, len, &i)) {
            return 0;
        }
        i = skip_whitespace(json, len, i);
        if (i >= len || json[i++] != ':') {
            return 0;
        }
        if (!skip_value(json, len, &i, depth)) {
            return 0;
        }
        i = skip_whitespace(json, len, i);
        if (i >= len) {
            return 0;
        }
        if (json[i] == '}') {
            *pos = i + 1;
            return 1;
        }
        if (json[i++] != ',') {
            return 0;
        }
        i = skip_whitespace(json, len, i);
        if (i >= len || json[i] == '}') {
            return 0;
        }
    }
}

static int skip_array(const char *json, size_t len, size_t *pos, unsigned int depth)
{
    size_t i = skip_whitespace(json, len, *pos + 1);

    if (i < len && json[i] == ']') {
        *pos = i + 1;
        return 1;
    }
    for (;;) {
        if (!skip_value(json, len, &i, depth)) {
            return 0;
        }
        i = skip_whitespace(json, len, i);
        if (i >= len) {
            return 0;
        }
        if (json[i] == ']') {
            *pos = i + 1;
            return 1;
        }
        if (json[i++] != ',') {
            return 0;
        }
        i = skip_whitespace(json, len, i);
        if (i >= len || json[i] == ']') {
            return 0;
        }
    }
}

static int skip_number(const char *json, size_t len, size_t *pos)
{
    size_t i = *pos;

    if (i < len && json[i] == '-') i++;
    if (i >= len) return 0;
    if (json[i] == '0') i++;
    else if (json[i] >= '1' && json[i] <= '9') {
        do { i++; } while (i < len && json[i] >= '0' && json[i] <= '9');
    }
    else return 0;
    if (i < len && json[i] == '.') {
        i++;
        if (i >= len || json[i] < '0' || json[i] > '9') return 0;
        do { i++; } while (i < len && json[i] >= '0' && json[i] <= '9');
    }
    if (i < len && (json[i] == 'e' || json[i] == 'E')) {
        i++;
        if (i < len && (json[i] == '+' || json[i] == '-')) i++;
        if (i >= len || json[i] < '0' || json[i] > '9') return 0;
        do { i++; } while (i < len && json[i] >= '0' && json[i] <= '9');
    }
    *pos = i;
    return 1;
}

static int skip_literal(const char *json, size_t len, size_t *pos, const char *literal)
{
    size_t literal_len = strlen(literal);
    if (*pos + literal_len > len || memcmp(json + *pos, literal, literal_len) != 0) {
        return 0;
    }
    *pos += literal_len;
    return 1;
}

static int skip_value(const char *json, size_t len, size_t *pos, unsigned int depth)
{
    size_t i = skip_whitespace(json, len, *pos);
    int result;

    if (i >= len) return 0;
    if (json[i] == '"') result = skip_string(json, len, &i);
    else if (json[i] == '{') result = depth < TTS_WEBSOCKET_JSON_MAX_NESTING && skip_object(json, len, &i, depth + 1);
    else if (json[i] == '[') result = depth < TTS_WEBSOCKET_JSON_MAX_NESTING && skip_array(json, len, &i, depth + 1);
    else if (json[i] == 't') result = skip_literal(json, len, &i, "true");
    else if (json[i] == 'f') result = skip_literal(json, len, &i, "false");
    else if (json[i] == 'n') result = skip_literal(json, len, &i, "null");
    else result = skip_number(json, len, &i);
    if (!result) return 0;
    *pos = i;
    return 1;
}

static int key_matches(const char *json, size_t start, size_t end, const char *key)
{
    size_t key_len = strlen(key);
    size_t i;
    if (end - start != key_len) return 0;
    for (i = start; i < end; i++) if (json[i] == '\\') return 0;
    return memcmp(json + start, key, key_len) == 0;
}

int tts_websocket_json_get_uint(const char *json, size_t len, const char *key, unsigned int *value)
{
    size_t pos;
    unsigned int candidate = 0;
    int found = 0;

    if (!json || !key || !*key || !value) return 0;
    pos = skip_whitespace(json, len, 0);
    if (pos >= len || json[pos++] != '{') return 0;
    for (;;) {
        size_t key_start, key_end;
        int match;
        pos = skip_whitespace(json, len, pos);
        if (pos >= len) return 0;
        if (json[pos] == '}') {
            pos = skip_whitespace(json, len, pos + 1);
            if (!found || pos != len) return 0;
            *value = candidate;
            return 1;
        }
        if (json[pos] != '"') return 0;
        key_start = pos + 1;
        if (!skip_string(json, len, &pos)) return 0;
        key_end = pos - 1;
        match = key_matches(json, key_start, key_end, key);
        pos = skip_whitespace(json, len, pos);
        if (pos >= len || json[pos++] != ':') return 0;
        pos = skip_whitespace(json, len, pos);
        if (match) {
            unsigned int parsed = 0;
            int has_digit = 0;
            while (pos < len && json[pos] >= '0' && json[pos] <= '9') {
                unsigned int digit = (unsigned int)(json[pos++] - '0');
                if (parsed > (UINT_MAX - digit) / 10U) return 0;
                parsed = parsed * 10U + digit;
                has_digit = 1;
            }
            pos = skip_whitespace(json, len, pos);
            if (!has_digit || pos >= len || (json[pos] != ',' && json[pos] != '}')) return 0;
            candidate = parsed;
            found = 1;
        }
        else if (!skip_value(json, len, &pos, 0)) return 0;
        pos = skip_whitespace(json, len, pos);
        if (pos >= len) return 0;
        if (json[pos] == '}') continue;
        if (json[pos++] != ',') return 0;
        pos = skip_whitespace(json, len, pos);
        if (pos >= len || json[pos] == '}') return 0;
    }
}
