/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file audio.h
 * @brief Codec audio playback for P4MiniShell (`beep` / `tone` / `wavplay`).
 *
 * Owns the speaker codec path (stereo 16-bit; tone rate configurable, WAVs
 * at file rate), the speaker volume,
 * the output routing, and a small background playback task so shell commands
 * never block on audio. The speaker amp is gated to the playback window only
 * (mute-at-boot invariant): idle output is always silent. The command layer
 * (`components/command/`) parses `beep` / `tone` / `wavplay` / `audio` /
 * `volume` and calls into this module; all audio logic lives here.
 */

#ifndef P4MINISHELL_AUDIO_H
#define P4MINISHELL_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
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

/** Report whether the playback task has opened the codec and streams. */
bool audio_playback_started(void);

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
 * Amp-enable predicate: the speaker amp may only be on while a playback
 * stream holds the codec open. Idle (or headphone-routed) output must leave
 * the amp off, or the powered-but-clockless DAC idles as white noise through
 * the speaker. Pure, unit-tested.
 */
bool audio_amp_should_enable(audio_output_mode_t mode, bool hp_inserted,
                             bool hp_supported, bool playing);

/**
 * Fold one PCM chunk into running microphone statistics. @p sum_sq_out
 * accumulates the sum of squared samples and @p peak_out tracks the largest
 * absolute sample; both must be zero-initialized by the caller. Pure,
 * unit-tested.
 */
void audio_mic_accumulate(const int16_t *samples, size_t count,
                          int64_t *sum_sq_out, int *peak_out);

/**
 * Derive RMS/peak levels from accumulated microphone statistics. Pure,
 * unit-tested.
 */
void audio_mic_stats_finish(int64_t sum_sq, uint64_t sample_count,
                            int peak, int *rms_out, int *peak_out);

/**
 * Fold one PCM chunk into a Goertzel tone-power accumulator for @p freq_hz
 * at @p sample_rate Hz. Call once per chunk with a running @p state (three
 * doubles, zero-initialized by the caller), then read the tone RMS with
 * audio_mic_goertzel_finish(). Pure, unit-tested.
 */
void audio_mic_goertzel_feed(const int16_t *samples, size_t count,
                             uint32_t sample_rate, uint32_t freq_hz,
                             double state[3]);

/**
 * Finish a Goertzel measurement over @p sample_count samples: tone RMS in
 * 16-bit LSB units. Pure, unit-tested.
 */
int audio_mic_goertzel_finish(const double state[3], uint64_t sample_count);

/**
 * Listen for a sine at @p freq_hz for @p duration_ms and report the in-band
 * tone RMS alongside the broadband total RMS (both 16-bit LSB units). May
 * run during a playback (same shared-rate reasoning as audio_mic_level).
 * Returns ESP_ERR_NOT_SUPPORTED with no mic input.
 */
esp_err_t audio_mic_hear(uint32_t freq_hz, uint32_t duration_ms,
                         int *tone_rms_out, int *total_rms_out);

/**
 * End-to-end acoustic audit: records the room floor, plays a sine at
 * @p freq_hz through the speaker path, records it with the Goertzel
 * detector, then stops and leaves the amp off. One command, device-side
 * sequencing — immune to host round-trip lag. Returns ESP_ERR_INVALID_STATE
 * when a playback is already active.
 */
esp_err_t audio_mic_audit(uint32_t freq_hz, uint32_t sample_ms,
                          int *tone_rms_out, int *total_rms_out,
                          int *room_rms_out, int *room_tone_out);

/**
 * Print the live speaker-codec register dump (Tab5 ES8388 banks) to the
 * transcript for hardware diagnostics. Returns ESP_ERR_NOT_SUPPORTED on
 * boards without a dumpable codec.
 */
esp_err_t audio_speaker_diag(void);

/**
 * Record the microphone for @p duration_ms and report the RMS/peak sample
 * levels (16-bit LSB units). May run during a playback: the mic shares the
 * speaker's rate, so the record hears the room plus the speaker output (the
 * audibility check). Returns ESP_ERR_NOT_SUPPORTED on boards without a
 * microphone input.
 */
esp_err_t audio_mic_level(uint32_t duration_ms, int *rms_out, int *peak_out);

/**
 * Hiss self-test: records the microphone with the speaker amp forced on and
 * again with it forced off (@p sample_ms each), then leaves the amp off.
 * A large amp-on excess proves the speaker path hisses while idle. Returns
 * ESP_ERR_NOT_SUPPORTED on boards without a microphone input.
 */
esp_err_t audio_hiss_selftest(uint32_t sample_ms, int *rms_on_out,
                              int *peak_on_out, int *rms_off_out,
                              int *peak_off_out);

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
