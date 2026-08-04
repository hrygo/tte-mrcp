#include "funasr_audio.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK_TRUE(label, expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
            failures++; \
        } \
    } while (0)

#define CHECK_SIZE(label, actual, expected) \
    do { \
        apr_size_t actual_value = (actual); \
        apr_size_t expected_value = (expected); \
        if (actual_value != expected_value) { \
            fprintf(stderr, "FAIL %s:%d: %s actual=%lu expected=%lu\n", \
                    __FILE__, __LINE__, label, \
                    (unsigned long)actual_value, (unsigned long)expected_value); \
            failures++; \
        } \
    } while (0)

static void check_samples(
    const char *label,
    const apr_int16_t *actual,
    const apr_int16_t *expected,
    apr_size_t count)
{
    apr_size_t i;

    for (i = 0; i < count; ++i) {
        if (actual[i] != expected[i]) {
            fprintf(stderr,
                    "FAIL %s sample[%lu] actual=%d expected=%d\n",
                    label,
                    (unsigned long)i,
                    actual[i],
                    expected[i]);
            failures++;
        }
    }
}

static void test_pcm_duration_formulas(void)
{
    CHECK_SIZE("mono one-second ring",
               funasr_pcm_bytes_for_ms(16000, 1, 2, 1000),
               32000);
    CHECK_SIZE("stereo one-second ring",
               funasr_pcm_bytes_for_ms(16000, 2, 2, 1000),
               64000);
    CHECK_SIZE("mono 200ms chunk",
               funasr_pcm_bytes_for_ms(16000, 1, 2, 200),
               6400);
    CHECK_SIZE("stereo 200ms chunk",
               funasr_pcm_bytes_for_ms(16000, 2, 2, 200),
               12800);
    CHECK_SIZE("invalid format returns zero",
               funasr_pcm_bytes_for_ms(16000, 0, 2, 1000),
               0);
}

static void test_mono_interpolation_and_continuity(void)
{
    const apr_int16_t first_input[] = {1000};
    const apr_int16_t second_input[] = {3000};
    const apr_int16_t first_expected[] = {1000, 1000};
    const apr_int16_t second_expected[] = {2000, 3000};
    apr_int16_t output[4];
    apr_size_t output_bytes = 0;
    funasr_resample_state_t state;

    memset(&state, 0, sizeof(state));
    CHECK_TRUE("first mono frame resamples",
               funasr_resample_8k_to_16k_into(
                   &state,
                   first_input,
                   sizeof(first_input),
                   1,
                   output,
                   sizeof(output),
                   &output_bytes) == TRUE);
    CHECK_SIZE("first mono frame size", output_bytes, sizeof(first_expected));
    check_samples("first mono frame", output, first_expected, 2);

    CHECK_TRUE("second mono frame resamples",
               funasr_resample_8k_to_16k_into(
                   &state,
                   second_input,
                   sizeof(second_input),
                   1,
                   output,
                   sizeof(output),
                   &output_bytes) == TRUE);
    CHECK_SIZE("second mono frame size", output_bytes, sizeof(second_expected));
    check_samples("second mono frame", output, second_expected, 2);
}

static void test_odd_byte_is_carried(void)
{
    const apr_int16_t input[] = {1000, 2000};
    const apr_int16_t expected[] = {1000, 1000, 1500, 2000};
    const unsigned char *bytes = (const unsigned char *)input;
    apr_int16_t output[4];
    apr_size_t output_bytes = 99;
    funasr_resample_state_t state;

    memset(&state, 0, sizeof(state));
    CHECK_TRUE("odd prefix accepted",
               funasr_resample_8k_to_16k_into(
                   &state,
                   bytes,
                   1,
                   1,
                   output,
                   sizeof(output),
                   &output_bytes) == TRUE);
    CHECK_SIZE("odd prefix produces no partial sample", output_bytes, 0);

    CHECK_TRUE("remaining bytes complete samples",
               funasr_resample_8k_to_16k_into(
                   &state,
                   bytes + 1,
                   sizeof(input) - 1,
                   1,
                   output,
                   sizeof(output),
                   &output_bytes) == TRUE);
    CHECK_SIZE("completed mono bytes", output_bytes, sizeof(expected));
    check_samples("odd byte carry", output, expected, 4);
}

