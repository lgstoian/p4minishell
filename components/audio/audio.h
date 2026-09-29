/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file audio.h
 * @brief Codec audio playback for P4MiniShell (`beep` / `tone` / `wavplay`).
 *
 * Owns the speaker codec path (mono 16-bit at 22050 Hz), the speaker volume,
 * and a small background playback task so shell commands never block on
 * audio. The command layer (`components/command/`) parses `beep` / `tone` /
 * `wavplay` / `audio` / `volume` and calls into this module; all audio logic
 * lives here.
 */

#ifndef P4MINISHELL_AUDIO_H
#define P4MINISHELL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ensure the audio subsystem is ready (codec + playback task).
 * Called lazily by the other entry points; idempotent.
 */
esp_err_t audio_init(void);

/**
 * Play a sine tone in the background.
 *
 * @param freq_hz       Frequency in Hz.
 * @param duration_ms   Duration in milliseconds.
 * @return true when playback started, false when audio is busy.
 */
bool audio_play_tone(int freq_hz, uint32_t duration_ms);

/**
 * Stream a 16-bit PCM WAV (mono or stereo, 22050 or 44100 Hz) from the SD
 * card in the background. The path must be an absolute resolved VFS path.
 *
 * @return true when playback started, false when audio is busy.
 */
bool audio_play_wav(const char *resolved_path);

/** Cut the current background playback short. */
void audio_stop(void);

/** Report whether a sound is currently playing. */
bool audio_busy(void);

/**
 * Set the speaker volume (0..100). Initializes the codec on first use.
 * All playback rides on this level.
 */
esp_err_t audio_set_volume(int percent);

/** Current speaker volume percentage (0..100). */
int audio_get_volume(void);

/**
 * Playback output routing. AUTO (default) plays the speaker unless headphones
 * are detected; SPEAKER forces the amp on; HEADPHONES forces it off. The route
 * is evaluated at play/volume/status time (no background poller).
 */
typedef enum {
    AUDIO_OUTPUT_AUTO = 0,
    AUDIO_OUTPUT_SPEAKER,
    AUDIO_OUTPUT_HEADPHONES,
} audio_output_mode_t;

/** Set the output mode (persists via `config AUDIO_OUTPUT=`). */
esp_err_t audio_set_output_mode(audio_output_mode_t mode);

/** Current output mode (as set, not the resolved route). */
audio_output_mode_t audio_get_output_mode(void);

/**
 * Resolve the effective route: forced modes win; AUTO follows the headphone
 * jack when the board has one, otherwise the speaker. Pure, unit-tested.
 */
audio_output_mode_t audio_route_resolve(audio_output_mode_t mode,
                                        bool hp_inserted, bool hp_supported);

/** Effective route right now (mode + live jack state). */
audio_output_mode_t audio_effective_route(void);

/**
 * Read the headphone jack state. @p inserted_out gets the plug state;
 * @p supported_out is false on boards without a jack (route always speaker).
 * Either out pointer may be NULL.
 */
void audio_headphone_state(bool *inserted_out, bool *supported_out);

/**
 * Parse an output-mode word ("auto"/"speaker"/"headphones",
 * case-insensitive). Pure, unit-tested.
 */
bool audio_output_parse(const char *text, audio_output_mode_t *out);

/** Mode name for status lines ("auto"/"speaker"/"headphones"). Pure. */
const char *audio_output_name(audio_output_mode_t mode);

/**
 * WAV acceptance predicate (16-bit PCM mono/stereo at 22050/44100 Hz with a
 * non-empty data chunk). Mirrors the `wavplay` checks; pure, unit-tested.
 */
bool audio_wav_params_ok(uint16_t audio_format, uint16_t channels,
                         uint32_t sample_rate, uint16_t bits,
                         uint32_t data_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_AUDIO_H */
