#include "tts_websocket_stream_pipeline.h"

#include "tts_websocket_pcm.h"
#include "tts_websocket_resampler.h"

#include <apr_atomic.h>

#include <stdint.h>
#include <string.h>

struct tts_websocket_stream_pipeline_t {
	apr_thread_mutex_t *terminal_mutex;
	tts_websocket_resampler_t *resampler;
	unsigned char carry[2];
	size_t carry_len;
	volatile apr_uint32_t stop_requested;
	volatile apr_uint32_t eof_ready;
	size_t input_samples;
	size_t output_samples;
	size_t process_calls;
	size_t finish_samples;
	size_t orphan_bytes;
	int error_code;
	int finished;
};

static int pipeline_is_stopped(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline && apr_atomic_read32(
		(volatile apr_uint32_t *)(uintptr_t)&pipeline->stop_requested) != 0;
}

static int pipeline_fail(
	tts_websocket_stream_pipeline_t *pipeline, int error_code)
{
	if(pipeline) {
		pipeline->error_code = error_code != 0 ? error_code : -1;
	}
	return TTS_WEBSOCKET_STREAM_PIPELINE_ERROR;
}

static int pipeline_emit(
	tts_websocket_stream_pipeline_t *pipeline,
	const short *samples,
	size_t sample_count,
	tts_websocket_stream_pipeline_output_f output,
	void *context)
{
	if(sample_count == 0) {
		return TTS_WEBSOCKET_STREAM_PIPELINE_OK;
	}
	if(pipeline_is_stopped(pipeline)) {
		return TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED;
	}
	if(!output || !output(context, samples, sample_count)) {
		if(pipeline_is_stopped(pipeline)) {
			return TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED;
		}
		return pipeline_fail(pipeline, -1);
	}
	return TTS_WEBSOCKET_STREAM_PIPELINE_OK;
}

static int pipeline_process_aligned(
	tts_websocket_stream_pipeline_t *pipeline,
	const unsigned char *aligned,
	size_t aligned_len,
	apr_pool_t *scratch_pool,
	tts_websocket_stream_pipeline_output_f output,
	void *context)
{
	size_t input_count;
	size_t output_capacity;
	size_t input_offset = 0;
	size_t output_count = 0;
	short *input_samples;
	short *output_samples;
	size_t i;

	if(aligned_len == 0) {
		return TTS_WEBSOCKET_STREAM_PIPELINE_OK;
	}
	if(!pipeline || !aligned || !scratch_pool || aligned_len % 2 != 0) {
		return pipeline_fail(pipeline, -1);
	}
	input_count = aligned_len / 2;
	output_capacity = tts_websocket_resampler_output_bound(
		pipeline->resampler, input_count);
	if(output_capacity == 0 ||
		input_count > SIZE_MAX / sizeof(*input_samples) ||
		output_capacity > SIZE_MAX / sizeof(*output_samples)) {
		return pipeline_fail(pipeline, -1);
	}
	input_samples = (short *)apr_palloc(
		scratch_pool, input_count * sizeof(*input_samples));
	output_samples = (short *)apr_palloc(
		scratch_pool, output_capacity * sizeof(*output_samples));
	if(!input_samples || !output_samples) {
		return pipeline_fail(pipeline, -1);
	}

	for(i = 0; i < input_count; ++i) {
		unsigned short value = (unsigned short)aligned[i * 2] |
			((unsigned short)aligned[i * 2 + 1] << 8);
		memcpy(&input_samples[i], &value, sizeof(value));
	}

	while(input_offset < input_count) {
		size_t input_available;
		size_t output_available;
		size_t consumed;
		size_t produced;
		int error_code;

		if(pipeline_is_stopped(pipeline)) {
			return TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED;
		}
		input_available = input_count - input_offset;
		output_available = output_capacity - output_count;
		if(output_available == 0) {
			return pipeline_fail(pipeline, -1);
		}
		consumed = input_available;
		produced = output_available;
		pipeline->process_calls++;
		error_code = tts_websocket_resampler_process(
			pipeline->resampler,
			input_samples + input_offset, &consumed,
			output_samples + output_count, &produced);
		if(consumed > input_available || produced > output_available) {
			return pipeline_fail(pipeline, -1);
		}
		pipeline->input_samples += consumed;
		pipeline->output_samples += produced;
		input_offset += consumed;
		output_count += produced;
		if(error_code != 0) {
			return pipeline_fail(pipeline, error_code);
		}
		if(consumed == 0 && produced == 0) {
			return pipeline_fail(pipeline, -1);
		}
	}

	return pipeline_emit(
		pipeline, output_samples, output_count, output, context);
}

