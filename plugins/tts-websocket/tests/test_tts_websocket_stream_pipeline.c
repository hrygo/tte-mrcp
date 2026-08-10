#include "../src/tts_websocket_stream_pipeline.h"

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_thread_proc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_RATE 24000U
#define OUTPUT_RATE 8000U
#define INPUT_SAMPLES 1200U
#define QUALITY 10

typedef struct pipeline_sink_t {
	tts_websocket_stream_pipeline_t *pipeline;
	size_t output_samples;
	size_t output_bytes;
	size_t finish_samples;
	int output_callbacks;
	int eof_seen_during_output;
	int in_finish;
} pipeline_sink_t;

static int collect_output(void *data, const short *samples, size_t sample_count)
{
	pipeline_sink_t *sink = (pipeline_sink_t*)data;

	if(!sink || (!samples && sample_count > 0)) {
		return 0;
	}
	sink->output_samples += sample_count;
	sink->output_bytes += sample_count;
	sink->output_callbacks++;
	if(sink->in_finish) {
		sink->finish_samples += sample_count;
	}
	if(tts_websocket_stream_pipeline_eof_ready(sink->pipeline)) {
		sink->eof_seen_during_output = 1;
	}
	return 1;
}

static void fill_pcm(short *samples)
{
	size_t i;

	for(i = 0; i < INPUT_SAMPLES; ++i) {
		samples[i] = (short)((int)((i * 37U) % 20000U) - 10000);
	}
	samples[INPUT_SAMPLES - 1] = 28000;
}

static void pcm_to_le(const short *samples, unsigned char *bytes)
{
	size_t i;

	for(i = 0; i < INPUT_SAMPLES; ++i) {
		unsigned short value = (unsigned short)samples[i];
		bytes[i * 2] = (unsigned char)(value & 0xFFU);
		bytes[i * 2 + 1] = (unsigned char)(value >> 8);
	}
}

static int feed_chunk(tts_websocket_stream_pipeline_t *pipeline,
	apr_pool_t *pool, pipeline_sink_t *sink,
	const unsigned char *bytes, size_t offset, size_t length)
{
	return tts_websocket_stream_pipeline_feed(
		pipeline, bytes + offset, length, pool, collect_output, sink) ==
		TTS_WEBSOCKET_STREAM_PIPELINE_OK;
}

static int open_pipeline(apr_pool_t *pool,
	tts_websocket_stream_pipeline_t **pipeline)
{
	int error_code = -1;

	*pipeline = tts_websocket_stream_pipeline_create(
		pool, INPUT_RATE, OUTPUT_RATE, 1U, QUALITY, &error_code);
	return *pipeline != NULL && error_code == 0;
}

static int test_carry_survives_audio_boundaries_and_eof_order(
	apr_pool_t *pool)
{
	short input[INPUT_SAMPLES];
	unsigned char bytes[INPUT_SAMPLES * 2 + 1];
	tts_websocket_stream_pipeline_t *pipeline = NULL;
	pipeline_sink_t sink;
	size_t offset = 0;
	size_t orphan_bytes = 0;
	size_t chunks[] = {5, 1, 17, 61, 3, 127, 401, 785};
	size_t chunk_index = 0;
	int finish_result;
	int ok = 0;

	fill_pcm(input);
	pcm_to_le(input, bytes);
	bytes[INPUT_SAMPLES * 2] = 0x7FU;
	memset(&sink, 0, sizeof(sink));
	if(!open_pipeline(pool, &pipeline)) {
		return 0;
	}
	sink.pipeline = pipeline;

	/* The first call ends at an odd byte. The next call represents data after
	 * an audio.done/audio.start boundary; the carry must remain in production
	 * pipeline state rather than in the worker's message handling. */
	while(offset < sizeof(bytes)) {
		size_t chunk = chunks[chunk_index %
			(sizeof(chunks) / sizeof(chunks[0]))];
		if(chunk > sizeof(bytes) - offset) {
			chunk = sizeof(bytes) - offset;
		}
		if(!feed_chunk(pipeline, pool, &sink, bytes, offset, chunk)) {
			goto cleanup;
		}
		if(offset == 0 && tts_websocket_stream_pipeline_carry_bytes(pipeline) != 1) {
			goto cleanup;
		}
		offset += chunk;
		chunk_index++;
	}
	if(tts_websocket_stream_pipeline_carry_bytes(pipeline) != 1) {
		goto cleanup;
	}

	sink.in_finish = 1;
	finish_result = tts_websocket_stream_pipeline_finish(
		pipeline, pool, collect_output, &sink, &orphan_bytes);
	if(finish_result != TTS_WEBSOCKET_STREAM_PIPELINE_OK ||
		orphan_bytes != 1 ||
		tts_websocket_stream_pipeline_orphan_bytes(pipeline) != 1 ||
		tts_websocket_stream_pipeline_carry_bytes(pipeline) != 0 ||
		sink.finish_samples == 0 ||
		sink.output_samples != INPUT_SAMPLES / 3 ||
		sink.output_bytes != INPUT_SAMPLES / 3 ||
	sink.eof_seen_during_output ||
		!tts_websocket_stream_pipeline_eof_ready(pipeline) ||
		tts_websocket_stream_pipeline_finish_samples(pipeline) !=
		sink.finish_samples) {
		goto cleanup;
	}
	ok = 1;

cleanup:
	tts_websocket_stream_pipeline_destroy(pipeline);
	return ok;
}

