#include "../src/tts_websocket_pcm.h"
#include "../src/tts_websocket_resampler.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_RATE 24000U
#define OUTPUT_RATE 8000U
#define RESAMPLER_QUALITY 10
#define OUTPUT_SLOP 1024U
#define PI 3.14159265358979323846

static short rounded_sample(double value)
{
	if (value > 32767.0) {
		return 32767;
	}
	if (value < -32768.0) {
		return -32768;
	}
	return (short)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

static void fill_sine(short *samples, size_t sample_count,
	double frequency, double amplitude)
{
	double phase = 0.0;
	double phase_step = 2.0 * PI * frequency / (double)INPUT_RATE;
	size_t i;

	for (i = 0; i < sample_count; ++i) {
		samples[i] = rounded_sample(amplitude * sin(phase));
		phase += phase_step;
		if (phase >= 2.0 * PI) {
			phase -= 2.0 * PI;
		}
	}
}

static int open_resampler(tts_websocket_resampler_t **resampler)
{
	int error_code = -1;

	*resampler = tts_websocket_resampler_create(
		INPUT_RATE, OUTPUT_RATE, 1, RESAMPLER_QUALITY, &error_code);
	return *resampler != NULL && error_code == 0;
}

static int same_samples(const short *left, size_t left_count,
	const short *right, size_t right_count)
{
	return left_count == right_count &&
		(left_count == 0 || memcmp(left, right, left_count * sizeof(short)) == 0);
}

static int run_sample_partition(const short *input, size_t input_count,
	const size_t *chunks, size_t chunk_count,
	short **result, size_t *result_count)
{
	tts_websocket_resampler_t *resampler = NULL;
	short *output = NULL;
	size_t output_capacity = input_count / 3 + OUTPUT_SLOP;
	size_t output_count = 0;
	size_t input_offset = 0;
	size_t chunk_index = 0;
	size_t bound;
	int ok = 0;

	*result = NULL;
	*result_count = 0;
	if ((!input && input_count > 0) || !chunks || chunk_count == 0 ||
		!open_resampler(&resampler)) {
		return 0;
	}

	bound = tts_websocket_resampler_output_bound(resampler, input_count);
	if (bound < input_count / 3) {
		goto cleanup;
	}

	output = (short *)calloc(output_capacity, sizeof(short));
	if (!output) {
		goto cleanup;
	}

	while (input_offset < input_count) {
		size_t requested = chunks[chunk_index % chunk_count];
		size_t input_samples;
		size_t output_samples;
		int error_code;

		if (requested == 0) {
			goto cleanup;
		}
		if (requested > input_count - input_offset) {
			requested = input_count - input_offset;
		}
		input_samples = requested;
		output_samples = output_capacity - output_count;
		error_code = tts_websocket_resampler_process(
			resampler, input + input_offset, &input_samples,
			output + output_count, &output_samples);
		if (error_code != 0 || input_samples != requested ||
			output_samples > output_capacity - output_count) {
			goto cleanup;
		}
		input_offset += input_samples;
		output_count += output_samples;
		++chunk_index;
	}

	{
		size_t output_samples = output_capacity - output_count;
		int error_code = tts_websocket_resampler_finish(
			resampler, output + output_count, &output_samples);
		if (error_code != 0 || output_samples > output_capacity - output_count) {
			goto cleanup;
		}
		output_count += output_samples;
	}

	if (output_count != input_count / 3) {
		goto cleanup;
	}

	*result = output;
	*result_count = output_count;
	output = NULL;
	ok = 1;

cleanup:
	free(output);
	tts_websocket_resampler_destroy(resampler);
	return ok;
}

static int run_byte_partition(const short *input, size_t input_count,
	int mode, size_t split,
	short **result, size_t *result_count)
{
	tts_websocket_resampler_t *resampler = NULL;
	unsigned char *bytes = NULL;
	unsigned char *aligned = NULL;
	unsigned char carry[2] = {0, 0};
	short *aligned_samples = NULL;
	short *output = NULL;
	size_t byte_count = input_count * 2;
	size_t output_capacity = input_count / 3 + OUTPUT_SLOP;
	size_t output_count = 0;
	size_t byte_offset = 0;
	size_t carry_length = 0;
	unsigned int random_seed = 0x13579BDFU;
	int ok = 0;
	size_t i;

	*result = NULL;
	*result_count = 0;
	if ((!input && input_count > 0) || !open_resampler(&resampler)) {
		return 0;
	}

	bytes = (unsigned char *)malloc(byte_count > 0 ? byte_count : 1);
	aligned = (unsigned char *)malloc(byte_count + 2);
	aligned_samples = (short *)malloc((input_count + 1) * sizeof(short));
	output = (short *)calloc(output_capacity, sizeof(short));
	if (!bytes || !aligned || !aligned_samples || !output) {
		goto cleanup;
	}

	for (i = 0; i < input_count; ++i) {
		unsigned short value = (unsigned short)input[i];
		bytes[i * 2] = (unsigned char)(value & 0xFFU);
		bytes[i * 2 + 1] = (unsigned char)(value >> 8);
	}

	while (byte_offset < byte_count) {
		size_t chunk_size;
		size_t aligned_length;

		if (mode == 0) {
			if (byte_offset == 0) {
				chunk_size = split > byte_count ? byte_count : split;
				if (chunk_size == 0) {
					chunk_size = byte_count;
				}
			} else {
				chunk_size = byte_count - byte_offset;
			}
		} else if (mode == 1) {
			chunk_size = 1;
		} else {
			random_seed = random_seed * 1664525U + 1013904223U;
			chunk_size = 1 + (random_seed % 29U);
		}
		if (chunk_size > byte_count - byte_offset) {
			chunk_size = byte_count - byte_offset;
		}

		aligned_length = tts_websocket_pcm_accumulate(
			carry, &carry_length,
			bytes + byte_offset, chunk_size, aligned, byte_count + 2, 2);
		if (aligned_length % 2 != 0) {
			goto cleanup;
		}

		if (aligned_length > 0) {
			size_t sample_count = aligned_length / 2;
			size_t input_samples;
			size_t output_samples;
			int error_code;

			for (i = 0; i < sample_count; ++i) {
				unsigned short value = (unsigned short)aligned[i * 2] |
					((unsigned short)aligned[i * 2 + 1] << 8);
				memcpy(&aligned_samples[i], &value, sizeof(value));
			}
			input_samples = sample_count;
			output_samples = output_capacity - output_count;
			error_code = tts_websocket_resampler_process(
				resampler, aligned_samples, &input_samples,
				output + output_count, &output_samples);
			if (error_code != 0 || input_samples != sample_count ||
				output_samples > output_capacity - output_count) {
				goto cleanup;
			}
			output_count += output_samples;
		}
		byte_offset += chunk_size;
	}

	if (carry_length != 0) {
		goto cleanup;
	}

	{
		size_t output_samples = output_capacity - output_count;
		int error_code = tts_websocket_resampler_finish(
			resampler, output + output_count, &output_samples);
		if (error_code != 0 || output_samples > output_capacity - output_count) {
			goto cleanup;
		}
		output_count += output_samples;
	}

	if (output_count != input_count / 3) {
		goto cleanup;
	}

	*result = output;
	*result_count = output_count;
	output = NULL;
	ok = 1;

cleanup:
	free(bytes);
	free(aligned);
	free(aligned_samples);
	free(output);
	tts_websocket_resampler_destroy(resampler);
	return ok;
}

static double rms(const short *samples, size_t begin, size_t end)
{
	double sum = 0.0;
	size_t i;

	if (end <= begin) {
		return 0.0;
	}
	for (i = begin; i < end; ++i) {
		double value = (double)samples[i];
		sum += value * value;
	}
	return sqrt(sum / (double)(end - begin));
}

static int test_exact_length_and_chunk_invariance(void)
{
	const size_t input_count = 12002;
	const size_t fixed_chunks[] = {1, 7, 31, 160, 511};
	const size_t contiguous_chunks[] = {12002};
	short *input = NULL;
	short *contiguous = NULL;
	short *partitioned = NULL;
	size_t contiguous_count = 0;
	size_t partitioned_count = 0;
	int ok = 0;

	input = (short *)malloc(input_count * sizeof(short));
	if (!input) {
		goto cleanup;
	}
	fill_sine(input, input_count, 1000.0, 12000.0);
	input[input_count - 40] = 26000;
	if (!run_sample_partition(input, input_count, contiguous_chunks, 1,
		&contiguous, &contiguous_count) ||
		!run_sample_partition(input, input_count, fixed_chunks, 5,
		&partitioned, &partitioned_count) ||
		contiguous_count != input_count / 3 ||
		!same_samples(contiguous, contiguous_count,
			partitioned, partitioned_count)) {
		goto cleanup;
	}
	ok = 1;

cleanup:
	free(input);
	free(contiguous);
	free(partitioned);
	return ok;
}

static int test_passband_300hz_1khz_3400hz(void)
{
	const double frequencies[] = {300.0, 1000.0, 3400.0};
	const size_t input_count = 24000;
	const size_t chunks[] = {24000};
	short *input = NULL;
	short *output = NULL;
	size_t output_count = 0;
	size_t i;
	int ok = 0;

	input = (short *)malloc(input_count * sizeof(short));
	if (!input) {
		return 0;
	}
	for (i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); ++i) {
		double input_rms;
		double output_rms;
		double loss_db;
		fill_sine(input, input_count, frequencies[i], 12000.0);
		free(output);
		output = NULL;
		if (!run_sample_partition(input, input_count, chunks, 1,
			&output, &output_count) || output_count != 8000) {
			goto cleanup;
		}
		input_rms = rms(input, 1536, input_count - 1536);
		output_rms = rms(output, 512, output_count - 512);
		if (input_rms <= 0.0 || output_rms <= 0.0) {
			goto cleanup;
		}
		loss_db = 20.0 * log10(output_rms / input_rms);
		if (loss_db < -1.0) {
			goto cleanup;
		}
	}
	ok = 1;

cleanup:
	free(input);
	free(output);
	return ok;
}

