/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_audio.c
 * @brief Unit tests for the pure audio helpers (components/audio/audio.c).
 *
 * Covers audio_output_parse(), audio_output_name(), audio_route_resolve(),
 * audio_amp_should_enable() and audio_wav_params_ok() — all side-effect
 * free. The BSP jack read, amp toggle, codec path, and playback task stay
 * hardware-verified (s12).
 */

#include "unity.h"
#include "audio.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void test_audio_output_parse(void)
{
    audio_output_mode_t mode = AUDIO_OUTPUT_SPEAKER;

    TEST_ASSERT_TRUE(audio_output_parse("auto", &mode));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_AUTO, (int)mode);
    TEST_ASSERT_TRUE(audio_output_parse("SPEAKER", &mode));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_SPEAKER, (int)mode);
    TEST_ASSERT_TRUE(audio_output_parse("Headphones", &mode));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_HEADPHONES, (int)mode);
    TEST_ASSERT_TRUE(audio_output_parse("headphone", &mode));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_HEADPHONES, (int)mode);

    /* Rejects: empty, unknown, NULL. The out value is untouched on reject. */
    mode = AUDIO_OUTPUT_SPEAKER;
    TEST_ASSERT_FALSE(audio_output_parse("", &mode));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_SPEAKER, (int)mode);
    TEST_ASSERT_FALSE(audio_output_parse("mute", &mode));
    TEST_ASSERT_FALSE(audio_output_parse(NULL, &mode));
    TEST_ASSERT_FALSE(audio_output_parse("auto", NULL));
}

void test_audio_output_name(void)
{
    TEST_ASSERT_EQUAL_STRING("auto", audio_output_name(AUDIO_OUTPUT_AUTO));
    TEST_ASSERT_EQUAL_STRING("speaker", audio_output_name(AUDIO_OUTPUT_SPEAKER));
    TEST_ASSERT_EQUAL_STRING("headphones", audio_output_name(AUDIO_OUTPUT_HEADPHONES));
    /* Out-of-range falls back to auto (never NULL). */
    TEST_ASSERT_EQUAL_STRING("auto", audio_output_name((audio_output_mode_t)99));
}

void test_audio_route_resolve(void)
{
    /* Forced modes win regardless of jack state or board support. */
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_SPEAKER,
        (int)audio_route_resolve(AUDIO_OUTPUT_SPEAKER, true, true));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_SPEAKER,
        (int)audio_route_resolve(AUDIO_OUTPUT_SPEAKER, false, false));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_HEADPHONES,
        (int)audio_route_resolve(AUDIO_OUTPUT_HEADPHONES, false, true));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_HEADPHONES,
        (int)audio_route_resolve(AUDIO_OUTPUT_HEADPHONES, false, false));

    /* AUTO follows the jack when the board reports one... */
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_HEADPHONES,
        (int)audio_route_resolve(AUDIO_OUTPUT_AUTO, true, true));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_SPEAKER,
        (int)audio_route_resolve(AUDIO_OUTPUT_AUTO, false, true));

    /* ...and stays on the speaker when the board has no jack. */
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_SPEAKER,
        (int)audio_route_resolve(AUDIO_OUTPUT_AUTO, false, false));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUTPUT_SPEAKER,
        (int)audio_route_resolve(AUDIO_OUTPUT_AUTO, true, false));
}

void test_audio_amp_should_enable(void)
{
    /* Idle never powers the amp, whatever the mode or jack claims. This is
     * the v1.3.0 white-noise regression gate: boot/volume/mode-switch paths
     * must leave the speaker silent. */
    TEST_ASSERT_FALSE(audio_amp_should_enable(AUDIO_OUTPUT_SPEAKER, false, true, false));
    TEST_ASSERT_FALSE(audio_amp_should_enable(AUDIO_OUTPUT_SPEAKER, false, false, false));
    TEST_ASSERT_FALSE(audio_amp_should_enable(AUDIO_OUTPUT_AUTO, false, true, false));
    TEST_ASSERT_FALSE(audio_amp_should_enable(AUDIO_OUTPUT_HEADPHONES, false, true, false));

    /* Playing follows the resolved route: speaker-routed windows drive the
     * amp, headphone-routed windows do not. */
    TEST_ASSERT_TRUE(audio_amp_should_enable(AUDIO_OUTPUT_SPEAKER, false, true, true));
    TEST_ASSERT_TRUE(audio_amp_should_enable(AUDIO_OUTPUT_SPEAKER, false, false, true));
    TEST_ASSERT_TRUE(audio_amp_should_enable(AUDIO_OUTPUT_AUTO, false, true, true));
    TEST_ASSERT_TRUE(audio_amp_should_enable(AUDIO_OUTPUT_AUTO, false, false, true));
    TEST_ASSERT_FALSE(audio_amp_should_enable(AUDIO_OUTPUT_HEADPHONES, false, true, true));
    TEST_ASSERT_FALSE(audio_amp_should_enable(AUDIO_OUTPUT_HEADPHONES, false, false, true));
    TEST_ASSERT_FALSE(audio_amp_should_enable(AUDIO_OUTPUT_AUTO, true, true, true));
}