static int test_stop_wins_without_resampler_tail(apr_pool_t *pool)
{
	short input[INPUT_SAMPLES];
	unsigned char bytes[INPUT_SAMPLES * 2];
	tts_websocket_stream_pipeline_t *pipeline = NULL;
	pipeline_sink_t sink;
	size_t orphan_bytes = 0;
	int finish_result;
	int ok = 0;

	fill_pcm(input);
	pcm_to_le(input, bytes);
	memset(&sink, 0, sizeof(sink));
	if(!open_pipeline(pool, &pipeline)) {
		return 0;
	}
	sink.pipeline = pipeline;
	if(!feed_chunk(pipeline, pool, &sink, bytes, 0, sizeof(bytes))) {
		goto cleanup;
	}
	tts_websocket_stream_pipeline_request_stop(pipeline);
	sink.in_finish = 1;
	finish_result = tts_websocket_stream_pipeline_finish(
		pipeline, pool, collect_output, &sink, &orphan_bytes);
	if(finish_result != TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED ||
		orphan_bytes != 0 || sink.finish_samples != 0 ||
		tts_websocket_stream_pipeline_finish_samples(pipeline) != 0 ||
		tts_websocket_stream_pipeline_eof_ready(pipeline)) {
		goto cleanup;
	}
	ok = 1;

cleanup:
	tts_websocket_stream_pipeline_destroy(pipeline);
	return ok;
}

static void* APR_THREAD_FUNC detached_worker(apr_thread_t *thread, void *data)
{
	(void)thread;
	(void)data;
	return NULL;
}

static int test_failed_join_retains_worker_handle(apr_pool_t *pool)
{
	apr_threadattr_t *attributes = NULL;
	apr_thread_t *thread = NULL;
	apr_status_t join_status = APR_SUCCESS;
	apr_status_t status;

	if(apr_threadattr_create(&attributes, pool) != APR_SUCCESS ||
		apr_threadattr_detach_set(attributes, 1) != APR_SUCCESS ||
		apr_thread_create(&thread, attributes, detached_worker, NULL, pool) !=
		APR_SUCCESS) {
		return 0;
	}
	apr_sleep(20000);
	if(tts_websocket_stream_pipeline_join_worker(&thread, &join_status) ||
		join_status == APR_SUCCESS || thread == NULL) {
		return 0;
	}
	status = apr_thread_detach(thread);
	return status != APR_SUCCESS || thread != NULL;
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
	if(test_carry_survives_audio_boundaries_and_eof_order(pool)) {
		passed++;
	} else {
		fprintf(stderr, "carry/EOF ordering test failed\n");
	}
	if(test_stop_wins_without_resampler_tail(pool)) {
		passed++;
	} else {
		fprintf(stderr, "STOP tail suppression test failed\n");
	}
	if(test_failed_join_retains_worker_handle(pool)) {
		passed++;
	} else {
		fprintf(stderr, "failed join ownership test failed\n");
	}

	apr_pool_destroy(pool);
	apr_terminate();
	printf("%d/3 stream pipeline tests passed\n", passed);
	return passed == 3 ? 0 : 1;
}
