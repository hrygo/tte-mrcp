#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "../src/tts_websocket_pcm.h"

static int test_odd_chunks_preserve_bytes(void)
{
    unsigned char carry[6] = {0};
    size_t carry_len = 0;
    const unsigned char first[] = {0xA0, 0xA1, 0xB0};
    const unsigned char second[] = {0xB1, 0xC0, 0xC1, 0xD0, 0xD1};
    unsigned char output[16] = {0};
    const unsigned char expected[] = {
        0xA0, 0xA1, 0xB0, 0xB1, 0xC0, 0xC1
    };
    size_t produced;

    produced = tts_websocket_pcm_accumulate(
        carry, &carry_len, first, sizeof(first), output, sizeof(output), 6);
    if (produced != 0 || carry_len != sizeof(first)) {
        return 0;
    }

    produced = tts_websocket_pcm_accumulate(
        carry, &carry_len, second, sizeof(second), output, sizeof(output), 6);
    if (produced != sizeof(expected) || carry_len != 2) {
        return 0;
    }
    return memcmp(output, expected, sizeof(expected)) == 0 &&
        carry[0] == 0xD0 && carry[1] == 0xD1;
}

static int test_all_one_byte_chunks_match_contiguous_input(void)
{
    unsigned char carry[6] = {0};
    size_t carry_len = 0;
    const unsigned char input[] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
        0x0D
    };
    unsigned char output[16] = {0};
    size_t produced = 0;
    size_t i;

    for (i = 0; i < sizeof(input); ++i) {
        produced += tts_websocket_pcm_accumulate(
            carry, &carry_len, input + i, 1,
            output + produced, sizeof(output) - produced, 6);
    }

    if (produced != 12 || carry_len != 1 || carry[0] != input[12]) {
        return 0;
    }
    return memcmp(output, input, produced) == 0;
}

static int test_silence_fill_covers_complete_frame(void)
{
    unsigned char frame[160];
    size_t i;

    memset(frame, 0x00, sizeof(frame));
    tts_websocket_pcm_fill_silence(frame, sizeof(frame));
    for (i = 0; i < sizeof(frame); ++i) {
        if (frame[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    if (!test_odd_chunks_preserve_bytes()) {
        fprintf(stderr, "test_odd_chunks_preserve_bytes failed\n");
        return 1;
    }
    if (!test_all_one_byte_chunks_match_contiguous_input()) {
        fprintf(stderr, "test_all_one_byte_chunks_match_contiguous_input failed\n");
        return 1;
    }
    if (!test_silence_fill_covers_complete_frame()) {
        fprintf(stderr, "test_silence_fill_covers_complete_frame failed\n");
        return 1;
    }
    puts("3/3 PCM streaming tests passed");
    return 0;
}
