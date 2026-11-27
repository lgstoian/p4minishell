/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file audio_engine.c
 * @brief Pure bounded mixer / ADSR / sequencer / ducking / offline PCM render.
 *
 * No codec handle, no FreeRTOS call, no BSP read: the board side (`audio.c`)
 * drives the SAME speaker task and SAME amp gate for these frames as it does
 * for `beep`/`tone`/`wavplay`. This file is compiled natively by
 * `tools/host_audio_gate.py` so the shipped mixer math is asserted without
 * hardware. Mixer ideas are re-expressed in original C (BMDS mix idea, pin
 * ef05be9f; no source copied - see tools/LICENSE-BMDS-MIT.txt).
 */

#include "audio_engine.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/** 64-step sine table, amplitude 16000, generated once and stored as data. */
static const int16_t s_sine64[64] = {
      0,  1568,  3121,  4645,  6123,  7542,  8889, 10150,
  11314, 12368, 13304, 14111, 14782, 15311, 15693, 15923,
  16000, 15923, 15693, 15311, 14782, 14111, 13304, 12368,
  11314, 10150,  8889,  7542,  6123,  4645,  3121,  1568,
      0, -1568, -3121, -4645, -6123, -7542, -8889,-10150,
 -11314,-12368,-13304,-14111,-14782,-15311,-15693,-15923,
 -16000,-15923,-15693,-15311,-14782,-14111,-13304,-12368,
 -11314,-10150, -8889, -7542, -6123, -4645, -3121, -1568
};

static const audio_engine_env_t s_env_flat = { 0u, 0u, 100, 0u };
static const audio_engine_env_t s_env_pluck = { 5u, 20u, 40, 40u };
static const audio_engine_env_t s_env_pad = { 50u, 100u, 80, 200u };

static uint32_t ms_to_samples(uint32_t ms, uint32_t rate)
{
    return (uint32_t)(((uint64_t)ms * (uint64_t)rate) / 1000u);
}

static int32_t clamp_q16(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value > AUDIO_ENGINE_ONE_Q16) {
        return AUDIO_ENGINE_ONE_Q16;
    }
    return value;
}

/** Multiply by a Q16 gain with one deterministic truncation toward zero. */
static int32_t q16_mul(int64_t value, int32_t gain_q16)
{
    return (int32_t)(value * (int64_t)gain_q16 /
                     (int64_t)AUDIO_ENGINE_ONE_Q16);
}

/** Integer linear ramp; all arguments are bounded by one Q16 unit. */
static int32_t ramp_q16(int32_t from, int32_t to, uint32_t pos, uint32_t span)
{
    if (span == 0u) {
        return to;
    }
    if (pos >= span) {
        return to;
    }
    return (int32_t)((int64_t)from +
                     ((int64_t)(to - from) * (int64_t)pos) / (int64_t)span);
}

int32_t audio_engine_gain_q16(int percent)
{
    if (percent <= 0) {
        return 0;
    }
    if (percent >= 100) {
        return AUDIO_ENGINE_ONE_Q16;
    }
    return (int32_t)(((int64_t)percent * AUDIO_ENGINE_ONE_Q16) / 100);
}

void audio_engine_init(audio_engine_t *engine, uint32_t sample_rate)
{
    size_t i;

    if (engine == NULL) {
        return;
    }
    memset(engine, 0, sizeof(*engine));
    engine->sample_rate = (sample_rate == 0u) ?
                          AUDIO_ENGINE_RATE_DEFAULT_HZ : sample_rate;
    engine->master_gain_q16 = AUDIO_ENGINE_ONE_Q16;
    engine->music_gain_q16 = AUDIO_ENGINE_ONE_Q16;
    engine->sfx_gain_q16 = AUDIO_ENGINE_ONE_Q16;
    engine->duck_gain_q16 = AUDIO_ENGINE_ONE_Q16;
    engine->duck_attack_ms = 5u;
    engine->duck_release_ms = 200u;
    engine->default_env = s_env_flat;
    for (i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
        engine->channel[i].gain_q16 = AUDIO_ENGINE_ONE_Q16;
        engine->channel[i].env = s_env_flat;
    }
}

void audio_engine_reset(audio_engine_t *engine)
{
    size_t i;

    if (engine == NULL) {
        return;
    }
    for (i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
        memset(&engine->channel[i], 0, sizeof(engine->channel[i]));
        engine->channel[i].gain_q16 = AUDIO_ENGINE_ONE_Q16;
        engine->channel[i].env = engine->default_env;
    }
}

