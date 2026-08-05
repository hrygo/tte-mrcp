#include "funasr_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK_TRUE(label, expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
            failures++; \
        } \
    } while (0)

static void check_string(
    const char *label,
    const char *json,
    apr_size_t json_size,
    const char *key,
    apr_size_t limit,
    funasr_json_status_e expected_status,
    const char *expected)
{
    char *value = NULL;
    apr_size_t value_size = 0;
    funasr_json_status_e status = funasr_json_get_string_heap(
        json,
        json_size,
        key,
        limit,
        &value,
        &value_size);

    if (status != expected_status) {
        fprintf(stderr,
                "FAIL %s: status=%d expected=%d\n",
                label,
                (int)status,
                (int)expected_status);
        failures++;
    }

    if (expected) {
        if (!value || value_size != strlen(expected) ||
            memcmp(value, expected, value_size) != 0 ||
            value[value_size] != '\0') {
            fprintf(stderr, "FAIL %s: decoded value mismatch\n", label);
            failures++;
        }
    } else if (value != NULL || value_size != 0) {
        fprintf(stderr, "FAIL %s: failure returned owned output\n", label);
        failures++;
    }

    funasr_json_heap_free(value);
}

static void test_bounded_integer(void)
{
    const char json[] = {'{', '"', 'c', 'o', 'd', 'e', '"', ':', '1', '2', '}'};
    int value = 0;

    CHECK_TRUE("bounded integer parses without nul terminator",
               funasr_json_get_int(
                   json,
                   sizeof(json),
                   "code",
                   &value) == FUNASR_JSON_OK);
    CHECK_TRUE("bounded integer value", value == 12);

    CHECK_TRUE("invalid integer is rejected",
               funasr_json_get_int(
                   "{\"code\":12x}",
                   sizeof("{\"code\":12x}") - 1,
                   "code",
                   &value) == FUNASR_JSON_INVALID);
    CHECK_TRUE("missing integer key",
               funasr_json_get_int(
                   "{\"message\":1}",
                   sizeof("{\"message\":1}") - 1,
                   "code",
                   &value) == FUNASR_JSON_NOT_FOUND);
}

static void test_strings_and_unicode(void)
{
    check_string(
        "escaped quote and newline",
        "{\"text\":\"say \\\"hi\\\"\\n\"}",
        sizeof("{\"text\":\"say \\\"hi\\\"\\n\"}") - 1,
        "text",
        64,
        FUNASR_JSON_OK,
        "say \"hi\"\n");
    check_string(
        "BMP unicode",
        "{\"text\":\"\\u4f60\\u597d\"}",
        sizeof("{\"text\":\"\\u4f60\\u597d\"}") - 1,
        "text",
        64,
        FUNASR_JSON_OK,
        "\xe4\xbd\xa0\xe5\xa5\xbd");
    check_string(
        "surrogate pair",
        "{\"text\":\"\\ud83d\\ude03\"}",
        sizeof("{\"text\":\"\\ud83d\\ude03\"}") - 1,
        "text",
        64,
        FUNASR_JSON_OK,
        "\xf0\x9f\x98\x83");
    check_string(
        "raw UTF-8",
        "{\"text\":\"\xe6\x94\xb6\xe5\x88\xb0\"}",
        sizeof("{\"text\":\"\xe6\x94\xb6\xe5\x88\xb0\"}") - 1,
        "text",
        64,
        FUNASR_JSON_OK,
        "\xe6\x94\xb6\xe5\x88\xb0");
}

static void test_invalid_and_limited_strings(void)
{
    check_string(
        "invalid hex",
        "{\"text\":\"\\u12xz\"}",
        sizeof("{\"text\":\"\\u12xz\"}") - 1,
        "text",
        64,
        FUNASR_JSON_INVALID,
        NULL);
    check_string(
        "unmatched high surrogate",
        "{\"text\":\"\\ud83d\"}",
        sizeof("{\"text\":\"\\ud83d\"}") - 1,
        "text",
        64,
        FUNASR_JSON_INVALID,
        NULL);
    check_string(
        "unmatched low surrogate",
        "{\"text\":\"\\ude03\"}",
        sizeof("{\"text\":\"\\ude03\"}") - 1,
        "text",
        64,
        FUNASR_JSON_INVALID,
        NULL);
    check_string(
        "unterminated string",
        "{\"text\":\"abc}",
        sizeof("{\"text\":\"abc}") - 1,
        "text",
        64,
        FUNASR_JSON_INVALID,
        NULL);
    check_string(
        "missing key",
        "{\"message\":\"abc\"}",
        sizeof("{\"message\":\"abc\"}") - 1,
        "text",
        64,
        FUNASR_JSON_NOT_FOUND,
        NULL);
    check_string(
        "decoded output limit",
        "{\"text\":\"abcd\"}",
        sizeof("{\"text\":\"abcd\"}") - 1,
        "text",
        3,
        FUNASR_JSON_LIMIT_EXCEEDED,
        NULL);
}

int main(void)
{
    test_bounded_integer();
    test_strings_and_unicode();
    test_invalid_and_limited_strings();

    if (failures != 0) {
        fprintf(stderr, "%d JSON assertion(s) failed\n", failures);
        return 1;
    }

    printf("PASS test_json_unescape\n");
    return 0;
}
