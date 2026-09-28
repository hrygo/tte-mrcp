#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "tts_websocket_drain.h"
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

uint64_t tts_websocket_monotonic_now_us(void)
{
#if defined(_WIN32)
	LARGE_INTEGER counter;
	LARGE_INTEGER frequency;

	if(!QueryPerformanceFrequency(&frequency) ||
	   !QueryPerformanceCounter(&counter) ||
	   frequency.QuadPart <= 0) {
		return 0;
	}
	return (uint64_t)(
		(counter.QuadPart / frequency.QuadPart) * 1000000ULL +
		((counter.QuadPart % frequency.QuadPart) * 1000000ULL) /
			frequency.QuadPart);
#elif defined(__APPLE__)
	mach_timebase_info_data_t timebase;
	uint64_t ticks;
	uint64_t whole;
	uint64_t remainder;
	uint64_t nanoseconds;

	if(mach_timebase_info(&timebase) != KERN_SUCCESS ||
	   timebase.denom == 0) {
		return 0;
	}
	ticks = mach_absolute_time();
	whole = ticks / timebase.denom;
	remainder = ticks % timebase.denom;
	nanoseconds = whole * timebase.numer +
		(remainder * timebase.numer) / timebase.denom;
	return nanoseconds / 1000U;
#else
	struct timespec value;

	if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
		return 0;
	}
	return (uint64_t)value.tv_sec * 1000000ULL +
		(uint64_t)(value.tv_nsec / 1000L);
#endif
}

void tts_websocket_drain_init(tts_websocket_drain_t *drain)
{
	if(drain) {
		memset(drain, 0, sizeof(*drain));
	}
}

void tts_websocket_drain_session_done(
	tts_websocket_drain_t *drain, uint64_t now_us)
{
	if(!drain || drain->session_done_received) {
		return;
	}
	drain->session_done_received = 1;
	if(now_us > (uint64_t)-1 - TTS_WEBSOCKET_DRAIN_GRACE_US) {
		drain->deadline_us = (uint64_t)-1;
	} else {
		drain->deadline_us = now_us + TTS_WEBSOCKET_DRAIN_GRACE_US;
	}
}

void tts_websocket_drain_peer_closed(tts_websocket_drain_t *drain)
{
	if(drain) {
		drain->peer_closed = 1;
	}
}

uint64_t tts_websocket_drain_remaining_us(
	const tts_websocket_drain_t *drain, uint64_t now_us)
{
	if(!drain || !drain->session_done_received || drain->peer_closed ||
		now_us >= drain->deadline_us) {
		return 0;
	}
	return drain->deadline_us - now_us;
}

int tts_websocket_drain_active(
	const tts_websocket_drain_t *drain, uint64_t now_us)
{
	return tts_websocket_drain_remaining_us(drain, now_us) > 0;
}

int tts_websocket_drain_accept_binary(
	const tts_websocket_drain_t *drain,
	int receiving_audio,
	int audio_started,
	uint64_t now_us)
{
	if(!drain || drain->peer_closed || !audio_started) {
		return 0;
	}
	if(drain->session_done_received &&
		!tts_websocket_drain_active(drain, now_us)) {
		return 0;
	}
	if(receiving_audio) {
		return 1;
	}
	/* Before session.done, audio.done only closes the current sentence. */
	if(!drain->session_done_received) {
		return 1;
	}
	/* After session.done, accept only frames inside the bounded drain window. */
	return tts_websocket_drain_active(drain, now_us);
}