tts_websocket_stream_pipeline_t *tts_websocket_stream_pipeline_create(
	apr_pool_t *pool,
	unsigned int input_rate,
	unsigned int output_rate,
	unsigned int channels,
	int quality,
	int *error_code)
{
	tts_websocket_stream_pipeline_t *pipeline;
	int resampler_error = 0;

	if(error_code) {
		*error_code = 0;
	}
	if(!pool) {
		if(error_code) {
			*error_code = -1;
		}
		return NULL;
	}
	pipeline = (tts_websocket_stream_pipeline_t *)apr_pcalloc(
		pool, sizeof(*pipeline));
	if(!pipeline) {
		if(error_code) {
			*error_code = -1;
		}
		return NULL;
	}
	pipeline->resampler = tts_websocket_resampler_create(
		input_rate, output_rate, channels, quality, &resampler_error);
	if(!pipeline->resampler) {
		if(error_code) {
			*error_code = resampler_error;
		}
		return NULL;
	}
	if(apr_thread_mutex_create(&pipeline->terminal_mutex,
		APR_THREAD_MUTEX_DEFAULT, pool) != APR_SUCCESS) {
		tts_websocket_resampler_destroy(pipeline->resampler);
		pipeline->resampler = NULL;
		if(error_code) {
			*error_code = -1;
		}
		return NULL;
	}
	return pipeline;
}

void tts_websocket_stream_pipeline_destroy(
	tts_websocket_stream_pipeline_t *pipeline)
{
	if(!pipeline) {
		return;
	}
	tts_websocket_resampler_destroy(pipeline->resampler);
	pipeline->resampler = NULL;
}

int tts_websocket_stream_pipeline_feed(
	tts_websocket_stream_pipeline_t *pipeline,
	const unsigned char *input,
	size_t input_size,
	apr_pool_t *scratch_pool,
	tts_websocket_stream_pipeline_output_f output,
	void *context)
{
	size_t combined_capacity;
	size_t carry_len;
	size_t aligned_len;
	unsigned char *combined;

	if(!pipeline || (!input && input_size > 0) || !scratch_pool ||
		pipeline->finished) {
		return pipeline_fail(pipeline, -1);
	}
	if(pipeline_is_stopped(pipeline)) {
		return TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED;
	}
	if(pipeline->carry_len > SIZE_MAX - input_size) {
		return pipeline_fail(pipeline, -1);
	}
	combined_capacity = pipeline->carry_len + input_size;
	combined = (unsigned char *)apr_palloc(
		scratch_pool, combined_capacity > 0 ? combined_capacity : 1);
	if(!combined) {
		return pipeline_fail(pipeline, -1);
	}
	carry_len = pipeline->carry_len;
	aligned_len = tts_websocket_pcm_accumulate(
		pipeline->carry, &carry_len, input, input_size,
		combined, combined_capacity, 2);
	if(aligned_len > combined_capacity || carry_len > 1) {
		return pipeline_fail(pipeline, -1);
	}
	pipeline->carry_len = carry_len;
	return pipeline_process_aligned(
		pipeline, combined, aligned_len, scratch_pool, output, context);
}

void tts_websocket_stream_pipeline_request_stop(
	tts_websocket_stream_pipeline_t *pipeline)
{
	if(!pipeline) {
		return;
	}
	/* The engine serializes this publication with ring writes. The pipeline
	 * flag itself is atomic so a finish already in progress can observe STOP
	 * without taking the terminal mutex in the opposite order. */
	apr_atomic_set32(&pipeline->stop_requested, 1);
}

