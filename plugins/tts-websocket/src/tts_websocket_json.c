#include "tts_websocket_json.h"

#include <limits.h>
#include <string.h>

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

    if (i >= len || json[i] != '"') {
        return 0;
    }
    i++;
    while (i < len) {
        if (json[i] == '\\') {
            i++;
            if (i >= len) {
                return 0;
            }
            if (json[i] == 'u') {
                size_t hex_count;
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

static int skip_container(const char *json, size_t len, size_t *pos)
{
    size_t i = *pos;
    char closing;

    if (i >= len || (json[i] != '{' && json[i] != '[')) {
        return 0;
    }
    closing = (json[i] == '{') ? '}' : ']';
    i++;
    while (i < len) {
        if (json[i] == '"') {
            if (!skip_string(json, len, &i)) {
                return 0;
            }
            continue;
        }
        if (json[i] == '{' || json[i] == '[') {
            if (!skip_container(json, len, &i)) {
                return 0;
            }
            continue;
        }
        if (json[i] == '}' || json[i] == ']') {
            if (json[i] != closing) {
                return 0;
            }
            *pos = i + 1;
            return 1;
        }
        i++;
    }
    return 0;
}

static int key_matches(const char *json, size_t start, size_t end, const char *key)
{
    size_t key_len = strlen(key);
    size_t i;

    if (end - start != key_len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        if (json[i] == '\\') {
            return 0;
        }
    }
    return memcmp(json + start, key, key_len) == 0;
}

static int skip_value(const char *json, size_t len, size_t *pos)
{
    size_t i = *pos;

    if (i >= len) {
        return 0;
    }
    if (json[i] == '"') {
        if (!skip_string(json, len, &i)) {
            return 0;
        }
        *pos = i;
        return 1;
    }
    if (json[i] == '{' || json[i] == '[') {
        return skip_container(json, len, pos);
    }
    while (i < len && json[i] != ',' && json[i] != '}') {
        i++;
    }
    if (i == *pos) {
        return 0;
    }
    *pos = i;
    return 1;
}

int tts_websocket_json_get_uint(
    const char *json,
    size_t len,
    const char *key,
    unsigned int *value)
{
    size_t pos;

    if (!json || !key || !*key || !value) {
        return 0;
    }
    pos = skip_whitespace(json, len, 0);
    if (pos >= len || json[pos++] != '{') {
        return 0;
    }

    for (;;) {
        size_t key_start;
        size_t key_end;
        int match;

        pos = skip_whitespace(json, len, pos);
        if (pos >= len) {
            return 0;
        }
        if (json[pos] == '}') {
            return 0;
        }
        if (json[pos] != '"') {
            return 0;
        }
        key_start = pos + 1;
        if (!skip_string(json, len, &pos)) {
            return 0;
        }
        key_end = pos - 1;
        match = key_matches(json, key_start, key_end, key);

        pos = skip_whitespace(json, len, pos);
        if (pos >= len || json[pos++] != ':') {
            return 0;
        }
        pos = skip_whitespace(json, len, pos);

        if (match) {
            unsigned int parsed = 0;
            int has_digit = 0;

            while (pos < len && json[pos] >= '0' && json[pos] <= '9') {
                unsigned int digit = (unsigned int)(json[pos] - '0');
                if (parsed > (UINT_MAX - digit) / 10U) {
                    return 0;
                }
                parsed = parsed * 10U + digit;
                has_digit = 1;
                pos++;
            }
            pos = skip_whitespace(json, len, pos);
            if (!has_digit || pos >= len || (json[pos] != ',' && json[pos] != '}')) {
                return 0;
            }
            *value = parsed;
            return 1;
        }

        if (!skip_value(json, len, &pos)) {
            return 0;
        }
        pos = skip_whitespace(json, len, pos);
        if (pos >= len || json[pos] == '}') {
            return 0;
        }
        if (json[pos++] != ',') {
            return 0;
        }
    }
}
