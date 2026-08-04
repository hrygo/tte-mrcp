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

#endif
