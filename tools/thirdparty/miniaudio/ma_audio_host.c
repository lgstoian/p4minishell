/*
 * ma_audio_host.c - host-only miniaudio helper for P4MiniShell's audio gate
 * and host-side audio asset preparation.
 *
 * P4MiniShell host tool: SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * P4MiniShell host tool: SPDX-License-Identifier: MIT
 *
 * Vendored dependency: miniaudio v0.11.26 (commit
 * 877ea4a7c2601c7b0f2a44e83dfac42e6470fea6), dual public domain (Unlicense)
 * or MIT No Attribution (MIT-0). Its unmodified upstream header and LICENSE
 * ship in this directory; miniaudio is NOT part of the firmware or ESP-IDF
 * build and is NOT an audio path in components/audio.
 *
 * Usage:
 *   ma_audio_host decode  <input.wav> <output.pcm>
 *       Decode a WAV to raw little-endian s16 PCM at its own rate/channels.
 *       Prints: ma.decoder channels=<n> rate=<hz> frames=<n> bytes=<n>
 *   ma_audio_host prepare <input> <rate> <channels> <output.pcm>
 *       Decode ANY miniaudio-supported file (WAV/MP3/FLAC/Vorbis...),
 *       resample and convert to s16 at the exact rate/channels the firmware's
 *       one wavplay path accepts. Prints:
 *         ma.prepare channels=<n> rate=<hz> frames=<n> bytes=<n>
 */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_pcm(const char *path, const void *pcm, size_t bytes)
{
    FILE *out = fopen(path, "wb");

    if (out == NULL) {
        fprintf(stderr, "ma: cannot open output %s\n", path);
        return 0;
    }
    if (fwrite(pcm, 1u, bytes, out) != bytes) {
        fprintf(stderr, "ma: short write to %s\n", path);
        fclose(out);
        return 0;
    }
    if (fclose(out) != 0) {
        fprintf(stderr, "ma: close failed for %s\n", path);
        return 0;
    }
    return 1;
}

/* Decode to s16 at the requested rate/channels. A resampled stream's length
 * is not known up front, so read to EOF into a growing buffer. */
static int decode_all(const char *input, unsigned rate, unsigned channels,
                      void **pcm_out, ma_uint64 *frames_out)
{
    ma_decoder decoder;
    ma_decoder_config config;
    ma_result result;
    ma_uint64 cap = 4096;
    ma_uint64 total = 0;
    unsigned char *pcm;
    size_t frame_bytes;

    if (rate == 0 || (channels != 1 && channels != 2)) {
        fprintf(stderr, "ma: bad rate/channels\n");
        return 0;
    }
    frame_bytes = (size_t)channels * 2u;
    config = ma_decoder_config_init(ma_format_s16, channels, rate);
    result = ma_decoder_init_file(input, &config, &decoder);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma: cannot decode %s: %s\n", input,
                ma_result_description(result));
        return 0;
    }
    if (decoder.outputChannels != channels ||
            decoder.outputSampleRate != rate) {
        fprintf(stderr, "ma: decoder ignored requested format\n");
        ma_decoder_uninit(&decoder);
        return 0;
    }

    pcm = (unsigned char *)malloc((size_t)cap * frame_bytes);
    if (pcm == NULL) {
        fprintf(stderr, "ma: out of memory\n");
        ma_decoder_uninit(&decoder);
        return 0;
    }
    for (;;) {
        ma_uint64 got = 0;

        if (total + 4096u > cap) {
            unsigned char *bigger;
            ma_uint64 next = cap * 2u;

            if (next > ((ma_uint64)(~(size_t)0)) / frame_bytes) {
                fprintf(stderr, "ma: PCM too large\n");
                free(pcm);
                ma_decoder_uninit(&decoder);
                return 0;
            }
            bigger = (unsigned char *)realloc(pcm, (size_t)next * frame_bytes);
            if (bigger == NULL) {
                fprintf(stderr, "ma: out of memory\n");
                free(pcm);
                ma_decoder_uninit(&decoder);
                return 0;
            }
            pcm = bigger;
            cap = next;
        }
        result = ma_decoder_read_pcm_frames(
            &decoder, pcm + (size_t)total * frame_bytes, 4096u, &got);
        if (result == MA_AT_END) {
            break;
        }
        if (result != MA_SUCCESS) {
            fprintf(stderr, "ma: decode failed: %s\n",
                    ma_result_description(result));
            free(pcm);
            ma_decoder_uninit(&decoder);
            return 0;
        }
        if (got == 0) {
            break;
        }
        total += got;
    }
    ma_decoder_uninit(&decoder);
    if (total == 0) {
        fprintf(stderr, "ma: empty audio stream\n");
        free(pcm);
        return 0;
    }
    *pcm_out = pcm;
    *frames_out = total;
    return 1;
}

/* Probe a file's native rate/channels so `decode` preserves its format. */
static int native_format(const char *input, unsigned *rate, unsigned *channels)
{
    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_file(input, &config, &decoder);

    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma: cannot open %s: %s\n", input,
                ma_result_description(result));
        return 0;
    }
    *rate = decoder.outputSampleRate;
    *channels = decoder.outputChannels;
    ma_decoder_uninit(&decoder);
    return (*rate != 0 && (*channels == 1 || *channels == 2));
}

int main(int argc, char **argv)
{
    void *pcm = NULL;
    ma_uint64 frames = 0;
    unsigned rate = 0;
    unsigned channels = 0;
    const char *mode = (argc > 1) ? argv[1] : "";
    int preparing = (strcmp(mode, "prepare") == 0);
    int decoding = (strcmp(mode, "decode") == 0);
    int ok;

    if ((preparing && argc != 6) || (decoding && argc != 4) ||
            (!preparing && !decoding)) {
        fprintf(stderr, "usage: %s decode <in.wav> <out.pcm>\n"
                        "       %s prepare <input> <rate> <channels> <out.pcm>\n",
                argv[0], argv[0]);
        return 2;
    }

    if (preparing) {
        rate = (unsigned)strtoul(argv[3], NULL, 10);
        channels = (unsigned)strtoul(argv[4], NULL, 10);
    } else if (!native_format(argv[2], &rate, &channels)) {
        return 3;
    }

    ok = decode_all(argv[2], rate, channels, &pcm, &frames);
    if (!ok) {
        return 3;
    }
    if (!write_pcm(argv[argc - 1], pcm,
                   (size_t)frames * channels * 2u)) {
        free(pcm);
        return 4;
    }
    if (preparing) {
        printf("ma.prepare channels=%u rate=%u frames=%llu bytes=%llu\n",
               channels, rate, (unsigned long long)frames,
               (unsigned long long)frames * channels * 2u);
    } else {
        printf("ma.decoder channels=%u rate=%u frames=%llu bytes=%llu\n",
               channels, rate, (unsigned long long)frames,
               (unsigned long long)frames * channels * 2u);
    }
    free(pcm);
    return 0;
}