void test_audio_mic_stats(void)
{
    int64_t sum_sq = 0;
    int peak = 0;
    int rms = -1;
    int out_peak = -1;
    /* DC block: alternating +/-1000 has RMS 1000 and peak 1000. */
    int16_t block[4] = { 1000, -1000, 1000, -1000 };

    audio_mic_accumulate(block, 4, &sum_sq, &peak);
    TEST_ASSERT_EQUAL_INT(1000, peak);
    audio_mic_stats_finish(sum_sq, 4, peak, &rms, &out_peak);
    TEST_ASSERT_EQUAL_INT(1000, rms);
    TEST_ASSERT_EQUAL_INT(1000, out_peak);

    /* Silence accumulates to zero without touching the peak. */
    sum_sq = 0;
    peak = 0;
    {
        int16_t silence[8] = { 0 };
        audio_mic_accumulate(silence, 8, &sum_sq, &peak);
    }
    audio_mic_stats_finish(sum_sq, 8, peak, &rms, &out_peak);
    TEST_ASSERT_EQUAL_INT(0, rms);
    TEST_ASSERT_EQUAL_INT(0, out_peak);

    /* Empty input is defined (zero), NULL-safe. */
    audio_mic_stats_finish(0, 0, 0, &rms, &out_peak);
    TEST_ASSERT_EQUAL_INT(0, rms);
    audio_mic_accumulate(NULL, 4, &sum_sq, &peak);
    audio_mic_accumulate(block, 4, NULL, &peak);
    audio_mic_stats_finish(sum_sq, 4, peak, NULL, NULL);
}

void test_audio_mic_goertzel(void)
{
    /* A pure 440 Hz sine at 22050 Hz over 0.5 s: the detector must report
     * near the sine's RMS (~40% full scale here) and reject silence. */
    double state[3] = { 0.0, 0.0, 0.0 };
    int16_t sine[11025];
    int i;
    int tone;

    for (i = 0; i < 11025; i++) {
        sine[i] = (int16_t)(13000.0 *
            sin(2.0 * 3.14159265358979323846 * 440.0 * (double)i / 22050.0));
    }
    audio_mic_goertzel_feed(sine, 11025, 22050, 440, state);
    tone = audio_mic_goertzel_finish(state, 11025);
    TEST_ASSERT_INT_WITHIN(1500, 9192, tone);

    /* Off-frequency energy must not leak in: the same buffer read at 880 Hz
     * stays small, and silence reads zero. */
    {
        double off[3] = { 0.0, 0.0, 0.0 };
        double quiet[3] = { 0.0, 0.0, 0.0 };
        int16_t silence[11025] = { 0 };

        audio_mic_goertzel_feed(sine, 11025, 22050, 880, off);
        TEST_ASSERT_LESS_THAN(2000, audio_mic_goertzel_finish(off, 11025));
        audio_mic_goertzel_feed(silence, 11025, 22050, 440, quiet);
        TEST_ASSERT_EQUAL_INT(0, audio_mic_goertzel_finish(quiet, 11025));
    }

    /* NULL-safe, empty-safe. */
    audio_mic_goertzel_feed(NULL, 10, 22050, 440, state);
    TEST_ASSERT_EQUAL_INT(0, audio_mic_goertzel_finish(NULL, 10));
    TEST_ASSERT_EQUAL_INT(0, audio_mic_goertzel_finish(state, 0));
}

void test_audio_wav_params_ok(void)
{
    /* The two accepted shapes: 22050/44100 Hz, mono/stereo, 16-bit PCM. */
    TEST_ASSERT_TRUE(audio_wav_params_ok(1, 1, 22050, 16, 100));
    TEST_ASSERT_TRUE(audio_wav_params_ok(1, 2, 22050, 16, 100));
    TEST_ASSERT_TRUE(audio_wav_params_ok(1, 1, 44100, 16, 100));
    TEST_ASSERT_TRUE(audio_wav_params_ok(1, 2, 44100, 16, 100));

    /* Everything the player rejects. */
    TEST_ASSERT_FALSE(audio_wav_params_ok(3, 1, 22050, 16, 100));   /* non-PCM */
    TEST_ASSERT_FALSE(audio_wav_params_ok(1, 1, 22050, 8, 100));    /* 8-bit */
    TEST_ASSERT_FALSE(audio_wav_params_ok(1, 1, 22050, 24, 100));   /* 24-bit */
    TEST_ASSERT_FALSE(audio_wav_params_ok(1, 0, 22050, 16, 100));   /* no channels */
    TEST_ASSERT_FALSE(audio_wav_params_ok(1, 3, 22050, 16, 100));   /* >stereo */
    TEST_ASSERT_FALSE(audio_wav_params_ok(1, 1, 8000, 16, 100));    /* rate */
    TEST_ASSERT_FALSE(audio_wav_params_ok(1, 1, 48000, 16, 100));   /* rate */
    TEST_ASSERT_FALSE(audio_wav_params_ok(1, 1, 22050, 16, 0));     /* empty */
}
