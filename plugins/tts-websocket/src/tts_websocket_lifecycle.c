#include "tts_websocket_lifecycle.h"

#include <apr_time.h>

void tts_websocket_stream_lifecycle_init(
	tts_websocket_stream_lifecycle_t *lifecycle)
{
	if(!lifecycle) {
		return;
	}
	apr_atomic_set32(&lifecycle->closing, 0);
	apr_atomic_set32(&lifecycle->active_callbacks, 0);
}

void tts_websocket_stream_lifecycle_reopen(
	tts_websocket_stream_lifecycle_t *lifecycle)
{
	if(lifecycle) {
		apr_atomic_set32(&lifecycle->closing, 0);
	}
}

apt_bool_t tts_websocket_stream_lifecycle_enter(
	tts_websocket_stream_lifecycle_t *lifecycle)
{
	if(!lifecycle || apr_atomic_read32(&lifecycle->closing)) {
		return FALSE;
	}
	apr_atomic_inc32(&lifecycle->active_callbacks);
	if(apr_atomic_read32(&lifecycle->closing)) {
		apr_atomic_dec32(&lifecycle->active_callbacks);
		return FALSE;
	}
	return TRUE;
}

void tts_websocket_stream_lifecycle_leave(
	tts_websocket_stream_lifecycle_t *lifecycle)
{
	if(lifecycle) {
		apr_atomic_dec32(&lifecycle->active_callbacks);
	}
}

void tts_websocket_stream_lifecycle_begin_close(
	tts_websocket_stream_lifecycle_t *lifecycle)
{
	if(lifecycle) {
		apr_atomic_set32(&lifecycle->closing, 1);
	}
}

void tts_websocket_stream_lifecycle_wait(
	tts_websocket_stream_lifecycle_t *lifecycle)
{
	if(!lifecycle) {
		return;
	}
	while(apr_atomic_read32(&lifecycle->active_callbacks) != 0) {
		apr_sleep(1000);
	}
}

apr_uint32_t tts_websocket_stream_lifecycle_active(
	tts_websocket_stream_lifecycle_t *lifecycle)
{
	return lifecycle ? apr_atomic_read32(&lifecycle->active_callbacks) : 0;
}