static void test_split_stereo_frame_preserves_layout(void)
{
    const apr_int16_t input[] = {100, -100, 300, -300};
    const apr_int16_t expected[] = {100, -100, 100, -100, 200, -200, 300, -300};
    const unsigned char *bytes = (const unsigned char *)input;
    apr_int16_t output[8];
    apr_size_t output_bytes = 0;
    funasr_resample_state_t state;

    memset(&state, 0, sizeof(state));
    CHECK_TRUE("partial stereo frame accepted",
               funasr_resample_8k_to_16k_into(
                   &state,
                   bytes,
                   3,
                   2,
                   output,
                   sizeof(output),
                   &output_bytes) == TRUE);
    CHECK_SIZE("partial stereo frame is carried", output_bytes, 0);

    CHECK_TRUE("stereo remainder resamples",
               funasr_resample_8k_to_16k_into(
                   &state,
                   bytes + 3,
                   sizeof(input) - 3,
                   2,
                   output,
                   sizeof(output),
                   &output_bytes) == TRUE);
    CHECK_SIZE("stereo output size", output_bytes, sizeof(expected));
    check_samples("stereo channel layout", output, expected, 8);
}

static void test_validation_and_capacity_failure_preserve_state(void)
{
    const apr_int16_t input[] = {1000, 2000};
    apr_int16_t output[4];
    apr_size_t output_bytes = 99;
    funasr_resample_state_t state;
    funasr_resample_state_t before;

    memset(&state, 0, sizeof(state));
    before = state;
    CHECK_TRUE("zero channels rejected",
               funasr_resample_8k_to_16k_into(
                   &state,
                   input,
                   sizeof(input),
                   0,
                   output,
                   sizeof(output),
                   &output_bytes) == FALSE);
    CHECK_SIZE("zero channels output", output_bytes, 0);
    CHECK_TRUE("invalid channel does not mutate state",
               memcmp(&state, &before, sizeof(state)) == 0);

    output_bytes = 99;
    CHECK_TRUE("three channels rejected",
               funasr_resample_8k_to_16k_into(
                   &state,
                   input,
                   sizeof(input),
                   3,
                   output,
                   sizeof(output),
                   &output_bytes) == FALSE);
    CHECK_SIZE("three channels output", output_bytes, 0);

    output_bytes = 99;
    before = state;
    CHECK_TRUE("small output buffer rejected",
               funasr_resample_8k_to_16k_into(
                   &state,
                   input,
                   sizeof(input),
                   1,
                   output,
                   sizeof(input),
                   &output_bytes) == FALSE);
    CHECK_SIZE("small output output", output_bytes, 0);
    CHECK_TRUE("capacity failure does not mutate state",
               memcmp(&state, &before, sizeof(state)) == 0);

    CHECK_SIZE("capacity includes a partial mono sample",
               funasr_resample_output_capacity(1, 1),
               4);
    CHECK_SIZE("capacity includes a partial stereo frame",
               funasr_resample_output_capacity(3, 2),
               8);
}

static void test_chunked_ramp_matches_one_shot(void)
{
    const apr_int16_t input[] = {100, 300, 500, 700, 900, 1100};
    apr_int16_t one_shot[12];
    apr_int16_t chunked[12];
    apr_size_t one_shot_bytes;
    apr_size_t first_bytes;
    apr_size_t second_bytes;
    funasr_resample_state_t one_shot_state;
    funasr_resample_state_t chunked_state;

    memset(&one_shot_state, 0, sizeof(one_shot_state));
    memset(&chunked_state, 0, sizeof(chunked_state));
    CHECK_TRUE("one-shot ramp resamples",
               funasr_resample_8k_to_16k_into(
                   &one_shot_state,
                   input,
                   sizeof(input),
                   1,
                   one_shot,
                   sizeof(one_shot),
                   &one_shot_bytes) == TRUE);
    CHECK_TRUE("first ramp chunk resamples",
               funasr_resample_8k_to_16k_into(
                   &chunked_state,
                   input,
                   2 * sizeof(input[0]),
                   1,
                   chunked,
                   sizeof(chunked),
                   &first_bytes) == TRUE);
    CHECK_TRUE("second ramp chunk resamples",
               funasr_resample_8k_to_16k_into(
                   &chunked_state,
                   input + 2,
                   sizeof(input) - 2 * sizeof(input[0]),
                   1,
                   (unsigned char *)chunked + first_bytes,
                   sizeof(chunked) - first_bytes,
                   &second_bytes) == TRUE);
    CHECK_SIZE("chunked ramp byte count",
               first_bytes + second_bytes,
               one_shot_bytes);
    CHECK_TRUE("chunked ramp equals one-shot",
               memcmp(one_shot, chunked, one_shot_bytes) == 0);
}

int main(void)
{
    test_pcm_duration_formulas();
    test_mono_interpolation_and_continuity();
    test_odd_byte_is_carried();
    test_split_stereo_frame_preserves_layout();
    test_validation_and_capacity_failure_preserve_state();
    test_chunked_ramp_matches_one_shot();

    if (failures != 0) {
        fprintf(stderr, "%d resample assertion(s) failed\n", failures);
        return 1;
    }

    printf("PASS test_resample\n");
    return 0;
}
