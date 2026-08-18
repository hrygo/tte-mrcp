#ifndef TTS_WEBSOCKET_COMPLETION_H
#define TTS_WEBSOCKET_COMPLETION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	TTS_WEBSOCKET_COMPLETION_OWNER_NONE = 0,
	TTS_WEBSOCKET_COMPLETION_OWNER_MPF,
	TTS_WEBSOCKET_COMPLETION_OWNER_WATCHDOG,
	TTS_WEBSOCKET_COMPLETION_OWNER_CANCELLED
} tts_websocket_completion_owner_e;

typedef struct {
	uint64_t generation;
	int active;
	tts_websocket_completion_owner_e owner;
} tts_websocket_completion_t;

void tts_websocket_completion_init(tts_websocket_completion_t *state);
uint64_t tts_websocket_completion_begin(tts_websocket_completion_t *state);
int tts_websocket_completion_claim(
	tts_websocket_completion_t *state,
	uint64_t generation,
	tts_websocket_completion_owner_e owner);
int tts_websocket_completion_cancel(
	tts_websocket_completion_t *state,
	uint64_t generation);
uint64_t tts_websocket_completion_timeout_ms(
	uint64_t remaining_pcmu_bytes,
	uint64_t postroll_ms,
	uint64_t grace_ms,
	uint64_t minimum_ms);

#ifdef __cplusplus
}
#endif

#endif
