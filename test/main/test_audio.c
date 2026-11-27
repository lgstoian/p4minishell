/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_audio.c
 * @brief Unit tests for the pure audio helpers (components/audio/audio.c).
 *
 * Covers audio_output_parse(), audio_output_name(), audio_route_resolve(),
 * audio_amp_should_enable(), audio_wav_params_ok(), audio_mic_accumulate(),
 * audio_mic_stats_finish(), and the Goertzel detector — all side-effect
 * free. The BSP jack read, amp toggle, codec path, and playback task stay
 * hardware-verified (s12).
 */

#include "unity.h"
#include "audio.h"
#include "audio_engine.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * The engine regressions are static helpers invoked from two already
 * registered Unity cases: test_main.c is owned by the parent and adding new
 * test_* symbols there would leave an unregistered suite (green_guard).
 */
static void audio_engine_regression_checks(void);
static void audio_engine_bound_checks(void);

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

    audio_engine_regression_checks();
}

void test_audio_mic_goertzel(void)
{
    /* A pure 440 Hz sine at 22050 Hz: the detector must report near the
     * sine's RMS (~40% full scale here) and reject silence. N=2205 is an
     * exact 44 cycles, so 440 Hz (and its 880 Hz harmonic check) fall on
     * integer DFT bins. Buffers are heap-allocated: two 0.5 s stack buffers
     * overflowed the Unity task stack (stack protection fault, reset loop).
     */
#define GOERTZEL_N 2205
    double state[3] = { 0.0, 0.0, 0.0 };
    int16_t *sine = malloc(GOERTZEL_N * sizeof(int16_t));
    int16_t *silence = malloc(GOERTZEL_N * sizeof(int16_t));
    int i;
    int tone;

    TEST_ASSERT_NOT_NULL(sine);
    TEST_ASSERT_NOT_NULL(silence);
    for (i = 0; i < GOERTZEL_N; i++) {
        sine[i] = (int16_t)(13000.0 *
            sin(2.0 * 3.14159265358979323846 * 440.0 * (double)i / 22050.0));
        silence[i] = 0;
    }
    audio_mic_goertzel_feed(sine, GOERTZEL_N, 22050, 440, state);
    tone = audio_mic_goertzel_finish(state, GOERTZEL_N);
    TEST_ASSERT_INT_WITHIN(1500, 9192, tone);

    /* Off-frequency energy must not leak in: the same buffer read at 880 Hz
     * stays small, and silence reads zero. */
    {
        double off[3] = { 0.0, 0.0, 0.0 };
        double quiet[3] = { 0.0, 0.0, 0.0 };

        audio_mic_goertzel_feed(sine, GOERTZEL_N, 22050, 880, off);
        TEST_ASSERT_LESS_THAN(2000, audio_mic_goertzel_finish(off, GOERTZEL_N));
        audio_mic_goertzel_feed(silence, GOERTZEL_N, 22050, 440, quiet);
        TEST_ASSERT_EQUAL_INT(0, audio_mic_goertzel_finish(quiet, GOERTZEL_N));
    }
    free(sine);
    free(silence);
#undef GOERTZEL_N

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

    audio_engine_bound_checks();
}

/* ---- audio engine (pure mixer) regressions ---------------------------- */

