/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file audio_engine.h
 * @module audio
 * @layer leaf
 * @domain media
 * @provides Bounded mixer, ADSR, sample-accurate sequencer, SFX bus, ducking, offline PCM render
 * @status stable
 * @test test/main/test_audio.c, tools/host_audio_gate.py
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Pure, hardware-free audio engine feeding the ONE existing codec path.
 *
 * This module never opens a codec, starts a task or reads a pin: it turns a
 * bounded set of channels into deterministic stereo 16-bit PCM. On the board
 * `audio.c` submits those frames to its existing playback task / speaker
 * handle; on the host the same translation unit is compiled by
 * `tools/host_audio_gate.py` and rendered into PCM for assertions. Mixer ideas
 * are re-expressed here in original C (BMDS mix idea, pin ef05be9f; no source
 * copied - see tools/LICENSE-BMDS-MIT.txt).
 *
 * All arithmetic is integer fixed-point (Q16 gains, constant PCM sine table)
 * so a render is bit-identical on the host and the target for the same input.
 */

#ifndef P4MINISHELL_AUDIO_ENGINE_H
#define P4MINISHELL_AUDIO_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Hard channel bound: a full mixer refuses the next add (no growth). */
#define AUDIO_ENGINE_MAX_CHANNELS 8u

/** Default rate; the board passes P4_CONFIG_TONE_SAMPLE_RATE_HZ explicitly. */
#define AUDIO_ENGINE_RATE_DEFAULT_HZ 48000u

/** Q16 unit gain (1.0). Gains are clamped to 0..AUDIO_ENGINE_ONE_Q16. */
#define AUDIO_ENGINE_ONE_Q16 65536

/** Peak amplitude of the built-in sine table, leaving mixing headroom. */
#define AUDIO_ENGINE_SINE_AMPLITUDE 16000

/** Which summed bus a channel feeds. SFX drives ducking of the music bus. */
typedef enum {
    AUDIO_ENGINE_BUS_MUSIC = 0,
    AUDIO_ENGINE_BUS_SFX = 1,
} audio_engine_bus_t;

/** Linear ADSR, in milliseconds / percent. Pure data, unit-tested. */
typedef struct {
    uint32_t attack_ms;
    uint32_t decay_ms;
    int sustain_pct;      /* 0..100 */
    uint32_t release_ms;
} audio_engine_env_t;

/** One decoded sequencer event, in absolute samples (sample-accurate). */
typedef struct {
    uint32_t start_sample;
    uint32_t length_samples;
    int freq_hz;
    int gain_pct;
} audio_engine_seq_event_t;

/** A mixer channel. PCM pointers are borrowed: the caller owns their memory. */
typedef struct {
    uint32_t start_sample;
    uint32_t length_samples;
    int freq_hz;              /* >0: sine table source; 0: PCM source */
    const int16_t *pcm;       /* mono frames, borrowed; NULL for sine */
    uint32_t pcm_frames;      /* frames available before repetition */
    int32_t gain_q16;
    audio_engine_bus_t bus;
    audio_engine_env_t env;
    bool in_use;
} audio_engine_channel_t;

/** Engine state. Zero-init via audio_engine_init(); no hidden globals. */
typedef struct {
    uint32_t sample_rate;
    int32_t master_gain_q16;
    int32_t music_gain_q16;
    int32_t sfx_gain_q16;
    int32_t duck_gain_q16;      /* music multiplier while an SFX is active */
    uint32_t duck_attack_ms;
    uint32_t duck_release_ms;
    audio_engine_env_t default_env;
    audio_engine_channel_t channel[AUDIO_ENGINE_MAX_CHANNELS];
} audio_engine_t;

/** Reset to a flat envelope, unity bus/master gains and no active channels. */
void audio_engine_init(audio_engine_t *engine, uint32_t sample_rate);

/** Drop every channel (borrowed PCM pointers are untouched). */
void audio_engine_reset(audio_engine_t *engine);

/** Percent 0..100 -> Q16, clamped. Pure. */
int32_t audio_engine_gain_q16(int percent);

/** Parse "attack,decay,sustain,release" or flat/pluck/pad. Pure, unit-tested. */
bool audio_engine_env_parse(const char *text, audio_engine_env_t *out);

/** Canonical envelope name for status lines. Pure. */
const char *audio_engine_env_preset_name(const audio_engine_env_t *env);

/** Parse "<freq>:<dur_ms>@<start_ms>[:<gain_pct>]" into absolute samples. Pure. */
bool audio_engine_seq_parse(const char *text, uint32_t sample_rate,
                            audio_engine_seq_event_t *out);

/** Add a tone channel; returns its stable index or -1 when the mixer is full. */
int audio_engine_add_tone(audio_engine_t *engine, uint32_t start_sample,
                          uint32_t length_samples, int freq_hz, int gain_pct,
                          audio_engine_bus_t bus, const audio_engine_env_t *env);

/**
 * Add a borrowed mono PCM channel, repeated to fill @p repeat times its length.
 * Returns the stable index or -1 when full / the source is unusable.
 */
int audio_engine_add_pcm(audio_engine_t *engine, uint32_t start_sample,
                         const int16_t *pcm, uint32_t pcm_frames,
                         uint32_t repeat, int gain_pct, audio_engine_bus_t bus,
                         const audio_engine_env_t *env);

/** Add an already decoded sequencer event; returns index or -1 when full. */
int audio_engine_add_event(audio_engine_t *engine,
                           const audio_engine_seq_event_t *event,
                           audio_engine_bus_t bus,
                           const audio_engine_env_t *env);

/** Active channel count / full predicate (bounded by AUDIO_ENGINE_MAX_CHANNELS). */
size_t audio_engine_count(const audio_engine_t *engine);
bool audio_engine_full(const audio_engine_t *engine);

/** Sample index just past the last channel (including release and duck tail). */
uint32_t audio_engine_length_samples(const audio_engine_t *engine);

/**
 * Deterministic offline render: write @p frames stereo frames starting at the
 * ABSOLUTE sample @p start_sample. No call-order state: rendering [0,N) in one
 * call or in chunks produces identical PCM. Clips saturating to int16.
 */
void audio_engine_render(audio_engine_t *engine, int16_t *stereo_out,
                         size_t frames, uint32_t start_sample);

/** Fold rendered stereo PCM into running meter stats (pure, NULL-safe). */
void audio_engine_meter_accumulate(const int16_t *stereo, size_t frames,
                                   int64_t *sum_sq_out, int *peak_out);

/** Finish a meter reading: RMS in 16-bit LSB units, rounded to nearest. */
int audio_engine_meter_finish(int64_t sum_sq, uint64_t sample_count);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_AUDIO_ENGINE_H */
