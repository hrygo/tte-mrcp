#ifndef FUNASR_AUDIO_H
#define FUNASR_AUDIO_H

#include "apt.h"

#include <apr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUNASR_OUTPUT_SAMPLE_RATE 16000U
#define FUNASR_SAMPLE_WIDTH_BYTES 2U
#define FUNASR_MAX_CHANNELS 2U

typedef struct funasr_resample_state_t {
    unsigned char partial[FUNASR_SAMPLE_WIDTH_BYTES * FUNASR_MAX_CHANNELS];
    apr_size_t partial_size;
    int16_t previous[FUNASR_MAX_CHANNELS];
    apt_bool_t previous_valid;
} funasr_resample_state_t;

apr_size_t funasr_pcm_bytes_for_ms(
    apr_uint32_t rate,
    apr_uint16_t channels,
    apr_uint16_t width,
    apr_uint32_t duration_ms);

apr_size_t funasr_resample_output_capacity(
    apr_size_t input_bytes,
    apr_uint16_t channels);

apt_bool_t funasr_resample_8k_to_16k_into(
    funasr_resample_state_t *state,
    const void *input,
    apr_size_t input_bytes,
    apr_uint16_t channels,
    void *output,
    apr_size_t output_capacity,
    apr_size_t *output_bytes);

#ifdef __cplusplus
}
#endif

#endif