static int test_stopband_6khz_10khz(void)
{
	const double frequencies[] = {6000.0, 10000.0};
	const size_t input_count = 24000;
	const size_t chunks[] = {24000};
	short *input = NULL;
	short *output = NULL;
	size_t output_count = 0;
	size_t i;
	int ok = 0;

	input = (short *)malloc(input_count * sizeof(short));
	if (!input) {
		return 0;
	}
	for (i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); ++i) {
		double input_rms;
		double output_rms;
		double residual_db;
		fill_sine(input, input_count, frequencies[i], 12000.0);
		free(output);
		output = NULL;
		if (!run_sample_partition(input, input_count, chunks, 1,
			&output, &output_count) || output_count != 8000) {
			goto cleanup;
		}
		input_rms = rms(input, 1536, input_count - 1536);
		output_rms = rms(output, 512, output_count - 512);
		if (input_rms <= 0.0) {
			goto cleanup;
		}
		if (output_rms == 0.0) {
			continue;
		}
		residual_db = 20.0 * log10(output_rms / input_rms);
		if (residual_db > -60.0) {
			goto cleanup;
		}
	}
	ok = 1;

cleanup:
	free(input);
	free(output);
	return ok;
}

static int test_short_and_non_multiple_lengths(void)
{
	const size_t lengths[] = {0, 1, 2, 3, 4, 5, 7, 10, 31, 32, 33,
		160, 161, 511, 512, 513};
	const size_t chunks[] = {1, 7, 31};
	short *input = (short *)calloc(514, sizeof(short));
	short *output = NULL;
	size_t output_count = 0;
	size_t i;
	int ok = 0;

	if (!input) {
		return 0;
	}
	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		size_t j;
		for (j = 0; j < lengths[i]; ++j) {
			input[j] = (short)(j * 113 - 2000);
		}
		if (!run_sample_partition(input, lengths[i], chunks, 3,
			&output, &output_count) || output_count != lengths[i] / 3) {
			goto cleanup;
		}
		free(output);
		output = NULL;
	}
	ok = 1;

