#ifndef TTS_WEBSOCKET_RESAMPLER_H
#define TTS_WEBSOCKET_RESAMPLER_H

#include <stddef.h>

typedef struct tts_websocket_resampler_t tts_websocket_resampler_t;

tts_websocket_resampler_t *tts_websocket_resampler_create(
	unsigned int input_rate, unsigned int output_rate,
	unsigned int channels, int quality, int *error_code);

int tts_websocket_resampler_process(
	tts_websocket_resampler_t *resampler,
	const short *input, size_t *input_samples,
	short *output, size_t *output_samples);

int tts_websocket_resampler_finish(
	tts_websocket_resampler_t *resampler,
	short *output, size_t *output_samples);

size_t tts_websocket_resampler_output_bound(
	const tts_websocket_resampler_t *resampler,
	size_t input_samples);

void tts_websocket_resampler_destroy(
	tts_websocket_resampler_t *resampler);

#endif