static void audio_engine_regression_checks(void)
{
    audio_engine_t engine;
    audio_engine_env_t env;
    audio_engine_seq_event_t first;
    audio_engine_seq_event_t second;
    int16_t out[192];
    const int16_t music[4] = { 1000, 2000, 3000, 4000 };
    const int16_t sfx[4] = { 500, 500, 500, 500 };
    const int16_t loud[1] = { 30000 };
    const int16_t unit[1] = { 10000 };
    const int16_t steady[8] = { 5000, 5000, 5000, 5000,
                                5000, 5000, 5000, 5000 };
    int16_t whole[16];
    int16_t chunks[16];
    int64_t sum_sq = 0;
    int peak = 0;
    int i;

    /* Envelope parsing: presets, explicit values and rejects. */
    TEST_ASSERT_TRUE(audio_engine_env_parse("flat", &env));
    TEST_ASSERT_EQUAL_UINT32(0, env.attack_ms);
    TEST_ASSERT_EQUAL_INT(100, env.sustain_pct);
    TEST_ASSERT_TRUE(audio_engine_env_parse("5,20,40,40", &env));
    TEST_ASSERT_EQUAL_UINT32(5, env.attack_ms);
    TEST_ASSERT_EQUAL_UINT32(20, env.decay_ms);
    TEST_ASSERT_EQUAL_INT(40, env.sustain_pct);
    TEST_ASSERT_EQUAL_UINT32(40, env.release_ms);
    TEST_ASSERT_FALSE(audio_engine_env_parse("1,2,3", &env));
    TEST_ASSERT_FALSE(audio_engine_env_parse("1,2,101,3", &env));

    /* Sample-accurate sequencing: 250 ms at 1 kHz is exactly 250 samples. */
    TEST_ASSERT_TRUE(audio_engine_seq_parse("440:100@250:80", 1000, &first));
    TEST_ASSERT_EQUAL_UINT32(250, first.start_sample);
    TEST_ASSERT_EQUAL_UINT32(100, first.length_samples);
    TEST_ASSERT_EQUAL_INT(440, first.freq_hz);
    TEST_ASSERT_EQUAL_INT(80, first.gain_pct);
    TEST_ASSERT_TRUE(audio_engine_seq_parse("660:50@350", 1000, &second));
    TEST_ASSERT_EQUAL_UINT32(350, second.start_sample);
    TEST_ASSERT_TRUE(second.start_sample > first.start_sample);
    TEST_ASSERT_FALSE(audio_engine_seq_parse("440:100@250:junk", 1000, &first));

    /* Exact two-bus mixing: no hidden gain, stereo duplication. */
    audio_engine_init(&engine, 1000);
    TEST_ASSERT_EQUAL_INT(0, audio_engine_add_pcm(&engine, 0, music, 4, 1,
                                                   100, AUDIO_ENGINE_BUS_MUSIC, NULL));
    TEST_ASSERT_EQUAL_INT(1, audio_engine_add_pcm(&engine, 0, sfx, 4, 1,
                                                   100, AUDIO_ENGINE_BUS_SFX, NULL));
    audio_engine_render(&engine, out, 4, 0);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT16((int16_t)(music[i] + sfx[i]), out[2 * i]);
        TEST_ASSERT_EQUAL_INT16(out[2 * i], out[2 * i + 1]);
    }

    /* Clipping saturates: an unwrapped sum would read -6536, not +32767. */
    audio_engine_init(&engine, 1000);
    TEST_ASSERT_EQUAL_INT(0, audio_engine_add_pcm(&engine, 0, loud, 1, 1,
                                                   100, AUDIO_ENGINE_BUS_MUSIC, NULL));
    TEST_ASSERT_EQUAL_INT(1, audio_engine_add_pcm(&engine, 0, loud, 1, 1,
                                                   100, AUDIO_ENGINE_BUS_SFX, NULL));
    audio_engine_render(&engine, out, 1, 0);
    TEST_ASSERT_EQUAL_INT16(32767, out[0]);

    /* ADSR: attack ramp, decay to sustain, release back to silence. */
    audio_engine_init(&engine, 1000);
    TEST_ASSERT_TRUE(audio_engine_env_parse("5,20,40,40", &env));
    TEST_ASSERT_EQUAL_INT(0, audio_engine_add_pcm(&engine, 0, unit, 1, 40,
                                                   100, AUDIO_ENGINE_BUS_MUSIC, &env));
    audio_engine_render(&engine, out, 85, 0);
    TEST_ASSERT_INT_WITHIN(1, 0, out[0]);       /* attack starts at zero */
    TEST_ASSERT_INT_WITHIN(1, 4000, out[4]);    /* mid-attack (frame 2) */
    TEST_ASSERT_EQUAL_INT16(10000, out[10]);    /* attack complete (frame 5) */
    TEST_ASSERT_INT_WITHIN(1, 8500, out[20]);   /* into decay (frame 10) */
    TEST_ASSERT_INT_WITHIN(1, 4000, out[60]);   /* sustain (frame 30) */
    TEST_ASSERT_INT_WITHIN(1, 4000, out[80]);   /* release begins (frame 40) */
    TEST_ASSERT_EQUAL_INT16(0, out[160]);       /* beyond the release tail */

    /* Ducking: SFX attenuates the music bus only inside its window. */
    audio_engine_init(&engine, 1000);
    engine.duck_gain_q16 = audio_engine_gain_q16(25);
    engine.duck_attack_ms = 0;
    engine.duck_release_ms = 0;
    TEST_ASSERT_EQUAL_INT(0, audio_engine_add_pcm(&engine, 0, unit, 1, 10,
                                                   100, AUDIO_ENGINE_BUS_MUSIC, NULL));
    TEST_ASSERT_EQUAL_INT(1, audio_engine_add_pcm(&engine, 2, unit, 1, 2,
                                                   100, AUDIO_ENGINE_BUS_SFX, NULL));
    audio_engine_render(&engine, out, 6, 0);
    TEST_ASSERT_EQUAL_INT16(10000, out[0]);
    TEST_ASSERT_EQUAL_INT16(12500, out[4]);  /* 2500 ducked music + 10000 sfx */
    TEST_ASSERT_EQUAL_INT16(10000, out[8]);

    /* Absolute-cursor chunking equals one-shot offline render. */
    audio_engine_init(&engine, 1000);
    TEST_ASSERT_EQUAL_INT(0, audio_engine_add_pcm(&engine, 0, steady, 8, 1,
                                                   100, AUDIO_ENGINE_BUS_MUSIC, NULL));
    audio_engine_render(&engine, whole, 8, 0);
    audio_engine_render(&engine, chunks, 3, 0);
    audio_engine_render(&engine, chunks + 6, 5, 3);
    TEST_ASSERT_EQUAL_MEMORY(whole, chunks, 16 * sizeof(int16_t));
    for (i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT16(5000, whole[2 * i]);
    }

    /* Meter: peak and RMS of a DC-free alternating block. */
    {
        const int16_t alternating[8] = { 1000, -1000, 1000, -1000,
                                         1000, -1000, 1000, -1000 };

        audio_engine_meter_accumulate(alternating, 4, &sum_sq, &peak);
    }
    TEST_ASSERT_EQUAL_INT(1000, peak);
    TEST_ASSERT_EQUAL_INT(1000, audio_engine_meter_finish(sum_sq, 8));
    TEST_ASSERT_EQUAL_INT(0, audio_engine_meter_finish(0, 0));
}

static void audio_engine_bound_checks(void)
{
    audio_engine_t engine;
    int index;
    int i;

    audio_engine_init(&engine, 48000);
    for (i = 0; i < (int)AUDIO_ENGINE_MAX_CHANNELS; i++) {
        index = audio_engine_add_tone(&engine, (uint32_t)i * 480u, 480u,
                                      440, 100, AUDIO_ENGINE_BUS_MUSIC, NULL);
        TEST_ASSERT_EQUAL_INT(i, index);
    }
    TEST_ASSERT_TRUE(audio_engine_full(&engine));
    TEST_ASSERT_EQUAL_INT(AUDIO_ENGINE_MAX_CHANNELS,
                          (int)audio_engine_count(&engine));
    /* The next add is refused: the channel table never grows. */
    TEST_ASSERT_EQUAL_INT(-1, audio_engine_add_tone(&engine, 0, 480, 440, 100,
                                                    AUDIO_ENGINE_BUS_MUSIC, NULL));
    audio_engine_reset(&engine);
    TEST_ASSERT_EQUAL_UINT32(0, audio_engine_count(&engine));
    TEST_ASSERT_FALSE(audio_engine_full(&engine));
}