cleanup:
	free(input);
	free(output);
	return ok;
}

static int test_arbitrary_byte_partitions_match_contiguous_input(void)
{
	const size_t input_count = 1024;
	const size_t sample_chunks[] = {1024};
	short *input = NULL;
	short *expected = NULL;
	short *actual = NULL;
	size_t expected_count = 0;
	size_t actual_count = 0;
	size_t split;
	int ok = 0;

	input = (short *)malloc(input_count * sizeof(short));
	if (!input) {
		return 0;
	}
	fill_sine(input, input_count, 700.0, 10000.0);
	input[input_count - 40] = 24000;
	if (!run_sample_partition(input, input_count, sample_chunks, 1,
		&expected, &expected_count)) {
		goto cleanup;
	}

	for (split = 0; split <= 64; ++split) {
		free(actual);
		actual = NULL;
		if (!run_byte_partition(input, input_count, 0, split,
			&actual, &actual_count) ||
			!same_samples(expected, expected_count, actual, actual_count)) {
			goto cleanup;
		}
	}
	free(actual);
	actual = NULL;
	if (!run_byte_partition(input, input_count, 1, 0,
		&actual, &actual_count) ||
		!same_samples(expected, expected_count, actual, actual_count)) {
		goto cleanup;
	}
	free(actual);
	actual = NULL;
	if (!run_byte_partition(input, input_count, 2, 0,
		&actual, &actual_count) ||
		!same_samples(expected, expected_count, actual, actual_count)) {
		goto cleanup;
	}
	ok = 1;

cleanup:
	free(input);
	free(expected);
	free(actual);
	return ok;
}

