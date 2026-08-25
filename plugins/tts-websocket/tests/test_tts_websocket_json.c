#include <stdio.h>
#include <string.h>

#include "../src/tts_websocket_json.h"

static int test_top_level_sample_rate_ignores_sentence_text(void)
{
    const char json[] =
        "{\"type\":\"audio.start\",\"sentence_text\":\"sample_rate:24000\","
        "\"sample_rate\":8000}";
    unsigned int value = 0;

    return tts_websocket_json_get_uint(
        json, sizeof(json) - 1, "sample_rate", &value) && value == 8000;
}

static int test_escaped_quote_string_does_not_mask_sample_rate(void)
{
    const char json[] =
        "{\"sentence_text\":\"escaped \\\"sample_rate\\\":24000\","
        "\"sample_rate\":8000}";
    unsigned int value = 0;

    return tts_websocket_json_get_uint(
        json, sizeof(json) - 1, "sample_rate", &value) && value == 8000;
}

static int test_missing_or_non_numeric_sample_rate_fails(void)
{
    const char missing[] = "{\"type\":\"audio.start\"}";
    const char string_value[] = "{\"sample_rate\":\"8000\"}";
    unsigned int value = 0;

    return !tts_websocket_json_get_uint(
               missing, sizeof(missing) - 1, "sample_rate", &value) &&
        !tts_websocket_json_get_uint(
            string_value, sizeof(string_value) - 1, "sample_rate", &value);
}

static int test_missing_sample_rate_uses_8khz_default(void)
{
    const char json[] = "{\"type\":\"audio.start\"}";
    unsigned int value = 24000;

    return tts_websocket_json_get_uint_status(
        json, sizeof(json) - 1, "sample_rate", &value) == 0 && value == 24000;
}

static int test_invalid_sample_rate_is_not_missing(void)
{
    const char string_value[] = "{\"sample_rate\":\"8000\"}";
    const char overflow[] = "{\"sample_rate\":4294967296}";
    unsigned int value = 24000;

    if (tts_websocket_json_get_uint_status(string_value,
            sizeof(string_value) - 1, "sample_rate", &value) != -1 || value != 24000) {
        return 0;
    }
    return tts_websocket_json_get_uint_status(overflow,
        sizeof(overflow) - 1, "sample_rate", &value) == -1 && value == 24000;
}

static int test_explicit_sample_rate_is_valid(void)
{
    const char rate_8khz[] = "{\"sample_rate\":8000}";
    const char rate_24khz[] = "{\"sample_rate\":24000}";
    unsigned int value = 0;

    if (tts_websocket_json_get_uint_status(
            rate_8khz, sizeof(rate_8khz) - 1, "sample_rate", &value) != 1 || value != 8000) {
        return 0;
    }
    return tts_websocket_json_get_uint_status(
        rate_24khz, sizeof(rate_24khz) - 1, "sample_rate", &value) == 1 && value == 24000;
}

static int test_sample_rate_resolution_per_audio_start(void)
{
    return tts_websocket_sample_rate_resolve(24000, 0, 0) == 8000 &&
        tts_websocket_sample_rate_resolve(8000, 1, 24000) == 24000 &&
        tts_websocket_sample_rate_resolve(24000, -1, 0) == 24000 &&
        tts_websocket_sample_rate_resolve(8000, 1, 16000) == 8000;
}

static int test_overflowing_sample_rate_fails_without_writing_value(void)
{
    const char json[] = "{\"sample_rate\":4294967296}";
    unsigned int value = 8000;

    return !tts_websocket_json_get_uint(
               json, sizeof(json) - 1, "sample_rate", &value) &&
        value == 8000;
}

static int test_mismatched_nested_container_fails_without_writing_value(void)
{
    const char json[] =
        "{\"type\":\"audio.start\",\"meta\":{\"x\":0],\"sample_rate\":8000}";
    unsigned int value = 24000;

    return !tts_websocket_json_get_uint(
               json, sizeof(json) - 1, "sample_rate", &value) &&
        value == 24000;
}

