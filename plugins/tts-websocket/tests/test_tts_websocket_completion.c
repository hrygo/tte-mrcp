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

static void test_watchdog_deadline_freezes_while_paused(void)
{
	tts_websocket_watchdog_t watchdog;

	tts_websocket_watchdog_init(&watchdog, 1000000, 1000);
	assert(!tts_websocket_watchdog_expired(&watchdog, 1500000, 1));
	assert(!tts_websocket_watchdog_expired(&watchdog, 3000000, 1));
	assert(!tts_websocket_watchdog_expired(&watchdog, 3000000, 0));
	assert(!tts_websocket_watchdog_expired(&watchdog, 3499999, 0));
	assert(tts_websocket_watchdog_expired(&watchdog, 3500000, 0));
}

static void test_watchdog_deadline_saturates(void)
{
	tts_websocket_watchdog_t watchdog;

	tts_websocket_watchdog_init(&watchdog, UINT64_MAX - 10, UINT64_MAX);
	assert(watchdog.deadline_us == UINT64_MAX);
	assert(!tts_websocket_watchdog_expired(&watchdog, UINT64_MAX - 1, 0));
	assert(tts_websocket_watchdog_expired(&watchdog, UINT64_MAX, 0));
}

static void test_watchdog_engine_decisions(void)
{
	tts_websocket_completion_t completion;
	tts_websocket_watchdog_t watchdog;
	uint64_t generation;

	tts_websocket_completion_init(&completion);
	generation = tts_websocket_completion_begin(&completion);
	tts_websocket_watchdog_init(&watchdog, 1000000, 1000);
	assert(!tts_websocket_watchdog_should_claim(
		&watchdog, &completion, generation, 1500000, 1, 0));
	assert(!tts_websocket_watchdog_should_claim(
		&watchdog, &completion, generation, 2000000, 0, 0));
	assert(tts_websocket_watchdog_should_claim(
		&watchdog, &completion, generation, 2500000, 0, 0));

	tts_websocket_watchdog_init(&watchdog, 1000000, 1000);
	assert(!tts_websocket_watchdog_should_claim(
		&watchdog, &completion, generation, 3000000, 0, 1));
	assert(tts_websocket_completion_cancel(&completion, generation));
	assert(!tts_websocket_watchdog_should_claim(
		&watchdog, &completion, generation, 3000000, 0, 0));

	generation = tts_websocket_completion_begin(&completion);
	assert(tts_websocket_completion_claim(
		&completion, generation, TTS_WEBSOCKET_COMPLETION_OWNER_MPF));
	assert(!tts_websocket_watchdog_should_claim(
		&watchdog, &completion, generation, 3000000, 0, 0));
}

int main(void)
{
	test_completion_is_exactly_once();
	test_stale_generation_cannot_complete_new_speak();
	test_cancel_disarms_watchdog();
	test_timeout_covers_remaining_audio_and_guard_time();
	test_watchdog_deadline_freezes_while_paused();
	test_watchdog_deadline_saturates();
	test_watchdog_engine_decisions();
	puts("tts_websocket_completion tests passed");
	return 0;
}