static int test_finish_preserves_nonzero_tail(void)
{
	const size_t input_count = 1200;
	const size_t expected_count = 400;
	tts_websocket_resampler_t *resampler = NULL;
	short *input = NULL;
	short *output = NULL;
	size_t output_capacity = expected_count + OUTPUT_SLOP;
	size_t output_count;
	size_t finish_count;
	size_t input_samples;
	size_t i;
	int ok = 0;

	input = (short *)calloc(input_count, sizeof(short));
	output = (short *)calloc(output_capacity, sizeof(short));
	if (!input || !output || !open_resampler(&resampler)) {
		goto cleanup;
	}
	input[input_count - 32] = 28000;
	input_samples = input_count;
	output_count = output_capacity;
	if (tts_websocket_resampler_process(resampler, input, &input_samples,
		output, &output_count) != 0 || input_samples != input_count ||
		output_count > output_capacity) {
		goto cleanup;
	}
	finish_count = output_capacity - output_count;
	if (tts_websocket_resampler_finish(resampler, output + output_count,
		&finish_count) != 0 || finish_count == 0 ||
		output_count + finish_count != expected_count) {
		goto cleanup;
	}
	for (i = output_count; i < expected_count; ++i) {
		if (output[i] != 0) {
			ok = 1;
			break;
		}
	}

cleanup:
	free(input);
	free(output);
	tts_websocket_resampler_destroy(resampler);
	return ok;
}

static int test_finish_is_idempotent(void)
{
	const size_t input_count = 1200;
	short input[1200];
	short output[1200];
	tts_websocket_resampler_t *resampler = NULL;
	size_t input_samples = input_count;
	size_t output_samples = sizeof(output) / sizeof(output[0]);
	size_t second_output_samples = sizeof(output) / sizeof(output[0]);
	size_t i;
	int ok = 0;

	for (i = 0; i < input_count; ++i) {
		input[i] = (short)(i * 17 - 9000);
	}
	if (!open_resampler(&resampler) ||
		tts_websocket_resampler_process(resampler, input, &input_samples,
			output, &output_samples) != 0 || input_samples != input_count ||
		tts_websocket_resampler_finish(resampler, output + output_samples,
			&second_output_samples) != 0 || second_output_samples == 0) {
		goto cleanup;
	}
	second_output_samples = sizeof(output) / sizeof(output[0]);
	if (tts_websocket_resampler_finish(resampler, output,
		&second_output_samples) != 0 || second_output_samples != 0) {
		goto cleanup;
	}
	input_samples = 1;
	output_samples = sizeof(output) / sizeof(output[0]);
	if (tts_websocket_resampler_process(resampler, input, &input_samples,
		output, &output_samples) == 0) {
		goto cleanup;
	}
	ok = 1;

cleanup:
	tts_websocket_resampler_destroy(resampler);
	return ok;
}

static int test_state_isolation_between_sessions(void)
{
	const size_t input_count = 2400;
	const size_t chunks[] = {1, 7, 31, 160, 511};
	short *first_input = NULL;
	short *second_input = NULL;
	short *discarded = NULL;
	short *second_output = NULL;
	short *fresh_output = NULL;
	size_t discarded_count = 0;
	size_t second_count = 0;
	size_t fresh_count = 0;
	int ok = 0;

	first_input = (short *)malloc(input_count * sizeof(short));
	second_input = (short *)malloc(input_count * sizeof(short));
	if (!first_input || !second_input) {
		goto cleanup;
	}
	fill_sine(first_input, input_count, 300.0, 11000.0);
	fill_sine(second_input, input_count, 1700.0, 11000.0);
	if (!run_sample_partition(first_input, input_count, chunks, 5,
		&discarded, &discarded_count) ||
		!run_sample_partition(second_input, input_count, chunks, 5,
		&second_output, &second_count) ||
		!run_sample_partition(second_input, input_count, chunks, 5,
		&fresh_output, &fresh_count) ||
		!same_samples(second_output, second_count, fresh_output, fresh_count)) {
		goto cleanup;
	}
	ok = 1;

cleanup:
	free(first_input);
	free(second_input);
	free(discarded);
	free(second_output);
	free(fresh_output);
	return ok;
}

