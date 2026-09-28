#ifndef TTS_WEBSOCKET_DRAIN_H
#define TTS_WEBSOCKET_DRAIN_H

#include <stdint.h>

/* The server is allowed a short interval to deliver frames already queued
 * behind session.done before the plugin declares the stream complete. */
#define TTS_WEBSOCKET_DRAIN_GRACE_US ((uint64_t)500000)

/* Monotonic microsecond clock used for bounded drain deadlines. */
uint64_t tts_websocket_monotonic_now_us(void);

typedef struct tts_websocket_drain_t {
	int session_done_received;
	int peer_closed;
	uint64_t deadline_us;
} tts_websocket_drain_t;

void tts_websocket_drain_init(tts_websocket_drain_t *drain);
void tts_websocket_drain_session_done(
	tts_websocket_drain_t *drain, uint64_t now_us);
void tts_websocket_drain_peer_closed(tts_websocket_drain_t *drain);
int tts_websocket_drain_active(
	const tts_websocket_drain_t *drain, uint64_t now_us);
uint64_t tts_websocket_drain_remaining_us(
	const tts_websocket_drain_t *drain, uint64_t now_us);
int tts_websocket_drain_accept_binary(
	const tts_websocket_drain_t *drain,
	int receiving_audio,
	int audio_started,
	uint64_t now_us);

#endif
