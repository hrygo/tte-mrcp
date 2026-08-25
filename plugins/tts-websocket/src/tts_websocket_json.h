#ifndef TTS_WEBSOCKET_JSON_H
#define TTS_WEBSOCKET_JSON_H

#include <stddef.h>

#define TTS_WEBSOCKET_JSON_UINT_INVALID (-1)
#define TTS_WEBSOCKET_JSON_UINT_MISSING 0
#define TTS_WEBSOCKET_JSON_UINT_FOUND 1

/* Extract a top-level unsigned integer JSON object member by exact key. */
int tts_websocket_json_get_uint(
    const char *json,
    size_t len,
    const char *key,
    unsigned int *value);

/* Return FOUND, MISSING, or INVALID without changing value on failure. */
int tts_websocket_json_get_uint_status(
    const char *json,
    size_t len,
    const char *key,
    unsigned int *value);

/* Apply the supported TTS sample-rate policy for one audio.start message. */
unsigned int tts_websocket_sample_rate_resolve(
    unsigned int previous_rate,
    int parse_status,
    unsigned int parsed_rate);

#endif