bool audio_engine_env_parse(const char *text, audio_engine_env_t *out)
{
    char buf[64];
    char *cursor;
    char *end;
    unsigned long fields[4];
    int i;
    audio_engine_env_t parsed;

    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcasecmp(text, "flat") == 0) {
        *out = s_env_flat;
        return true;
    }
    if (strcasecmp(text, "pluck") == 0) {
        *out = s_env_pluck;
        return true;
    }
    if (strcasecmp(text, "pad") == 0) {
        *out = s_env_pad;
        return true;
    }
    if (strlen(text) >= sizeof(buf)) {
        return false;
    }
    strcpy(buf, text);
    cursor = buf;
    for (i = 0; i < 4; i++) {
        fields[i] = strtoul(cursor, &end, 10);
        if (end == cursor) {
            return false;
        }
        cursor = end;
        if (i < 3) {
            if (*cursor != ',') {
                return false;
            }
            cursor++;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    if (fields[0] > 5000u || fields[1] > 5000u || fields[2] > 100u ||
        fields[3] > 5000u) {
        return false;
    }
    parsed.attack_ms = (uint32_t)fields[0];
    parsed.decay_ms = (uint32_t)fields[1];
    parsed.sustain_pct = (int)fields[2];
    parsed.release_ms = (uint32_t)fields[3];
    *out = parsed;
    return true;
}

const char *audio_engine_env_preset_name(const audio_engine_env_t *env)
{
    if (env == NULL) {
        return "flat";
    }
    if (env->attack_ms == s_env_flat.attack_ms &&
        env->decay_ms == s_env_flat.decay_ms &&
        env->sustain_pct == s_env_flat.sustain_pct &&
        env->release_ms == s_env_flat.release_ms) {
        return "flat";
    }
    if (env->attack_ms == s_env_pluck.attack_ms &&
        env->decay_ms == s_env_pluck.decay_ms &&
        env->sustain_pct == s_env_pluck.sustain_pct &&
        env->release_ms == s_env_pluck.release_ms) {
        return "pluck";
    }
    if (env->attack_ms == s_env_pad.attack_ms &&
        env->decay_ms == s_env_pad.decay_ms &&
        env->sustain_pct == s_env_pad.sustain_pct &&
        env->release_ms == s_env_pad.release_ms) {
        return "pad";
    }
    return "custom";
}

bool audio_engine_seq_parse(const char *text, uint32_t sample_rate,
                            audio_engine_seq_event_t *out)
{
    char buf[64];
    char *cursor;
    char *end;
    unsigned long freq;
    unsigned long duration_ms;
    unsigned long start_ms;
    unsigned long gain = 100u;

    if (text == NULL || out == NULL || sample_rate == 0u ||
        strlen(text) >= sizeof(buf)) {
        return false;
    }
    strcpy(buf, text);
    cursor = buf;
    freq = strtoul(cursor, &end, 10);
    if (end == cursor || *end != ':') {
        return false;
    }
    cursor = end + 1;
    duration_ms = strtoul(cursor, &end, 10);
    if (end == cursor || *end != '@') {
        return false;
    }
    cursor = end + 1;
    start_ms = strtoul(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }
    if (*end == ':') {
        cursor = end + 1;
        gain = strtoul(cursor, &end, 10);
        if (end == cursor) {
            return false;
        }
    }
    if (*end != '\0' || freq < 20u || freq > 20000u || duration_ms == 0u ||
        duration_ms > 5000u || start_ms > 60000u || gain > 100u) {
        return false;
    }
    out->start_sample = ms_to_samples((uint32_t)start_ms, sample_rate);
    out->length_samples = ms_to_samples((uint32_t)duration_ms, sample_rate);
    if (out->length_samples == 0u) {
        out->length_samples = 1u;
    }
    out->freq_hz = (int)freq;
    out->gain_pct = (int)gain;
    return true;
}

static int add_channel(audio_engine_t *engine, uint32_t start_sample,
                       uint32_t length_samples, int freq_hz,
                       const int16_t *pcm, uint32_t pcm_frames,
                       int gain_pct, audio_engine_bus_t bus,
                       const audio_engine_env_t *env)
{
    size_t i;

    if (engine == NULL || length_samples == 0u ||
        (freq_hz <= 0 && (pcm == NULL || pcm_frames == 0u))) {
        return -1;
    }
    for (i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
        audio_engine_channel_t *ch = &engine->channel[i];

        if (ch->in_use) {
            continue;
        }
        memset(ch, 0, sizeof(*ch));
        ch->in_use = true;
        ch->start_sample = start_sample;
        ch->length_samples = length_samples;
        ch->freq_hz = (freq_hz > 0) ? freq_hz : 0;
        ch->pcm = pcm;
        ch->pcm_frames = pcm_frames;
        ch->gain_q16 = audio_engine_gain_q16(gain_pct);
        ch->bus = (bus == AUDIO_ENGINE_BUS_SFX) ?
                  AUDIO_ENGINE_BUS_SFX : AUDIO_ENGINE_BUS_MUSIC;
        ch->env = (env != NULL) ? *env : engine->default_env;
        return (int)i;
    }
    return -1;
}

int audio_engine_add_tone(audio_engine_t *engine, uint32_t start_sample,
                          uint32_t length_samples, int freq_hz, int gain_pct,
                          audio_engine_bus_t bus, const audio_engine_env_t *env)
{
    return add_channel(engine, start_sample, length_samples, freq_hz, NULL, 0u,
                       gain_pct, bus, env);
}

int audio_engine_add_pcm(audio_engine_t *engine, uint32_t start_sample,
                         const int16_t *pcm, uint32_t pcm_frames,
                         uint32_t repeat, int gain_pct, audio_engine_bus_t bus,
                         const audio_engine_env_t *env)
{
    uint64_t length;

    if (repeat == 0u) {
        return -1;
    }
    length = (uint64_t)pcm_frames * (uint64_t)repeat;
    if (length == 0u || length > UINT32_MAX) {
        return -1;
    }
    return add_channel(engine, start_sample, (uint32_t)length, 0, pcm,
                       pcm_frames, gain_pct, bus, env);
}

int audio_engine_add_event(audio_engine_t *engine,
                           const audio_engine_seq_event_t *event,
                           audio_engine_bus_t bus,
                           const audio_engine_env_t *env)
{
    if (event == NULL) {
        return -1;
    }
    return audio_engine_add_tone(engine, event->start_sample,
                                 event->length_samples, event->freq_hz,
                                 event->gain_pct, bus, env);
}

size_t audio_engine_count(const audio_engine_t *engine)
{
    size_t i;
    size_t count = 0u;

    if (engine == NULL) {
        return 0u;
    }
    for (i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
        if (engine->channel[i].in_use) {
            count++;
        }
    }
    return count;
}

bool audio_engine_full(const audio_engine_t *engine)
{
    return audio_engine_count(engine) >= AUDIO_ENGINE_MAX_CHANNELS;
}

uint32_t audio_engine_length_samples(const audio_engine_t *engine)
{
    uint32_t end = 0u;
    size_t i;

    if (engine == NULL) {
        return 0u;
    }
    for (i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
        const audio_engine_channel_t *ch = &engine->channel[i];
        uint32_t tail;

        if (!ch->in_use) {
            continue;
        }
        tail = ch->start_sample + ch->length_samples +
               ms_to_samples(ch->env.release_ms, engine->sample_rate);
        if (ch->bus == AUDIO_ENGINE_BUS_SFX) {
            tail += ms_to_samples(engine->duck_release_ms, engine->sample_rate);
        }
        if (tail > end) {
            end = tail;
        }
    }
    return end;
}

/** ADSR gain at a sample relative to the channel's own start. */
static int32_t env_gain_q16(const audio_engine_env_t *env, uint32_t pos,
                            uint32_t length_samples, uint32_t rate)
{
    uint32_t attack = ms_to_samples(env->attack_ms, rate);
    uint32_t decay = ms_to_samples(env->decay_ms, rate);
    uint32_t release = ms_to_samples(env->release_ms, rate);
    int32_t sustain = audio_engine_gain_q16(env->sustain_pct);

    if (pos < length_samples) {
        if (attack > 0u && pos < attack) {
            return ramp_q16(0, AUDIO_ENGINE_ONE_Q16, pos, attack);
        }
        if (decay > 0u && pos >= attack && pos - attack < decay) {
            return ramp_q16(AUDIO_ENGINE_ONE_Q16, sustain, pos - attack, decay);
        }
        return sustain;
    }
    if (release == 0u || pos - length_samples >= release) {
        return 0;
    }
    return ramp_q16(sustain, 0, pos - length_samples, release);
}

/** Mono source at a sample relative to the channel's own start. */
static int32_t channel_source(const audio_engine_channel_t *ch, uint32_t pos,
                              uint32_t rate)
{
    if (ch->freq_hz > 0) {
        uint64_t phase = ((uint64_t)pos * (uint64_t)ch->freq_hz * 64u) /
                         (uint64_t)rate;

        return (int32_t)s_sine64[(size_t)(phase & 63u)];
    }
    if (ch->pcm != NULL && ch->pcm_frames != 0u) {
        return (int32_t)ch->pcm[(size_t)(pos % ch->pcm_frames)];
    }
    return 0;
}

/** Music-bus duck multiplier at an absolute sample, rebuilt from SFX events. */
static int32_t duck_gain_at(const audio_engine_t *engine, uint32_t frame)
{
    int32_t duck = AUDIO_ENGINE_ONE_Q16;
    uint32_t attack = ms_to_samples(engine->duck_attack_ms,
                                    engine->sample_rate);
    uint32_t release = ms_to_samples(engine->duck_release_ms,
                                     engine->sample_rate);
    size_t i;

    for (i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
        const audio_engine_channel_t *ch = &engine->channel[i];
        uint32_t end;
        int32_t candidate;

        if (!ch->in_use || ch->bus != AUDIO_ENGINE_BUS_SFX) {
            continue;
        }
        end = ch->start_sample + ch->length_samples;
        if (frame >= ch->start_sample && frame < end) {
            uint32_t pos = frame - ch->start_sample;

            candidate = (attack > 0u && pos < attack) ?
                        ramp_q16(AUDIO_ENGINE_ONE_Q16,
                                 engine->duck_gain_q16, pos, attack) :
                        engine->duck_gain_q16;
        } else if (frame >= end && release > 0u && frame - end < release) {
            candidate = ramp_q16(engine->duck_gain_q16,
                                 AUDIO_ENGINE_ONE_Q16, frame - end, release);
        } else {
            continue;
        }
        if (candidate < duck) {
            duck = candidate;
        }
    }
    return clamp_q16(duck);
}

void audio_engine_render(audio_engine_t *engine, int16_t *stereo_out,
                         size_t frames, uint32_t start_sample)
{
    size_t i;

    if (engine == NULL || stereo_out == NULL) {
        return;
    }
    for (i = 0; i < frames; i++) {
        uint32_t frame = start_sample + (uint32_t)i;
        int64_t music = 0;
        int64_t sfx = 0;
        int32_t sample;
        size_t c;

        for (c = 0; c < AUDIO_ENGINE_MAX_CHANNELS; c++) {
            const audio_engine_channel_t *ch = &engine->channel[c];
            uint32_t tail;
            int32_t env;
            int32_t level;

            if (!ch->in_use || frame < ch->start_sample) {
                continue;
            }
            tail = ch->start_sample + ch->length_samples +
                   ms_to_samples(ch->env.release_ms, engine->sample_rate);
            if (frame >= tail) {
                continue;
            }
            env = env_gain_q16(&ch->env, frame - ch->start_sample,
                               ch->length_samples, engine->sample_rate);
            if (env == 0) {
                continue;
            }
            level = q16_mul(channel_source(ch, frame - ch->start_sample,
                                           engine->sample_rate),
                            (int32_t)(((int64_t)env * ch->gain_q16) /
                                      AUDIO_ENGINE_ONE_Q16));
            if (ch->bus == AUDIO_ENGINE_BUS_SFX) {
                sfx += level;
            } else {
                music += level;
            }
        }
        music = q16_mul(music, engine->music_gain_q16);
        sfx = q16_mul(sfx, engine->sfx_gain_q16);
        music = q16_mul(music, duck_gain_at(engine, frame));
        sample = q16_mul(music + sfx, engine->master_gain_q16);
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }
        stereo_out[2u * i] = (int16_t)sample;
        stereo_out[2u * i + 1u] = (int16_t)sample;
    }
}

