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

static int test_8khz_passthrough_to_ulaw(void)
{
    /* 8kHz 输入不做重采样：每个 16-bit LE 采样直接编码为 1 个 μ-law 字节 */
    const unsigned char input[] = {
        0x00, 0x00, /* 0      -> 0xFF */
        0xE8, 0x03, /* 1000   -> 0xCE */
        0x18, 0xFC, /* -1000  -> 0x4E */
        0xFF, 0x7F  /* 32767  -> 0x80 */
    };
    const unsigned char expected[] = { 0xFF, 0xCE, 0x4E, 0x80 };
    unsigned char output[8] = { 0 };
    size_t produced;

    produced = tts_websocket_pcm_to_ulaw(
        input, sizeof(input), 8000, output, sizeof(output));
    if (produced != sizeof(expected)) {
        return 0;
    }
    return memcmp(output, expected, sizeof(expected)) == 0;
}

static int test_sample_rate_alignment_and_8khz_tail(void)
{
    const unsigned char input[] = {
        0x00, 0x00, /* 0 -> 0xFF */
        0xE8, 0x03  /* 1000 -> 0xCE */
    };
    const unsigned char expected[] = { 0xFF, 0xCE };
    unsigned char carry[2] = { 0 };
    unsigned char aligned[4] = { 0 };
    unsigned char output[4] = { 0 };
    size_t carry_len = 0;
    size_t aligned_len;
    size_t produced;

    if (tts_websocket_pcm_alignment(8000) != 2 ||
        tts_websocket_pcm_alignment(24000) != 6 ||
        tts_websocket_pcm_alignment(16000) != 0) {
        return 0;
    }

    aligned_len = tts_websocket_pcm_accumulate(carry, &carry_len, input,
        sizeof(input), aligned, sizeof(aligned), tts_websocket_pcm_alignment(8000));
    if (aligned_len != sizeof(input) || carry_len != 0) {
        return 0;
    }
    produced = tts_websocket_pcm_to_ulaw(
        aligned, aligned_len, 8000, output, sizeof(output));
    return produced == sizeof(expected) &&
        memcmp(output, expected, sizeof(expected)) == 0;
}

static int test_24khz_resample_to_ulaw(void)
{
    /* 24kHz 输入做 3:1 移动平均降采样后再编码：
     * 3 个相同采样 1000 的平均值仍为 1000 -> 0xCE；零采样 -> 0xFF */
    const unsigned char input[] = {
        0xE8, 0x03, 0xE8, 0x03, 0xE8, 0x03,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    const unsigned char expected[] = { 0xCE, 0xFF };
    unsigned char output[8] = { 0 };
    size_t produced;

    produced = tts_websocket_pcm_to_ulaw(
        input, sizeof(input), 24000, output, sizeof(output));
    if (produced != sizeof(expected)) {
        return 0;
    }
    return memcmp(output, expected, sizeof(expected)) == 0;
}

static int test_reject_invalid_rate_and_alignment(void)
{
    const unsigned char pcm8[] = { 0x00, 0x00, 0x00, 0x00 };
    const unsigned char pcm24[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    unsigned char output[16] = { 0 };

    /* 不支持的采样率必须拒绝，不能盲目按 3:1 处理 */
    if (tts_websocket_pcm_to_ulaw(pcm8, sizeof(pcm8), 16000, output, sizeof(output)) != 0) {
        return 0;
    }
    /* 8kHz 输入必须是偶数字节（完整 16-bit 采样） */
    if (tts_websocket_pcm_to_ulaw(pcm8, 3, 8000, output, sizeof(output)) != 0) {
        return 0;
    }
    /* 24kHz 输入必须是 6 字节（3 个采样）的整数倍 */
    if (tts_websocket_pcm_to_ulaw(pcm24, 4, 24000, output, sizeof(output)) != 0) {
        return 0;
    }
    /* 输出容量不足 */
    if (tts_websocket_pcm_to_ulaw(pcm8, sizeof(pcm8), 8000, output, 1) != 0) {
        return 0;
    }
    /* NULL 输入 */
    if (tts_websocket_pcm_to_ulaw(NULL, 4, 8000, output, sizeof(output)) != 0) {
        return 0;
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
    if (!test_8khz_passthrough_to_ulaw()) {
        fprintf(stderr, "test_8khz_passthrough_to_ulaw failed\n");
        return 1;
    }
    if (!test_sample_rate_alignment_and_8khz_tail()) {
        fprintf(stderr, "test_sample_rate_alignment_and_8khz_tail failed\n");
        return 1;
    }
    if (!test_24khz_resample_to_ulaw()) {
        fprintf(stderr, "test_24khz_resample_to_ulaw failed\n");
        return 1;
    }
    if (!test_reject_invalid_rate_and_alignment()) {
        fprintf(stderr, "test_reject_invalid_rate_and_alignment failed\n");
        return 1;
    }
    puts("7/7 PCM streaming tests passed");
    return 0;
}