static int test_invalid_arguments_fail(void)
{
	const unsigned int invalid_rates[][2] = {
		{0, OUTPUT_RATE}, {INPUT_RATE, 0}
	};
	short input[2] = {1, 2};
	short output[2] = {0, 0};
	tts_websocket_resampler_t *resampler = NULL;
	size_t input_samples;
	size_t output_samples;
	int error_code;
	size_t i;

	for (i = 0; i < sizeof(invalid_rates) / sizeof(invalid_rates[0]); ++i) {
		error_code = 0;
		if (tts_websocket_resampler_create(invalid_rates[i][0],
			invalid_rates[i][1], 1, RESAMPLER_QUALITY, &error_code) != NULL ||
			error_code == 0) {
			return 0;
		}
	}
	error_code = 0;
	if (tts_websocket_resampler_create(INPUT_RATE, OUTPUT_RATE, 0,
		RESAMPLER_QUALITY, &error_code) != NULL || error_code == 0) {
		return 0;
	}
	error_code = 0;
	if (tts_websocket_resampler_create(INPUT_RATE, OUTPUT_RATE, 1,
		-1, &error_code) != NULL || error_code == 0) {
		return 0;
	}
	error_code = 0;
	if (tts_websocket_resampler_create(INPUT_RATE, OUTPUT_RATE, 1,
		11, &error_code) != NULL || error_code == 0) {
		return 0;
	}
	if (tts_websocket_resampler_output_bound(NULL, 1) != 0) {
		return 0;
	}
	if (tts_websocket_resampler_process(NULL, input, &input_samples,
		output, &output_samples) == 0) {
		return 0;
	}
	if (!open_resampler(&resampler)) {
		return 0;
	}
	input_samples = 1;
	output_samples = 1;
	if (tts_websocket_resampler_process(resampler, NULL, &input_samples,
		output, &output_samples) == 0) {
		tts_websocket_resampler_destroy(resampler);
		return 0;
	}
	input_samples = 1;
	output_samples = 1;
	if (tts_websocket_resampler_process(resampler, input, &input_samples,
		NULL, &output_samples) == 0) {
		tts_websocket_resampler_destroy(resampler);
		return 0;
	}
	output_samples = 1;
	if (tts_websocket_resampler_finish(resampler, NULL, &output_samples) == 0) {
		tts_websocket_resampler_destroy(resampler);
		return 0;
	}
	tts_websocket_resampler_destroy(resampler);
	tts_websocket_resampler_destroy(NULL);
	return 1;
}

int main(void)
{
	if (!test_exact_length_and_chunk_invariance()) {
		fprintf(stderr, "test_exact_length_and_chunk_invariance failed\n");
		return 1;
	}
	if (!test_passband_300hz_1khz_3400hz()) {
		fprintf(stderr, "test_passband_300hz_1khz_3400hz failed\n");
		return 1;
	}
	if (!test_stopband_6khz_10khz()) {
		fprintf(stderr, "test_stopband_6khz_10khz failed\n");
		return 1;
	}
	if (!test_short_and_non_multiple_lengths()) {
		fprintf(stderr, "test_short_and_non_multiple_lengths failed\n");
		return 1;
	}
	if (!test_arbitrary_byte_partitions_match_contiguous_input()) {
		fprintf(stderr, "test_arbitrary_byte_partitions_match_contiguous_input failed\n");
		return 1;
	}
	if (!test_finish_preserves_nonzero_tail()) {
		fprintf(stderr, "test_finish_preserves_nonzero_tail failed\n");
		return 1;
	}
	if (!test_finish_is_idempotent()) {
		fprintf(stderr, "test_finish_is_idempotent failed\n");
		return 1;
	}
	if (!test_state_isolation_between_sessions()) {
		fprintf(stderr, "test_state_isolation_between_sessions failed\n");
		return 1;
	}
	if (!test_invalid_arguments_fail()) {
		fprintf(stderr, "test_invalid_arguments_fail failed\n");
		return 1;
	}
	puts("9/9 SpeexDSP resampler tests passed");
	return 0;
}
