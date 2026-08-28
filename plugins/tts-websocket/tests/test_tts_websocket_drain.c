#include "tts_websocket_drain.h"

#include <stdio.h>

static int test_session_done_starts_grace_window(void)
{
	/* The policy API is intentionally exercised with a deterministic clock. */
	tts_websocket_drain_t drain;
	const uint64_t done_at = 1000000;

	tts_websocket_drain_init(&drain);
	tts_websocket_drain_session_done(&drain, done_at);
	return tts_websocket_drain_active(&drain, done_at + 499999) &&
		!tts_websocket_drain_active(&drain, done_at + 500000) &&
		tts_websocket_drain_remaining_us(&drain, done_at + 100000) == 400000;
}

static int test_close_ends_drain_immediately(void)
{
	tts_websocket_drain_t drain;

	tts_websocket_drain_init(&drain);
	tts_websocket_drain_session_done(&drain, 2000000);
	tts_websocket_drain_peer_closed(&drain);
	return !tts_websocket_drain_active(&drain, 2000001) &&
		tts_websocket_drain_remaining_us(&drain, 2000001) == 0;
}

static int test_late_binary_is_retained_until_end(void)
{
	tts_websocket_drain_t drain;
	const uint64_t done_at = 3000000;
	const uint64_t close_at = done_at + 1000;
	uint64_t accepted_bytes = 0;
	int i;

	tts_websocket_drain_init(&drain);
	/* Reproduce the capture: audio.start -> 3x16KB -> audio.done ->
	 * session.done -> 13.44KB late binary -> peer Close. */
	for(i = 0; i < 3; ++i) {
		if(!tts_websocket_drain_accept_binary(&drain, 1, 1, done_at - 2)) {
			return 0;
		}
		accepted_bytes += 16000;
	}
	/* A binary frame is still eligible after audio.done once audio.start was seen. */
	if(!tts_websocket_drain_accept_binary(&drain, 0, 1, done_at - 1)) {
		return 0;
	}
	tts_websocket_drain_session_done(&drain, done_at);
	if(!tts_websocket_drain_accept_binary(&drain, 0, 1, done_at + 1) ||
		tts_websocket_drain_accept_binary(&drain, 0, 0, done_at + 1)) {
		return 0;
	}
	accepted_bytes += 13440;
	if(accepted_bytes != 61440) {
		return 0;
	}
	if(tts_websocket_drain_active(&drain, done_at + 500000)) {
		return 0;
	}
	/* Deadline expiry alone rejects a late frame; this must not depend on
	 * peer_closed being set first. */
	if(tts_websocket_drain_accept_binary(&drain, 0, 1,
			done_at + 500000)) {
		return 0;
	}
	/* A peer Close ends the drain immediately, including an early Close. */
	tts_websocket_drain_peer_closed(&drain);
	return !tts_websocket_drain_accept_binary(&drain, 0, 1, close_at);
}

int main(void)
{
	int passed = 0;

	if(test_session_done_starts_grace_window()) {
		passed++;
	} else {
		fprintf(stderr, "session.done grace-window test failed\n");
	}
	if(test_close_ends_drain_immediately()) {
		passed++;
	} else {
		fprintf(stderr, "peer Close drain test failed\n");
	}
	if(test_late_binary_is_retained_until_end()) {
		passed++;
	} else {
		fprintf(stderr, "late binary acceptance test failed\n");
	}

	printf("%d/3 WebSocket drain tests passed\n", passed);
	return passed == 3 ? 0 : 1;
}
