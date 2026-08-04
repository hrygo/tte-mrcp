#include "tts_websocket_lifecycle.h"

#include <apr_general.h>
#include <apr_thread_proc.h>
#include <stdio.h>

static int test_close_rejects_new_callbacks(void)
{
	tts_websocket_stream_lifecycle_t lifecycle;

	tts_websocket_stream_lifecycle_init(&lifecycle);
	if(!tts_websocket_stream_lifecycle_enter(&lifecycle)) {
		return 0;
	}
	tts_websocket_stream_lifecycle_begin_close(&lifecycle);
	if(tts_websocket_stream_lifecycle_enter(&lifecycle)) {
		tts_websocket_stream_lifecycle_leave(&lifecycle);
		return 0;
	}
	tts_websocket_stream_lifecycle_leave(&lifecycle);
	tts_websocket_stream_lifecycle_wait(&lifecycle);
	return tts_websocket_stream_lifecycle_active(&lifecycle) == 0;
}

static void* APR_THREAD_FUNC wait_for_close(apr_thread_t *thread, void *data)
{
	tts_websocket_stream_lifecycle_t *lifecycle =
		(tts_websocket_stream_lifecycle_t*)data;
	(void)thread;
	tts_websocket_stream_lifecycle_wait(lifecycle);
	return NULL;
}

static int test_close_waits_for_active_callback(apr_pool_t *pool)
{
	tts_websocket_stream_lifecycle_t lifecycle;
	apr_thread_t *thread = NULL;
	apr_status_t thread_result;

	tts_websocket_stream_lifecycle_init(&lifecycle);
	if(!tts_websocket_stream_lifecycle_enter(&lifecycle)) {
		return 0;
	}
	tts_websocket_stream_lifecycle_begin_close(&lifecycle);
	if(apr_thread_create(&thread, NULL, wait_for_close, &lifecycle, pool) != APR_SUCCESS) {
		tts_websocket_stream_lifecycle_leave(&lifecycle);
		return 0;
	}
	apr_sleep(20000);
	if(tts_websocket_stream_lifecycle_active(&lifecycle) != 1) {
		tts_websocket_stream_lifecycle_leave(&lifecycle);
		(void)apr_thread_join(&thread_result, thread);
		return 0;
	}
	tts_websocket_stream_lifecycle_leave(&lifecycle);
	if(apr_thread_join(&thread_result, thread) != APR_SUCCESS) {
		return 0;
	}
	return tts_websocket_stream_lifecycle_active(&lifecycle) == 0;
}

int main(void)
{
	apr_pool_t *pool = NULL;
	int passed = 0;

	if(apr_initialize() != APR_SUCCESS ||
	   apr_pool_create(&pool, NULL) != APR_SUCCESS) {
		fprintf(stderr, "failed to initialize APR\n");
		return 1;
	}

	if(test_close_rejects_new_callbacks()) {
		passed++;
	} else {
		fprintf(stderr, "close must reject new callbacks\n");
	}
	if(test_close_waits_for_active_callback(pool)) {
		passed++;
	} else {
		fprintf(stderr, "close must wait for active callbacks\n");
	}

	apr_pool_destroy(pool);
	apr_terminate();
	printf("%d/2 lifecycle tests passed\n", passed);
	return passed == 2 ? 0 : 1;
}