int tts_websocket_stream_pipeline_finish(
	tts_websocket_stream_pipeline_t *pipeline,
	apr_pool_t *scratch_pool,
	tts_websocket_stream_pipeline_output_f output,
	void *context,
	size_t *orphan_bytes)
{
	size_t finish_capacity;
	size_t produced;
	short *finish_output;
	int error_code;
	int result = TTS_WEBSOCKET_STREAM_PIPELINE_OK;

	if(orphan_bytes) {
		*orphan_bytes = 0;
	}
	if(!pipeline || !scratch_pool || !pipeline->terminal_mutex) {
		return pipeline_fail(pipeline, -1);
	}
	apr_thread_mutex_lock(pipeline->terminal_mutex);
	if(pipeline_is_stopped(pipeline)) {
		result = TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED;
		goto unlock;
	}
	if(pipeline->finished || apr_atomic_read32(
		(volatile apr_uint32_t *)(uintptr_t)&pipeline->eof_ready) != 0) {
		goto unlock;
	}
	if(pipeline->carry_len > 1) {
		result = pipeline_fail(pipeline, -1);
		goto unlock;
	}
	if(pipeline->carry_len == 1) {
		pipeline->carry_len = 0;
		pipeline->orphan_bytes++;
		if(orphan_bytes) {
			*orphan_bytes = 1;
		}
	}
	finish_capacity = tts_websocket_resampler_output_bound(
		pipeline->resampler, 0);
	if(finish_capacity == 0 || finish_capacity > SIZE_MAX / sizeof(*finish_output)) {
		result = pipeline_fail(pipeline, -1);
		goto unlock;
	}
	finish_output = (short *)apr_palloc(
		scratch_pool, finish_capacity * sizeof(*finish_output));
	if(!finish_output) {
		result = pipeline_fail(pipeline, -1);
		goto unlock;
	}
	produced = finish_capacity;
	error_code = tts_websocket_resampler_finish(
		pipeline->resampler, finish_output, &produced);
	pipeline->finish_samples += produced;
	pipeline->output_samples += produced;
	if(error_code != 0) {
		result = pipeline_fail(pipeline, error_code);
		goto unlock;
	}
	if(pipeline_is_stopped(pipeline)) {
		result = TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED;
		goto unlock;
	}
	result = pipeline_emit(
		pipeline, finish_output, produced, output, context);
	if(result != TTS_WEBSOCKET_STREAM_PIPELINE_OK) {
		goto unlock;
	}
	if(pipeline_is_stopped(pipeline)) {
		result = TTS_WEBSOCKET_STREAM_PIPELINE_STOPPED;
		goto unlock;
	}
	pipeline->finished = 1;
	apr_atomic_set32(&pipeline->eof_ready, 1);

unlock:
	apr_thread_mutex_unlock(pipeline->terminal_mutex);
	return result;
}

int tts_websocket_stream_pipeline_stop_requested(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline_is_stopped(pipeline);
}

int tts_websocket_stream_pipeline_eof_ready(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline && apr_atomic_read32(
		(volatile apr_uint32_t *)(uintptr_t)&pipeline->eof_ready) != 0;
}

size_t tts_websocket_stream_pipeline_carry_bytes(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline ? pipeline->carry_len : 0;
}

size_t tts_websocket_stream_pipeline_input_samples(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline ? pipeline->input_samples : 0;
}

size_t tts_websocket_stream_pipeline_output_samples(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline ? pipeline->output_samples : 0;
}

size_t tts_websocket_stream_pipeline_process_calls(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline ? pipeline->process_calls : 0;
}

size_t tts_websocket_stream_pipeline_finish_samples(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline ? pipeline->finish_samples : 0;
}

size_t tts_websocket_stream_pipeline_orphan_bytes(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline ? pipeline->orphan_bytes : 0;
}

int tts_websocket_stream_pipeline_error(
	const tts_websocket_stream_pipeline_t *pipeline)
{
	return pipeline ? pipeline->error_code : -1;
}

int tts_websocket_stream_pipeline_join_worker(
	apr_thread_t **worker,
	apr_status_t *join_status)
{
	apr_status_t status;
	apr_status_t thread_status;

	if(join_status) {
		*join_status = APR_EINVAL;
	}
	if(!worker || !*worker) {
		return 0;
	}
	status = apr_thread_join(&thread_status, *worker);
	if(join_status) {
		*join_status = status;
	}
	if(status != APR_SUCCESS) {
		return 0;
	}
	*worker = NULL;
	return 1;
}
