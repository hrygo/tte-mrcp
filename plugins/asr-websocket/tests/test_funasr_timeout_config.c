#include "funasr_timeout_config.h"

#include <stdio.h>

static int failures = 0;

#define CHECK_TRUE(label, expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
            failures++; \
        } \
    } while (0)

static void test_defaults_and_valid_values(void)
{
    funasr_timeout_value_e status;

    CHECK_TRUE(
        "missing value uses default",
        funasr_timeout_ms_parse(
            NULL,
            FUNASR_FIRST_AUDIO_RESULT_TIMEOUT_US,
            &status) == FUNASR_FIRST_AUDIO_RESULT_TIMEOUT_US);
    CHECK_TRUE(
        "missing value status",
        status == FUNASR_TIMEOUT_VALUE_DEFAULT);
    CHECK_TRUE(
        "milliseconds convert to microseconds",
        funasr_timeout_ms_parse(
            "7000",
            FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US,
            &status) == 7000000);
    CHECK_TRUE(
        "configured value status",
        status == FUNASR_TIMEOUT_VALUE_CONFIGURED);
}

static void test_invalid_values_fall_back(void)
{
    static const char *const values[] = {
        "",
        "0",
        "-1",
        "1000ms",
        "9223372036854775807"
    };
    funasr_timeout_value_e status;
    apr_size_t index;

    for (index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        CHECK_TRUE(
            "invalid value uses fallback",
            funasr_timeout_ms_parse(
                values[index],
                FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US,
                &status) == FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US);
        CHECK_TRUE(
            "invalid value status",
            status == FUNASR_TIMEOUT_VALUE_INVALID);
    }
}

int main(void)
{
    test_defaults_and_valid_values();
    test_invalid_values_fall_back();

    if (failures != 0) {
        fprintf(stderr, "%d timeout configuration assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS test_funasr_timeout_config\n");
    return 0;
}
