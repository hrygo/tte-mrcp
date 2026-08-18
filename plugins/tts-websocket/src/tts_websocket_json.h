#ifndef TTS_WEBSOCKET_JSON_H
#define TTS_WEBSOCKET_JSON_H

#include <stddef.h>

/* Extract a top-level unsigned integer JSON object member by exact key. */
int tts_websocket_json_get_uint(
    const char *json,
    size_t len,
    const char *key,
    unsigned int *value);

#endif
