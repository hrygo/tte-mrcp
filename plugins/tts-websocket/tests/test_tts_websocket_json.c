#include <stdio.h>

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
    puts("3/3 JSON parser tests passed");
    return 0;
}
