#include "tts_websocket_resampler.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include <speex/speex_resampler.h>

#define TTS_WEBSOCKET_RESAMPLER_ZERO_CHUNK 256U

struct tts_websocket_resampler_t {
	SpeexResamplerState *state;
	unsigned int input_rate;
	unsigned int output_rate;
	unsigned int channels;
	int quality;
	size_t total_input;
	size_t total_output;
	size_t output_target;
	int input_latency;
	int finished;
	short zero_padding[TTS_WEBSOCKET_RESAMPLER_ZERO_CHUNK];
};

static int calculate_output_target(
	const tts_websocket_resampler_t *resampler,
	size_t input_samples,
	size_t *output_samples)
{
	size_t whole;
	size_t remainder;
	size_t whole_output;
	size_t fractional_output;

	whole = input_samples / resampler->input_rate;
	remainder = input_samples % resampler->input_rate;
	if (whole > SIZE_MAX / resampler->output_rate ||
		remainder > SIZE_MAX / resampler->output_rate) {
		return RESAMPLER_ERR_OVERFLOW;
	}
	whole_output = whole * resampler->output_rate;
	fractional_output = (remainder * resampler->output_rate) /
		resampler->input_rate;
	if (whole_output > SIZE_MAX - fractional_output) {
		return RESAMPLER_ERR_OVERFLOW;
	}
	*output_samples = whole_output + fractional_output;
	return RESAMPLER_ERR_SUCCESS;
}

static int update_output_target(tts_websocket_resampler_t *resampler)
{
	return calculate_output_target(
		resampler, resampler->total_input, &resampler->output_target);
}

static size_t min_size(size_t left, size_t right)
{
	return left < right ? left : right;
}

tts_websocket_resampler_t *tts_websocket_resampler_create(
	unsigned int input_rate, unsigned int output_rate,
	unsigned int channels, int quality, int *error_code)
{
	tts_websocket_resampler_t *resampler;
	int speex_error = RESAMPLER_ERR_SUCCESS;

	if (error_code) {
		*error_code = RESAMPLER_ERR_SUCCESS;
	}
	if (input_rate == 0 || output_rate == 0 || channels != 1 ||
		quality < SPEEX_RESAMPLER_QUALITY_MIN ||
		quality > SPEEX_RESAMPLER_QUALITY_MAX) {
		if (error_code) {
			*error_code = RESAMPLER_ERR_INVALID_ARG;
		}
		return NULL;
	}

	resampler = (tts_websocket_resampler_t *)calloc(1, sizeof(*resampler));
	if (!resampler) {
		if (error_code) {
			*error_code = RESAMPLER_ERR_ALLOC_FAILED;
		}
		return NULL;
	}

	resampler->input_rate = input_rate;
	resampler->output_rate = output_rate;
	resampler->channels = channels;
	resampler->quality = quality;
	resampler->state = speex_resampler_init(
		channels, input_rate, output_rate, quality, &speex_error);
	if (!resampler->state) {
		free(resampler);
		if (error_code) {
			*error_code = speex_error;
		}
		return NULL;
	}

	resampler->input_latency = speex_resampler_get_input_latency(
		resampler->state);
	speex_error = speex_resampler_skip_zeros(resampler->state);
	if (speex_error != RESAMPLER_ERR_SUCCESS) {
		speex_resampler_destroy(resampler->state);
		free(resampler);
		if (error_code) {
			*error_code = speex_error;
		}
		return NULL;
	}
	if (resampler->input_latency < 0) {
		resampler->input_latency = 0;
	}
	return resampler;
}

