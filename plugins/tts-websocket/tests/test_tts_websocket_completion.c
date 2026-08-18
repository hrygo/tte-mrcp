#include "tts_websocket_completion.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_completion_is_exactly_once(void)
{
	tts_websocket_completion_t state;
	uint64_t generation;

	tts_websocket_completion_init(&state);
	generation = tts_websocket_completion_begin(&state);
	assert(tts_websocket_completion_claim(
		&state, generation, TTS_WEBSOCKET_COMPLETION_OWNER_MPF));
	assert(!tts_websocket_completion_claim(
		&state, generation, TTS_WEBSOCKET_COMPLETION_OWNER_WATCHDOG));
	assert(state.owner == TTS_WEBSOCKET_COMPLETION_OWNER_MPF);
}

static void test_stale_generation_cannot_complete_new_speak(void)
{
	tts_websocket_completion_t state;
	uint64_t old_generation;
	uint64_t new_generation;

	tts_websocket_completion_init(&state);
	old_generation = tts_websocket_completion_begin(&state);
	new_generation = tts_websocket_completion_begin(&state);
	assert(new_generation != old_generation);
	assert(!tts_websocket_completion_claim(
		&state, old_generation, TTS_WEBSOCKET_COMPLETION_OWNER_WATCHDOG));
	assert(tts_websocket_completion_claim(
		&state, new_generation, TTS_WEBSOCKET_COMPLETION_OWNER_MPF));
}

static void test_cancel_disarms_watchdog(void)
{
	tts_websocket_completion_t state;
	uint64_t generation;

	tts_websocket_completion_init(&state);
	generation = tts_websocket_completion_begin(&state);
	assert(tts_websocket_completion_cancel(&state, generation));
	assert(!tts_websocket_completion_claim(
		&state, generation, TTS_WEBSOCKET_COMPLETION_OWNER_WATCHDOG));
}

static void test_timeout_covers_remaining_audio_and_guard_time(void)
{
	assert(tts_websocket_completion_timeout_ms(8000, 200, 1500, 500) == 2700);
	assert(tts_websocket_completion_timeout_ms(0, 0, 200, 500) == 500);
	assert(tts_websocket_completion_timeout_ms(1, 0, 0, 0) == 1);
}

int main(void)
{
	test_completion_is_exactly_once();
	test_stale_generation_cannot_complete_new_speak();
	test_cancel_disarms_watchdog();
	test_timeout_covers_remaining_audio_and_guard_time();
	puts("tts_websocket_completion tests passed");
	return 0;
}
