#ifndef TTS_WEBSOCKET_LIFECYCLE_H
#define TTS_WEBSOCKET_LIFECYCLE_H

#include <apr_atomic.h>
#include <apt.h>

typedef struct tts_websocket_stream_lifecycle_t {
	volatile apr_uint32_t closing;
	volatile apr_uint32_t active_callbacks;
} tts_websocket_stream_lifecycle_t;

void tts_websocket_stream_lifecycle_init(
	tts_websocket_stream_lifecycle_t *lifecycle);
void tts_websocket_stream_lifecycle_reopen(
	tts_websocket_stream_lifecycle_t *lifecycle);
apt_bool_t tts_websocket_stream_lifecycle_enter(
	tts_websocket_stream_lifecycle_t *lifecycle);
void tts_websocket_stream_lifecycle_leave(
	tts_websocket_stream_lifecycle_t *lifecycle);
void tts_websocket_stream_lifecycle_begin_close(
	tts_websocket_stream_lifecycle_t *lifecycle);
void tts_websocket_stream_lifecycle_wait(
	tts_websocket_stream_lifecycle_t *lifecycle);
apr_uint32_t tts_websocket_stream_lifecycle_active(
	tts_websocket_stream_lifecycle_t *lifecycle);

#endif