static int test_malformed_nested_value_fails_without_writing_value(void)
{
    const char trailing_token[] =
        "{\"meta\":{\"x\":0 garbage},\"sample_rate\":8000}";
    const char missing_comma[] =
        "{\"meta\":{\"x\":0 \"y\":1},\"sample_rate\":8000}";
    unsigned int value = 24000;

    if (tts_websocket_json_get_uint(trailing_token, sizeof(trailing_token) - 1,
            "sample_rate", &value) || value != 24000) {
        return 0;
    }
    return !tts_websocket_json_get_uint(missing_comma, sizeof(missing_comma) - 1,
        "sample_rate", &value) && value == 24000;
}

static int test_malformed_member_after_sample_rate_fails_without_writing_value(void)
{
    const char json[] = "{\"sample_rate\":8000,\"meta\":{\"x\":0 garbage}}";
    unsigned int value = 24000;

    return !tts_websocket_json_get_uint(json, sizeof(json) - 1,
        "sample_rate", &value) && value == 24000;
}

static int test_excessive_nesting_fails_without_writing_value(void)
{
    char json[256];
    size_t pos = 0;
    size_t i;
    unsigned int value = 24000;

    memcpy(json + pos, "{\"meta\":", 8); pos += 8;
    for (i = 0; i < 65; i++) json[pos++] = '[';
    json[pos++] = '0';
    for (i = 0; i < 65; i++) json[pos++] = ']';
    memcpy(json + pos, ",\"sample_rate\":8000}", 20); pos += 20;
    return !tts_websocket_json_get_uint(json, pos, "sample_rate", &value) && value == 24000;
}

static int test_escaped_sample_rate_key(void)
{
    const char escaped[] = "{\"sample_\\u0072ate\":8000}";
    const char non_ascii[] = "{\"sample_\\u00e9ate\":8000}";
    unsigned int value = 24000;
    if (!tts_websocket_json_get_uint(escaped, sizeof(escaped) - 1, "sample_rate", &value) || value != 8000) return 0;
    value = 24000;
    return !tts_websocket_json_get_uint(non_ascii, sizeof(non_ascii) - 1, "sample_rate", &value) && value == 24000;
}

int main(void)
{
    if (!test_top_level_sample_rate_ignores_sentence_text()) {
        fprintf(stderr, "test_top_level_sample_rate_ignores_sentence_text failed\n");
        return 1;
    }
    if (!test_escaped_quote_string_does_not_mask_sample_rate()) {
        fprintf(stderr, "test_escaped_quote_string_does_not_mask_sample_rate failed\n");
        return 1;
    }
    if (!test_missing_or_non_numeric_sample_rate_fails()) {
        fprintf(stderr, "test_missing_or_non_numeric_sample_rate_fails failed\n");
        return 1;
    }
    if (!test_missing_sample_rate_uses_8khz_default()) {
        fprintf(stderr, "test_missing_sample_rate_uses_8khz_default failed\n");
        return 1;
    }
    if (!test_invalid_sample_rate_is_not_missing()) {
        fprintf(stderr, "test_invalid_sample_rate_is_not_missing failed\n");
        return 1;
    }
    if (!test_explicit_sample_rate_is_valid()) {
        fprintf(stderr, "test_explicit_sample_rate_is_valid failed\n");
        return 1;
    }
    if (!test_sample_rate_resolution_per_audio_start()) {
        fprintf(stderr, "test_sample_rate_resolution_per_audio_start failed\n");
        return 1;
    }
    if (!test_overflowing_sample_rate_fails_without_writing_value()) {
        fprintf(stderr, "test_overflowing_sample_rate_fails_without_writing_value failed\n");
        return 1;
    }
    if (!test_mismatched_nested_container_fails_without_writing_value()) {
        fprintf(stderr, "test_mismatched_nested_container_fails_without_writing_value failed\n");
        return 1;
    }
    if (!test_malformed_nested_value_fails_without_writing_value()) {
        fprintf(stderr, "test_malformed_nested_value_fails_without_writing_value failed\n");
        return 1;
    }
    if (!test_malformed_member_after_sample_rate_fails_without_writing_value()) {
        fprintf(stderr, "test_malformed_member_after_sample_rate_fails_without_writing_value failed\n");
        return 1;
    }
    if (!test_excessive_nesting_fails_without_writing_value()) {
        fprintf(stderr, "test_excessive_nesting_fails_without_writing_value failed\n");
        return 1;
    }
    if (!test_escaped_sample_rate_key()) {
        fprintf(stderr, "test_escaped_sample_rate_key failed\n");
        return 1;
    }
    puts("13/13 JSON parser tests passed");
    return 0;
}