int tts_websocket_resampler_process(
	tts_websocket_resampler_t *resampler,
	const short *input, size_t *input_samples,
	short *output, size_t *output_samples)
{
	size_t input_capacity;
	size_t output_capacity;
	size_t input_consumed = 0;
	size_t output_produced = 0;
	int error_code = RESAMPLER_ERR_SUCCESS;

	if (!resampler || !input_samples || !output_samples) {
		return RESAMPLER_ERR_INVALID_ARG;
	}
	input_capacity = *input_samples;
	output_capacity = *output_samples;
	*input_samples = 0;
	*output_samples = 0;
	if ((!input && input_capacity > 0) ||
		(!output && output_capacity > 0)) {
		return RESAMPLER_ERR_INVALID_ARG;
	}
	if (resampler->finished) {
		return RESAMPLER_ERR_BAD_STATE;
	}

	while (input_consumed < input_capacity && output_produced < output_capacity) {
		size_t input_chunk = min_size(
			input_capacity - input_consumed, (size_t)UINT_MAX);
		size_t output_chunk = min_size(
			output_capacity - output_produced, (size_t)UINT_MAX);
		spx_uint32_t speex_input = (spx_uint32_t)input_chunk;
		spx_uint32_t speex_output = (spx_uint32_t)output_chunk;

		error_code = speex_resampler_process_int(
			resampler->state, 0,
			input + input_consumed, &speex_input,
			output + output_produced, &speex_output);
		input_consumed += speex_input;
		output_produced += speex_output;
		if (error_code != RESAMPLER_ERR_SUCCESS) {
			break;
		}
		if (speex_input == 0 && speex_output == 0) {
			error_code = RESAMPLER_ERR_BAD_STATE;
			break;
		}
	}

	if (resampler->total_input > SIZE_MAX - input_consumed ||
		resampler->total_output > SIZE_MAX - output_produced) {
		error_code = RESAMPLER_ERR_OVERFLOW;
	} else {
		resampler->total_input += input_consumed;
		resampler->total_output += output_produced;
		if (update_output_target(resampler) != RESAMPLER_ERR_SUCCESS) {
			error_code = RESAMPLER_ERR_OVERFLOW;
		}
	}
	*input_samples = input_consumed;
	*output_samples = output_produced;
	return error_code;
}

int tts_websocket_resampler_finish(
	tts_websocket_resampler_t *resampler,
	short *output, size_t *output_samples)
{
	size_t output_capacity;
	size_t output_produced = 0;
	size_t zeros_remaining;
	int error_code;

	if (!resampler || !output_samples) {
		return RESAMPLER_ERR_INVALID_ARG;
	}
	output_capacity = *output_samples;
	*output_samples = 0;
	if (output_capacity > 0 && !output) {
		return RESAMPLER_ERR_INVALID_ARG;
	}
	if (resampler->finished) {
		return RESAMPLER_ERR_SUCCESS;
	}
	if (update_output_target(resampler) != RESAMPLER_ERR_SUCCESS) {
		return RESAMPLER_ERR_OVERFLOW;
	}
	if (resampler->total_output >= resampler->output_target) {
		resampler->finished = 1;
		return RESAMPLER_ERR_SUCCESS;
	}

	zeros_remaining = (size_t)resampler->input_latency;
	while (resampler->total_output < resampler->output_target &&
		zeros_remaining > 0 && output_produced < output_capacity) {
		size_t zero_chunk = min_size(zeros_remaining,
			(size_t)TTS_WEBSOCKET_RESAMPLER_ZERO_CHUNK);
		size_t output_chunk = min_size(
			output_capacity - output_produced,
			resampler->output_target - resampler->total_output);
		spx_uint32_t speex_input = (spx_uint32_t)zero_chunk;
		spx_uint32_t speex_output = (spx_uint32_t)output_chunk;

		error_code = speex_resampler_process_int(
			resampler->state, 0, resampler->zero_padding,
			&speex_input, output + output_produced, &speex_output);
		if (error_code != RESAMPLER_ERR_SUCCESS) {
			*output_samples = output_produced;
			return error_code;
		}
		zeros_remaining -= speex_input;
		output_produced += speex_output;
		resampler->total_output += speex_output;
		if (speex_input == 0 && speex_output == 0) {
			*output_samples = output_produced;
			return RESAMPLER_ERR_BAD_STATE;
		}
	}

	*output_samples = output_produced;
	if (resampler->total_output != resampler->output_target) {
		return RESAMPLER_ERR_OVERFLOW;
	}
	resampler->finished = 1;
	return RESAMPLER_ERR_SUCCESS;
}

size_t tts_websocket_resampler_output_bound(
	const tts_websocket_resampler_t *resampler, size_t input_samples)
{
	size_t target;
	size_t latency_output;

	if (!resampler || calculate_output_target(resampler, input_samples, &target) !=
		RESAMPLER_ERR_SUCCESS) {
		return 0;
	}
	if (resampler->input_latency > 0) {
		size_t latency = (size_t)resampler->input_latency;
		if (latency > SIZE_MAX / resampler->output_rate) {
			return 0;
		}
		latency_output = (latency * resampler->output_rate +
			resampler->input_rate - 1) / resampler->input_rate;
		if (latency_output == SIZE_MAX ||
			target > SIZE_MAX - latency_output - 1) {
			return 0;
		}
		target += latency_output + 1;
	}
	return target;
}

void tts_websocket_resampler_destroy(
	tts_websocket_resampler_t *resampler)
{
	if (!resampler) {
		return;
	}
	speex_resampler_destroy(resampler->state);
	free(resampler);
}
