#include "tts_websocket_pcm.h"

#include <string.h>

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