void audio_engine_meter_accumulate(const int16_t *stereo, size_t frames,
                                   int64_t *sum_sq_out, int *peak_out)
{
    size_t i;

    if (stereo == NULL || sum_sq_out == NULL || peak_out == NULL) {
        return;
    }
    for (i = 0; i < 2u * frames; i++) {
        int32_t value = stereo[i];
        int32_t magnitude = (value < 0) ? -value : value;

        *sum_sq_out += (int64_t)value * (int64_t)value;
        if (magnitude > *peak_out) {
            *peak_out = magnitude;
        }
    }
}

/** Integer square root (floor), bounded by 16-bit RMS: no libm dependency. */
static uint32_t isqrt_16bit(uint64_t value)
{
    uint32_t low = 0u;
    uint32_t high = 32768u;

    while (low < high) {
        uint32_t mid = low + (high - low + 1u) / 2u;
        uint64_t square = (uint64_t)mid * (uint64_t)mid;

        if (square <= value) {
            low = mid;
        } else {
            high = mid - 1u;
        }
    }
    return low;
}

int audio_engine_meter_finish(int64_t sum_sq, uint64_t sample_count)
{
    uint64_t mean;
    uint32_t root;

    if (sum_sq <= 0 || sample_count == 0u) {
        return 0;
    }
    mean = (uint64_t)sum_sq / sample_count;
    root = isqrt_16bit(mean);
    /* Round to nearest: prefer the higher root when it is closer. */
    {
        uint64_t lower_error = mean - (uint64_t)root * (uint64_t)root;
        uint64_t upper = (uint64_t)root + 1u;
        uint64_t upper_error = upper * upper - mean;

        if (upper_error < lower_error) {
            root = (uint32_t)upper;
        }
    }
    return (int)root;
}
