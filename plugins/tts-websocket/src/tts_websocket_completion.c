#include "tts_websocket_completion.h"

#include <limits.h>
#include <stddef.h>

static uint64_t tts_websocket_completion_add_saturated(
	uint64_t left,
	uint64_t right)
{
	return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

void tts_websocket_completion_init(tts_websocket_completion_t *state)
{
	if(!state) {
		return;
	}
	state->generation = 0;
	state->active = 0;
	state->owner = TTS_WEBSOCKET_COMPLETION_OWNER_NONE;
}

uint64_t tts_websocket_completion_begin(tts_websocket_completion_t *state)
{
	if(!state) {
		return 0;
	}
	state->generation++;
	if(state->generation == 0) {
		state->generation = 1;
	}
	state->active = 1;
	state->owner = TTS_WEBSOCKET_COMPLETION_OWNER_NONE;
	return state->generation;
}

int tts_websocket_completion_claim(
	tts_websocket_completion_t *state,
	uint64_t generation,
	tts_websocket_completion_owner_e owner)
{
	if(!state || !state->active || generation != state->generation ||
	   owner == TTS_WEBSOCKET_COMPLETION_OWNER_NONE ||
	   owner == TTS_WEBSOCKET_COMPLETION_OWNER_CANCELLED) {
		return 0;
	}
	state->active = 0;
	state->owner = owner;
	return 1;
}

int tts_websocket_completion_cancel(
	tts_websocket_completion_t *state,
	uint64_t generation)
{
	if(!state || !state->active || generation != state->generation) {
		return 0;
	}
	state->active = 0;
	state->owner = TTS_WEBSOCKET_COMPLETION_OWNER_CANCELLED;
	return 1;
}

uint64_t tts_websocket_completion_timeout_ms(
	uint64_t remaining_pcmu_bytes,
	uint64_t postroll_ms,
	uint64_t grace_ms,
	uint64_t minimum_ms)
{
	uint64_t audio_ms;
	uint64_t timeout_ms;

	if(remaining_pcmu_bytes > (UINT64_MAX - 7999) / 1000) {
		audio_ms = UINT64_MAX;
	} else {
		audio_ms = (remaining_pcmu_bytes * 1000 + 7999) / 8000;
	}
	timeout_ms = tts_websocket_completion_add_saturated(audio_ms, postroll_ms);
	timeout_ms = tts_websocket_completion_add_saturated(timeout_ms, grace_ms);
	return timeout_ms < minimum_ms ? minimum_ms : timeout_ms;
}
