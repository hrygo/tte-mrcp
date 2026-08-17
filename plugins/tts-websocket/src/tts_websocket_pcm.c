#include "tts_websocket_pcm.h"

#include <string.h>

/* μ-law encoder (ITU-T G.711): 14-bit signed sample to 8-bit μ-law byte.
 * Mirrors the reference algorithm used by the plugin audio pipeline:
 * sign/magnitude split, BIAS = 0x84, 3-bit exponent, 4-bit mantissa,
 * final bitwise inversion. */
static unsigned char linear_to_ulaw(short sample)
{
    int magnitude;
    int exponent;
    int mantissa;
    int exponent_mask;
    unsigned char ulaw_byte;

    /* Handle sign and magnitude, including the -32768 edge case:
     * signed right shifts are implementation-defined in C, so compare
     * instead of negating with a shift. */
    int sign = (sample < 0) ? 0x80 : 0;
    if (sign) {
        magnitude = (sample == -32768) ? 32767 : -sample;
    } else {
        magnitude = sample;
    }

    /* Add BIAS = 0x84 = 132 and clamp to 15 bits. */
    magnitude += 0x84;
    if (magnitude > 32767) {
        magnitude = 32767;
    }

    /* Exponent (3 bits). */
    exponent = 7;
    for (exponent_mask = 0x4000; !(magnitude & exponent_mask); exponent_mask >>= 1) {
        exponent--;
    }

    /* Mantissa (4 bits). */
    mantissa = (magnitude >> (exponent + 3)) & 0x0F;

    /* Combine S EEEE MMMM, then invert (μ-law property). */
    ulaw_byte = (unsigned char)(sign | (exponent << 4) | mantissa);
    return (unsigned char)~ulaw_byte;
}

static short read_le16(const unsigned char *p)
{
    return (short)((unsigned short)((unsigned short)(p[1] << 8) | p[0]));
}

size_t tts_websocket_pcm_accumulate(
    unsigned char *carry,
    size_t *carry_len,
    const unsigned char *input,
    size_t input_len,
    unsigned char *output,
    size_t output_capacity,
    size_t alignment)
{
    size_t old_carry_len;
    size_t produced = 0;
    size_t input_offset = 0;
    size_t remaining;

    if (!carry || !carry_len || (!input && input_len > 0) ||
        (!output && output_capacity > 0) || alignment == 0 ||
        *carry_len >= alignment) {
        return 0;
    }

    old_carry_len = *carry_len;
    if (old_carry_len > 0) {
        if (old_carry_len + input_len < alignment) {
            memcpy(carry + old_carry_len, input, input_len);
            *carry_len = old_carry_len + input_len;
            return 0;
        }

        if (output_capacity < alignment) {
            return 0;
        }
        memcpy(output, carry, old_carry_len);
        memcpy(output + old_carry_len, input, alignment - old_carry_len);
        produced = alignment;
        input_offset = alignment - old_carry_len;
    }

    remaining = input_len - input_offset;
    if (remaining >= alignment) {
        size_t complete = remaining - (remaining % alignment);
        if (produced + complete > output_capacity) {
            return 0;
        }
        memcpy(output + produced, input + input_offset, complete);
        produced += complete;
        input_offset += complete;
    }

    remaining = input_len - input_offset;
    if (remaining > 0) {
        memcpy(carry, input + input_offset, remaining);
    }
    *carry_len = remaining;
    return produced;
}

size_t tts_websocket_pcm_to_ulaw(
    const unsigned char *input_pcm,
    size_t input_size,
    unsigned int input_rate,
    unsigned char *output,
    size_t output_capacity)
{
    size_t input_samples;
    size_t output_samples;
    size_t i;

    if (!input_pcm || input_size == 0 || !output || output_capacity == 0) {
        return 0;
    }

    if (input_rate == 8000) {
        /* 8 kHz passthrough: no sample-rate conversion. */
        if (input_size % 2 != 0) {
            return 0;
        }
        input_samples = input_size / 2;
        if (output_capacity < input_samples) {
            return 0;
        }
        for (i = 0; i < input_samples; i++) {
            output[i] = linear_to_ulaw(read_le16(input_pcm + i * 2));
        }
        return input_samples;
    }

    if (input_rate == 24000) {
        /* Legacy 24 kHz path: 3:1 moving-average downsampling, then
         * μ-law encoding. Input must be a multiple of 6 bytes
         * (3 complete 16-bit samples per output sample). */
        if (input_size % 6 != 0) {
            return 0;
        }
        input_samples = input_size / 2;
        output_samples = input_samples / 3;
        if (output_capacity < output_samples) {
            return 0;
        }
        for (i = 0; i < output_samples; i++) {
            int sum = 0;
            size_t j;
            for (j = 0; j < 3; j++) {
                sum += read_le16(input_pcm + (i * 3 + j) * 2);
            }
            output[i] = linear_to_ulaw((short)(sum / 3));
        }
        return output_samples;
    }

    /* Unsupported sample rate: refuse instead of guessing. */
    return 0;
}

void tts_websocket_pcm_fill_silence(unsigned char *buffer, size_t size)
{
    if (buffer && size > 0) {
        memset(buffer, 0xFF, size);
    }
}
