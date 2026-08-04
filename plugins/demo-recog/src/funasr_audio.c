#include "funasr_audio.h"

#include <limits.h>
#include <string.h>

static unsigned char funasr_virtual_byte(
    const funasr_resample_state_t *state,
    const unsigned char *input,
    apr_size_t offset)
{
    if (offset < state->partial_size) {
        return state->partial[offset];
    }
    return input[offset - state->partial_size];
}

static int16_t funasr_virtual_sample(
    const funasr_resample_state_t *state,
    const unsigned char *input,
    apr_size_t byte_offset)
{
    apr_uint16_t value;

    value = (apr_uint16_t)funasr_virtual_byte(state, input, byte_offset);
    value |= (apr_uint16_t)(
        (apr_uint16_t)funasr_virtual_byte(state, input, byte_offset + 1) << 8);
    return (int16_t)value;
}

static int16_t funasr_average_sample(int16_t first, int16_t second)
{
    int sum;

    sum = (int)first + (int)second;
    if (sum >= 0) {
        sum += 1;
    }
    return (int16_t)(sum / 2);
}

apr_size_t funasr_pcm_bytes_for_ms(
    apr_uint32_t rate,
    apr_uint16_t channels,
    apr_uint16_t width,
    apr_uint32_t duration_ms)
{
    apr_uint64_t bytes;

    if (rate == 0 || channels == 0 || width == 0 || duration_ms == 0) {
        return 0;
    }

    bytes = (apr_uint64_t)rate;
    bytes *= channels;
    bytes *= width;
    bytes *= duration_ms;
    bytes /= 1000U;
    if (bytes > (apr_uint64_t)((apr_size_t)-1)) {
        return 0;
    }
    return (apr_size_t)bytes;
}

apr_size_t funasr_resample_output_capacity(
    apr_size_t input_bytes,
    apr_uint16_t channels)
{
    apr_size_t frame_bytes;
    apr_size_t frames;
    apr_size_t maximum;

    if (channels == 0 || channels > FUNASR_MAX_CHANNELS) {
        return 0;
    }

    frame_bytes = (apr_size_t)channels * FUNASR_SAMPLE_WIDTH_BYTES;
    if (input_bytes > (apr_size_t)-1 - (frame_bytes - 1)) {
        return 0;
    }
    frames = (input_bytes + frame_bytes - 1) / frame_bytes;
    if (frames > (apr_size_t)-1 / frame_bytes / 2) {
        return 0;
    }
    maximum = frames * frame_bytes * 2;
    return maximum;
}

apt_bool_t funasr_resample_8k_to_16k_into(
    funasr_resample_state_t *state,
    const void *input,
    apr_size_t input_bytes,
    apr_uint16_t channels,
    void *output,
    apr_size_t output_capacity,
    apr_size_t *output_bytes)
{
    const unsigned char *input_bytes_ptr;
    funasr_resample_state_t next_state;
    int16_t *output_samples;
    apr_size_t frame_bytes;
    apr_size_t total_bytes;
    apr_size_t frame_count;
    apr_size_t required_bytes;
    apr_size_t frame_index;
    apr_size_t channel_index;
    apr_size_t consumed_bytes;
    apr_size_t remainder_bytes;

    if (!output_bytes) {
        return FALSE;
    }
    *output_bytes = 0;

    if (!state || channels == 0 || channels > FUNASR_MAX_CHANNELS ||
        (!input && input_bytes != 0) || (!output && output_capacity != 0)) {
        return FALSE;
    }

    frame_bytes = (apr_size_t)channels * FUNASR_SAMPLE_WIDTH_BYTES;
    if (state->partial_size >= frame_bytes ||
        input_bytes > (apr_size_t)-1 - state->partial_size) {
        return FALSE;
    }

    total_bytes = state->partial_size + input_bytes;
    frame_count = total_bytes / frame_bytes;
    if (frame_count > (apr_size_t)-1 / frame_bytes / 2) {
        return FALSE;
    }
    required_bytes = frame_count * frame_bytes * 2;
    if (required_bytes > output_capacity || (required_bytes != 0 && !output)) {
        return FALSE;
    }

    input_bytes_ptr = (const unsigned char *)input;
    output_samples = (int16_t *)output;
    next_state = *state;

    for (frame_index = 0; frame_index < frame_count; ++frame_index) {
        for (channel_index = 0; channel_index < channels; ++channel_index) {
            apr_size_t current_offset;
            apr_size_t next_offset;
            apr_size_t output_offset;
            int16_t current_sample;
            int16_t next_sample;

            current_offset = frame_index * frame_bytes +
                channel_index * FUNASR_SAMPLE_WIDTH_BYTES;
            current_sample = funasr_virtual_sample(
                state,
                input_bytes_ptr,
                current_offset);
            if (frame_index + 1 < frame_count) {
                next_offset = current_offset + frame_bytes;
                next_sample = funasr_virtual_sample(
                    state,
                    input_bytes_ptr,
                    next_offset);
            } else {
                next_sample = current_sample;
            }

            output_offset = frame_index * channels * 2 + channel_index;
            output_samples[output_offset] = current_sample;
            output_samples[output_offset + channels] =
                funasr_average_sample(current_sample, next_sample);
        }
    }

    if (frame_count != 0 && next_state.previous_valid) {
        for (channel_index = 0; channel_index < channels; ++channel_index) {
            int16_t first_sample;

            first_sample = funasr_virtual_sample(
                state,
                input_bytes_ptr,
                channel_index * FUNASR_SAMPLE_WIDTH_BYTES);
            output_samples[channel_index] = funasr_average_sample(
                next_state.previous[channel_index],
                first_sample);
        }
    }

    if (frame_count != 0) {
        apr_size_t last_frame_offset;

        last_frame_offset = (frame_count - 1) * frame_bytes;
        for (channel_index = 0; channel_index < channels; ++channel_index) {
            next_state.previous[channel_index] = funasr_virtual_sample(
                state,
                input_bytes_ptr,
                last_frame_offset +
                    channel_index * FUNASR_SAMPLE_WIDTH_BYTES);
        }
        next_state.previous_valid = TRUE;
    }

    consumed_bytes = frame_count * frame_bytes;
    remainder_bytes = total_bytes - consumed_bytes;
    next_state.partial_size = remainder_bytes;
    for (frame_index = 0; frame_index < remainder_bytes; ++frame_index) {
        next_state.partial[frame_index] = funasr_virtual_byte(
            state,
            input_bytes_ptr,
            consumed_bytes + frame_index);
    }

    *state = next_state;
    *output_bytes = required_bytes;
    return TRUE;
}
