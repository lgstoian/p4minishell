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

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_AUDIO_H */
