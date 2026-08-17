#ifndef TTS_WEBSOCKET_PCM_H
#define TTS_WEBSOCKET_PCM_H

#include <stddef.h>

/**
 * Append a network PCM chunk to a carry buffer and return only complete
 * alignment-sized blocks. Bytes in the carry buffer are never discarded.
 *
 * @param carry          carry storage, at least alignment bytes
 * @param carry_len      in/out number of bytes currently in carry
 * @param input          input chunk
 * @param input_len      input length
 * @param output         destination for complete blocks
 * @param output_capacity destination capacity
 * @param alignment      required block alignment (> 0)
 * @return number of bytes written to output, or 0 on invalid arguments or
 *         insufficient output capacity
 */
size_t tts_websocket_pcm_accumulate(
    unsigned char *carry,
    size_t *carry_len,
    const unsigned char *input,
    size_t input_len,
    unsigned char *output,
    size_t output_capacity,
    size_t alignment);

/** Fill a complete PCMU frame with the G.711 mu-law silence value. */
void tts_websocket_pcm_fill_silence(
    unsigned char *buffer,
    size_t size);

/**
 * Convert 16-bit little-endian PCM at the given input sample rate into
 * 8 kHz μ-law (PCMU) audio.
 *
 * Supported input sample rates:
 *   - 8000: passthrough, every input sample becomes one μ-law byte
 *           (no sample-rate conversion is performed).
 *   - 24000: 3:1 moving-average downsampling before μ-law encoding
 *            (legacy TTS service behavior).
 *
 * @param input_pcm      input PCM data (16-bit signed little-endian samples)
 * @param input_size     input size in bytes
 * @param input_rate     input sample rate in Hz (8000 or 24000)
 * @param output         destination buffer for μ-law bytes
 * @param output_capacity capacity of the output buffer in bytes
 * @return number of μ-law bytes written to output, or 0 on invalid
 *         arguments, unsupported sample rate, misaligned input size
 *         (odd byte count at 8 kHz, or size not a multiple of 6 bytes
 *         at 24 kHz), or insufficient output capacity
 */
size_t tts_websocket_pcm_to_ulaw(
    const unsigned char *input_pcm,
    size_t input_size,
    unsigned int input_rate,
    unsigned char *output,
    size_t output_capacity);

#endif
