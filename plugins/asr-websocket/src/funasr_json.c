#include "funasr_json.h"

#include "apt.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static apt_bool_t funasr_json_is_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static funasr_json_status_e funasr_json_find_value(
    const char *json,
    apr_size_t json_size,
    const char *key,
    apr_size_t *value_offset)
{
    apr_size_t key_size;
    apr_size_t index;

    if (!json || !key || !value_offset) {
        return FUNASR_JSON_INVALID;
    }

    key_size = strlen(key);
    index = 0;
    while (index < json_size) {
        apr_size_t string_start;
        apr_size_t string_end;
        apr_size_t cursor;
        apt_bool_t escaped;

        if (json[index] != '"') {
            ++index;
            continue;
        }

        string_start = index + 1;
        string_end = string_start;
        escaped = FALSE;
        while (string_end < json_size) {
            char value;

            value = json[string_end];
            if (escaped) {
                escaped = FALSE;
            } else if (value == '\\') {
                escaped = TRUE;
            } else if (value == '"') {
                break;
            }
            ++string_end;
        }
        if (string_end >= json_size || escaped) {
            return FUNASR_JSON_INVALID;
        }

        cursor = string_end + 1;
        while (cursor < json_size && funasr_json_is_space(json[cursor])) {
            ++cursor;
        }
        if (string_end - string_start == key_size &&
            memcmp(json + string_start, key, key_size) == 0 &&
            cursor < json_size && json[cursor] == ':') {
            ++cursor;
            while (cursor < json_size && funasr_json_is_space(json[cursor])) {
                ++cursor;
            }
            if (cursor >= json_size) {
                return FUNASR_JSON_INVALID;
            }
            *value_offset = cursor;
            return FUNASR_JSON_OK;
        }

        index = string_end + 1;
    }

    return FUNASR_JSON_NOT_FOUND;
}

static apt_bool_t funasr_json_is_delimiter(char value)
{
    return value == ',' || value == '}' || value == ']';
}

funasr_json_status_e funasr_json_get_int(
    const char *json,
    apr_size_t json_size,
    const char *key,
    int *value)
{
    funasr_json_status_e status;
    apr_size_t index;
    apr_uint64_t magnitude;
    apr_uint64_t limit;
    apt_bool_t negative;

    if (!value) {
        return FUNASR_JSON_INVALID;
    }

    status = funasr_json_find_value(json, json_size, key, &index);
    if (status != FUNASR_JSON_OK) {
        return status;
    }

    negative = FALSE;
    if (json[index] == '-') {
        negative = TRUE;
        ++index;
    }
    if (index >= json_size || !isdigit((unsigned char)json[index])) {
        return FUNASR_JSON_INVALID;
    }

    limit = negative ? (apr_uint64_t)INT_MAX + 1U : (apr_uint64_t)INT_MAX;
    magnitude = 0;
    while (index < json_size && isdigit((unsigned char)json[index])) {
        apr_uint64_t digit;

        digit = (apr_uint64_t)(json[index] - '0');
        if (magnitude > (limit - digit) / 10U) {
            return FUNASR_JSON_INVALID;
        }
        magnitude = magnitude * 10U + digit;
        ++index;
    }

    while (index < json_size && funasr_json_is_space(json[index])) {
        ++index;
    }
    if (index < json_size && !funasr_json_is_delimiter(json[index])) {
        return FUNASR_JSON_INVALID;
    }

    if (negative) {
        if (magnitude == (apr_uint64_t)INT_MAX + 1U) {
            *value = INT_MIN;
        } else {
            *value = -(int)magnitude;
        }
    } else {
        *value = (int)magnitude;
    }
    return FUNASR_JSON_OK;
}

static apt_bool_t funasr_json_hex_value(char value, apr_uint32_t *digit)
{
    if (value >= '0' && value <= '9') {
        *digit = (apr_uint32_t)(value - '0');
        return TRUE;
    }
    if (value >= 'a' && value <= 'f') {
        *digit = (apr_uint32_t)(value - 'a' + 10);
        return TRUE;
    }
    if (value >= 'A' && value <= 'F') {
        *digit = (apr_uint32_t)(value - 'A' + 10);
        return TRUE;
    }
    return FALSE;
}

static apt_bool_t funasr_json_parse_hex4(
    const char *json,
    apr_size_t json_size,
    apr_size_t offset,
    apr_uint32_t *codepoint)
{
    int index;
    apr_uint32_t value;

    if (offset > json_size || json_size - offset < 4) {
        return FALSE;
    }

    value = 0;
    for (index = 0; index < 4; ++index) {
        apr_uint32_t digit;

        if (!funasr_json_hex_value(json[offset + index], &digit)) {
            return FALSE;
        }
        value = (value << 4) | digit;
    }
    *codepoint = value;
    return TRUE;
}

static apr_size_t funasr_json_utf8_size(apr_uint32_t codepoint)
{
    if (codepoint <= 0x7FU) {
        return 1;
    }
    if (codepoint <= 0x7FFU) {
        return 2;
    }
    if (codepoint <= 0xFFFFU) {
        return 3;
    }
    if (codepoint <= 0x10FFFFU) {
        return 4;
    }
    return 0;
}

