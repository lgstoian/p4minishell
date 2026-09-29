/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_audio.c
 * @brief Unit tests for the pure audio helpers (components/audio/audio.c).
 *
 * Covers audio_output_parse(), audio_output_name(), audio_route_resolve()
 * and audio_wav_params_ok() — all side-effect free. The BSP jack read, amp
 * toggle, codec path, and playback task stay hardware-verified (s12).
 */

#include "unity.h"
#include "audio.h"
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
