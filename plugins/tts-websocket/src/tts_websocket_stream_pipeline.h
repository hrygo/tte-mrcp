#ifndef TTS_WEBSOCKET_STREAM_PIPELINE_H
#define TTS_WEBSOCKET_STREAM_PIPELINE_H

#include <apr_errno.h>
#include <apr_pools.h>
#include <apr_thread_proc.h>

#include <stddef.h>

typedef struct tts_websocket_stream_pipeline_t
	tts_websocket_stream_pipeline_t;

typedef int (*tts_websocket_stream_pipeline_output_f)(
	void *context, const short *samples, size_t sample_count);

enum {
	TTS_WEBSOCKET_STREAM_PIPELINE_OK = 0,
	TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED = 1,
	TTS_WEBSOCKET_STREAM_PIPELINE_ERROR = 2
};

tts_websocket_stream_pipeline_t *tts_websocket_stream_pipeline_create(
	apr_pool_t *pool,
	unsigned int input_rate,
	unsigned int output_rate,
	unsigned int channels,
	int quality,
	int *error_code);

void tts_websocket_stream_pipeline_destroy(
	tts_websocket_stream_pipeline_t *pipeline);

int tts_websocket_stream_pipeline_feed(
	tts_websocket_stream_pipeline_t *pipeline,
	const unsigned char *input,
	size_t input_size,
	apr_pool_t *scratch_pool,
	tts_websocket_stream_pipeline_output_f output,
	void *context);

void tts_websocket_stream_pipeline_request_stop(
	tts_websocket_stream_pipeline_t *pipeline);

int tts_websocket_stream_pipeline_finish(
	tts_websocket_stream_pipeline_t *pipeline,
	apr_pool_t *scratch_pool,
	tts_websocket_stream_pipeline_output_f output,
	void *context,
	size_t *orphan_bytes);

int tts_websocket_stream_pipeline_stop_requested(
	const tts_websocket_stream_pipeline_t *pipeline);

int tts_websocket_stream_pipeline_eof_ready(
	const tts_websocket_stream_pipeline_t *pipeline);

size_t tts_websocket_stream_pipeline_carry_bytes(
	const tts_websocket_stream_pipeline_t *pipeline);

size_t tts_websocket_stream_pipeline_input_samples(
	const tts_websocket_stream_pipeline_t *pipeline);

size_t tts_websocket_stream_pipeline_output_samples(
	const tts_websocket_stream_pipeline_t *pipeline);

size_t tts_websocket_stream_pipeline_process_calls(
	const tts_websocket_stream_pipeline_t *pipeline);

size_t tts_websocket_stream_pipeline_finish_samples(
	const tts_websocket_stream_pipeline_t *pipeline);

size_t tts_websocket_stream_pipeline_orphan_bytes(
	const tts_websocket_stream_pipeline_t *pipeline);

int tts_websocket_stream_pipeline_error(
	const tts_websocket_stream_pipeline_t *pipeline);

/* Clear the caller's worker handle only after apr_thread_join succeeds. */
int tts_websocket_stream_pipeline_join_worker(
	apr_thread_t **worker,
	apr_status_t *join_status);

#endif