static void funasr_json_write_utf8(
    apr_uint32_t codepoint,
    char *output,
    apr_size_t *output_index)
{
    apr_size_t index;

    index = *output_index;
    if (codepoint <= 0x7FU) {
        output[index++] = (char)codepoint;
    } else if (codepoint <= 0x7FFU) {
        output[index++] = (char)(0xC0U | (codepoint >> 6));
        output[index++] = (char)(0x80U | (codepoint & 0x3FU));
    } else if (codepoint <= 0xFFFFU) {
        output[index++] = (char)(0xE0U | (codepoint >> 12));
        output[index++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[index++] = (char)(0x80U | (codepoint & 0x3FU));
    } else {
        output[index++] = (char)(0xF0U | (codepoint >> 18));
        output[index++] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
        output[index++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[index++] = (char)(0x80U | (codepoint & 0x3FU));
    }
    *output_index = index;
}

static funasr_json_status_e funasr_json_decode_string(
    const char *json,
    apr_size_t json_size,
    apr_size_t start,
    char *output,
    apr_size_t *decoded_size)
{
    apr_size_t index;
    apr_size_t output_index;

    if (start >= json_size || json[start] != '"' || !decoded_size) {
        return FUNASR_JSON_INVALID;
    }

    index = start + 1;
    output_index = 0;
    while (index < json_size) {
        unsigned char value;

        value = (unsigned char)json[index++];
        if (value == '"') {
            if (output) {
                output[output_index] = '\0';
            }
            *decoded_size = output_index;
            return FUNASR_JSON_OK;
        }
        if (value < 0x20U) {
            return FUNASR_JSON_INVALID;
        }
        if (value != '\\') {
            if (output) {
                output[output_index] = (char)value;
            }
            ++output_index;
            continue;
        }

        if (index >= json_size) {
            return FUNASR_JSON_INVALID;
        }
        value = (unsigned char)json[index++];
        switch (value) {
            case '"':
            case '\\':
            case '/':
                if (output) {
                    output[output_index] = (char)value;
                }
                ++output_index;
                break;
            case 'b':
                if (output) {
                    output[output_index] = '\b';
                }
                ++output_index;
                break;
            case 'f':
                if (output) {
                    output[output_index] = '\f';
                }
                ++output_index;
                break;
            case 'n':
                if (output) {
                    output[output_index] = '\n';
                }
                ++output_index;
                break;
            case 'r':
                if (output) {
                    output[output_index] = '\r';
                }
                ++output_index;
                break;
            case 't':
                if (output) {
                    output[output_index] = '\t';
                }
                ++output_index;
                break;
            case 'u':
            {
                apr_uint32_t codepoint;
                apr_size_t encoded_size;

                if (!funasr_json_parse_hex4(
                        json,
                        json_size,
                        index,
                        &codepoint)) {
                    return FUNASR_JSON_INVALID;
                }
                index += 4;

                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    apr_uint32_t low_surrogate;

                    if (index + 6 > json_size ||
                        json[index] != '\\' || json[index + 1] != 'u' ||
                        !funasr_json_parse_hex4(
                            json,
                            json_size,
                            index + 2,
                            &low_surrogate) ||
                        low_surrogate < 0xDC00U ||
                        low_surrogate > 0xDFFFU) {
                        return FUNASR_JSON_INVALID;
                    }
                    index += 6;
                    codepoint = 0x10000U +
                        ((codepoint - 0xD800U) << 10) +
                        (low_surrogate - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    return FUNASR_JSON_INVALID;
                }

                encoded_size = funasr_json_utf8_size(codepoint);
                if (encoded_size == 0 ||
                    output_index > (apr_size_t)-1 - encoded_size) {
                    return FUNASR_JSON_INVALID;
                }
                if (output) {
                    funasr_json_write_utf8(
                        codepoint,
                        output,
                        &output_index);
                } else {
                    output_index += encoded_size;
                }
                break;
            }
            default:
                return FUNASR_JSON_INVALID;
        }
    }

    return FUNASR_JSON_INVALID;
}

funasr_json_status_e funasr_json_get_string_heap(
    const char *json,
    apr_size_t json_size,
    const char *key,
    apr_size_t output_limit,
    char **value,
    apr_size_t *value_size)
{
    funasr_json_status_e status;
    apr_size_t value_offset;
    apr_size_t decoded_size;
    char *decoded;

    if (!value || !value_size) {
        return FUNASR_JSON_INVALID;
    }
    *value = NULL;
    *value_size = 0;

    status = funasr_json_find_value(
        json,
        json_size,
        key,
        &value_offset);
    if (status != FUNASR_JSON_OK) {
        return status;
    }

    decoded_size = 0;
    status = funasr_json_decode_string(
        json,
        json_size,
        value_offset,
        NULL,
        &decoded_size);
    if (status != FUNASR_JSON_OK) {
        return status;
    }
    if (decoded_size > output_limit) {
        return FUNASR_JSON_LIMIT_EXCEEDED;
    }
    if (decoded_size == (apr_size_t)-1) {
        return FUNASR_JSON_LIMIT_EXCEEDED;
    }

    decoded = (char *)malloc(decoded_size + 1);
    if (!decoded) {
        return FUNASR_JSON_NO_MEMORY;
    }

    status = funasr_json_decode_string(
        json,
        json_size,
        value_offset,
        decoded,
        &decoded_size);
    if (status != FUNASR_JSON_OK) {
        free(decoded);
        return status;
    }

    *value = decoded;
    *value_size = decoded_size;
    return FUNASR_JSON_OK;
}

void funasr_json_heap_free(void *value)
{
    free(value);
}
