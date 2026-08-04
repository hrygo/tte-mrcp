#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* A minimal local implementation of the resample logic (copied/adapted)
   This file is a unit test harness and does not depend on APR. */

char* funasr_resample_8k_to_16k(void *pool, const char *in_buf, size_t in_size, size_t *out_size, int channels)
{
    if (!in_buf || in_size == 0 || channels <= 0) {
        *out_size = 0;
        return NULL;
    }

    const char *data_ptr = in_buf;
    size_t data_size = in_size;
    int used_channels = channels;

    if (in_size >= 12 && memcmp(in_buf, "RIFF", 4) == 0 && memcmp(in_buf + 8, "WAVE", 4) == 0) {
        size_t offset = 12;
        while (offset + 8 <= in_size) {
            const char *chunk = in_buf + offset;
            uint32_t chunk_size = (uint32_t)((unsigned char)chunk[4] | ((unsigned char)chunk[5] << 8) | ((unsigned char)chunk[6] << 16) | ((unsigned char)chunk[7] << 24));
            if (offset + 8 + chunk_size > in_size) break;

            if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
                const unsigned char *fmt = (const unsigned char*)(chunk + 8);
                uint16_t audio_format = (uint16_t)(fmt[0] | (fmt[1] << 8));
                uint16_t wav_channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
                uint16_t bits_per_sample = (uint16_t)(fmt[14] | (fmt[15] << 8));
                if (audio_format != 1 || bits_per_sample != 16) {
                    *out_size = 0;
                    return NULL;
                }
                used_channels = (int)wav_channels;
            } else if (memcmp(chunk, "data", 4) == 0) {
                data_ptr = chunk + 8;
                data_size = (size_t)chunk_size;
                break;
            }

            offset += 8 + chunk_size;
            if (chunk_size & 1) offset++; /* pad */
        }
    }

    if (data_size == 0) {
        *out_size = 0;
        return NULL;
    }

    const int16_t *in_samples = (const int16_t*)data_ptr;
    size_t bytes_per_sample = sizeof(int16_t);
    size_t frame_samples = data_size / (bytes_per_sample * used_channels);
    size_t out_frame_samples = frame_samples * 2;
    size_t total_out_samples = out_frame_samples * used_channels;
    size_t buf_size = total_out_samples * bytes_per_sample;

    char *out = (char*)malloc(buf_size);
    if (!out) {
        *out_size = 0;
        return NULL;
    }
    int16_t *out_samples = (int16_t*)out;

    for (size_t n = 0; n < frame_samples; n++) {
        for (int ch = 0; ch < used_channels; ch++) {
            size_t in_idx = n * used_channels + ch;
            size_t out_idx1 = (n * 2) * used_channels + ch;
            size_t out_idx2 = (n * 2 + 1) * used_channels + ch;
            int16_t s = in_samples[in_idx];
            int16_t s_next = s;
            if (n + 1 < frame_samples) {
                s_next = in_samples[(n + 1) * used_channels + ch];
            }
            out_samples[out_idx1] = s;
            out_samples[out_idx2] = (int16_t)(((int)s + (int)s_next) / 2);
        }
    }

    *out_size = buf_size;
    return out;
}

/* Build a tiny 8kHz mono 16-bit WAV in memory with 4 samples and test resampling */
int main(void)
{
    /* Prepare 4 samples: 1000, 2000, -1000, -2000 */
    int16_t samples[4] = {1000, 2000, -1000, -2000};
    uint16_t num_channels = 1;
    uint32_t sample_rate = 8000;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    uint16_t block_align = num_channels * bits_per_sample / 8;

    uint32_t data_bytes = sizeof(samples);
    uint32_t fmt_chunk_size = 16;
    uint32_t riff_size = 4 + (8 + fmt_chunk_size) + (8 + data_bytes);

    size_t wav_size = 12 + (8 + fmt_chunk_size) + (8 + data_bytes);
    unsigned char *wav = (unsigned char*)malloc(wav_size);
    if (!wav) return 2;
    unsigned char *p = wav;

    memcpy(p, "RIFF", 4); p += 4;
    /* RIFF size */ memcpy(p, &riff_size, 4); p += 4;
    memcpy(p, "WAVE", 4); p += 4;

    /* fmt chunk */ memcpy(p, "fmt ", 4); p += 4;
    memcpy(p, &fmt_chunk_size, 4); p += 4;
    uint16_t audio_format = 1;
    memcpy(p, &audio_format, 2); p += 2;
    memcpy(p, &num_channels, 2); p += 2;
    memcpy(p, &sample_rate, 4); p += 4;
    memcpy(p, &byte_rate, 4); p += 4;
    memcpy(p, &block_align, 2); p += 2;
    memcpy(p, &bits_per_sample, 2); p += 2;

    /* data chunk */ memcpy(p, "data", 4); p += 4;
    memcpy(p, &data_bytes, 4); p += 4;
    memcpy(p, samples, data_bytes); p += data_bytes;

    /* call resample */
    size_t out_size = 0;
    char *out = funasr_resample_8k_to_16k(NULL, (const char*)wav, wav_size, &out_size, 1);
    if (!out) {
        printf("FAIL: resample returned NULL\n");
        free(wav);
        return 1;
    }

    /* Expect out_size == data_bytes * 2 */
    if (out_size != data_bytes * 2) {
        printf("FAIL: unexpected out_size %zu (expected %u)\n", out_size, (unsigned) (data_bytes * 2));
        free(out);
        free(wav);
        return 1;
    }

    int16_t *out_samples = (int16_t*)out;
    size_t out_frames = out_size / (sizeof(int16_t) * num_channels);
    if (out_frames != 8) {
        printf("FAIL: unexpected out_frames %zu\n", out_frames);
        free(out);
        free(wav);
        return 1;
    }

    /* Expected sequence: s0, avg(s0,s1), s1, avg(s1,s2), s2, avg(s2,s3), s3, avg(s3,s3) */
    int16_t expected[8];
    expected[0] = samples[0];
    expected[1] = (samples[0] + samples[1]) / 2;
    expected[2] = samples[1];
    expected[3] = (samples[1] + samples[2]) / 2;
    expected[4] = samples[2];
    expected[5] = (samples[2] + samples[3]) / 2;
    expected[6] = samples[3];
    expected[7] = (samples[3] + samples[3]) / 2;

    int ok = 1;
    for (size_t i = 0; i < 8; i++) {
        if (out_samples[i] != expected[i]) {
            printf("FAIL: sample[%zu] = %d (expected %d)\n", i, out_samples[i], expected[i]);
            ok = 0;
        }
    }

    if (ok) printf("PASS\n");

    free(out);
    free(wav);
    return ok ? 0 : 1;
}
