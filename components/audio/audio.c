/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file audio.c
 * @brief Codec audio playback for P4MiniShell (ES8311 on the reference board,
 * ES8388 on the Tab5; both reached through the board BSP).
 *
 * `beep`, `tone`, and `wavplay` hand a play request to a small dedicated
 * task and return immediately, so batch files never block on audio. One
 * sound plays at a time; a new request while one is active is refused with
 * "audio: busy". `audio stop` sets a stop flag the play loop checks so a
 * long tone or WAV can be cut short. Tones synthesize stereo 16-bit PCM at
 * the configured rate; WAVs (16-bit mono/stereo at 22050/44100 Hz) play at
 * their own rate with mono upmixed into both slots. Speaker volume is owned
 * here too and drives all playback. The speaker amp is gated to the playback
 * window only (enabled after open, muted before close; always off while
 * idle), so a powered but clockless codec can never idle as white noise
 * through the speaker.
 */

#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_codec_dev.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ansi_palette.h"
#include "bsp/esp-bsp.h"
#include "board_bsp.h"
#include "p4minishell_config.h"
#include "shell.h"

#define AUDIO_TAG "audio"

/* ---- Hardware ---- */
static esp_codec_dev_handle_t s_speaker_dev;
static int s_volume_percent = P4_CONFIG_VOLUME_DEFAULT_PCT;
static audio_output_mode_t s_output_mode = AUDIO_OUTPUT_AUTO;

/* ---- Background playback engine ---- */
typedef enum {
    AUDIO_PLAY_TONE,
    AUDIO_PLAY_WAV,
} audio_play_kind_t;

typedef struct {
    audio_play_kind_t kind;
    int freq_hz;
    uint32_t duration_ms;
    char wav_path[P4_CONFIG_SD_PATH_BYTES];
} audio_request_t;

static TaskHandle_t s_audio_task;
static SemaphoreHandle_t s_audio_sem;    /* wakes the play task */
static SemaphoreHandle_t s_audio_mutex;  /* protects the request + busy flag */
static volatile bool s_audio_playing;    /* a request is being processed */
static volatile bool s_audio_stop;       /* cut the current playback short */
static volatile bool s_audio_started;    /* the task opened the codec and streams */
static audio_request_t s_audio_request;

static void audio_play_task(void *arg);
static void audio_task_play_tone(const audio_request_t *req);
static void audio_task_play_wav(const audio_request_t *req);
static void audio_amp_mute(void);
static void audio_amp_apply_playback(void);

/** Default sample description for the mono 16-bit microphone path (the
 *  speaker playback uses the stereo variant below). */
static void audio_fill_sample_info(esp_codec_dev_sample_info_t *fs)
{
    fs->sample_rate = 22050;
    fs->channel = 1;
    fs->bits_per_sample = 16;
    fs->channel_mask = 1;
    fs->mclk_multiple = 0;
}

/** Speaker playback runs stereo (the ES8388 expects full PHILIPS frames).
 *  Samples are interleaved LRLR; mono sources are duplicated into both
 *  slots. */
static void audio_fill_speaker_info(esp_codec_dev_sample_info_t *fs)
{
    fs->sample_rate = P4_CONFIG_TONE_SAMPLE_RATE_HZ;
    fs->channel = 2;
    fs->bits_per_sample = 16;
    fs->channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                       ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    fs->mclk_multiple = 0;
}

/** Ensure the speaker codec device exists (lazy, one-time). */
static esp_err_t audio_ensure_speaker(void)
{
    int attempt;

    if (s_speaker_dev == NULL) {
        /* The codec/I2S path needs DMA-capable heap, which can be contended
         * while Wi-Fi, USB and the SD stack initialize (a boot pre-warm in
         * app_main normally wins this race). Retry briefly instead of failing
         * once; the pressure subsides within seconds as boot settles. */
        for (attempt = 0; attempt < 3; attempt++) {
            s_speaker_dev = bsp_audio_codec_speaker_init();
            if (s_speaker_dev != NULL) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (s_speaker_dev == NULL) {
            return ESP_FAIL;
        }
    }

    /* Mute-at-boot invariant: the speaker amp must never be latched on while
     * idle. A powered ES8388 with stopped I2S clocks idles as white noise
     * through the NS4150B, so every ensure path (boot pre-warm, volume
     * queries, mode switches) leaves the amp off. The playback window
     * enables it only after esp_codec_dev_open() has the clocks running and
     * mutes it again before every close. Best-effort: boards whose amp is
     * codec-driven report NOT_SUPPORTED and keep the driver's own control. */
    audio_amp_mute();
    return ESP_OK;
}

/** Ensure the playback task and its synchronization are created. */
static esp_err_t audio_ensure_task(void)
{
    if (s_audio_task != NULL) {
        return ESP_OK;
    }

    if (s_audio_sem == NULL) {
        s_audio_sem = xSemaphoreCreateBinary();
        s_audio_mutex = xSemaphoreCreateMutex();
    }
    if (s_audio_sem == NULL || s_audio_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreateWithCaps(audio_play_task, "audio_play",
                    P4_CONFIG_AUDIO_TASK_STACK, NULL,
                    P4_CONFIG_AUDIO_TASK_PRIORITY, &s_audio_task,
                    MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Play a generated sine tone for @p duration_ms. Runs on the audio task. */
static void audio_task_play_tone(const audio_request_t *req)
{
    esp_codec_dev_sample_info_t fs;
    const double two_pi = 2.0 * 3.14159265358979323846;
    double two_pi_f = two_pi * (double)req->freq_hz /
                      (double)P4_CONFIG_TONE_SAMPLE_RATE_HZ;
    double amplitude = ((double)P4_CONFIG_TONE_AMPLITUDE_PCT / 100.0) * 32767.0;
    uint32_t samples_total = (uint32_t)(((uint64_t)req->duration_ms *
                                        P4_CONFIG_TONE_SAMPLE_RATE_HZ) / 1000u);
    uint32_t fade_samples = P4_CONFIG_TONE_SAMPLE_RATE_HZ / 200u; /* 5 ms */
    uint32_t chunk_samples = P4_CONFIG_TONE_CHUNK_SAMPLES;
    uint32_t samples_written = 0;
    int16_t *chunk;

    audio_fill_speaker_info(&fs);
    if (audio_ensure_speaker() != ESP_OK ||
        esp_codec_dev_open(s_speaker_dev, &fs) != ESP_CODEC_DEV_OK) {
        shell_transcript_appendf_ansi(SH_ERR "tone: failed to open the speaker\n" SH_RST);
        return;
    }
    /* Clocks are running from here on: resolve the jack and gate the amp for
     * the playback window only (see the mute-at-boot invariant above). */
    {
        esp_err_t fixup = bsp_audio_codec_speaker_post_open(s_speaker_dev);

        /* NOT_SUPPORTED is the happy path on boards without the quirk (the
         * reference board's amp lives in its codec driver). */
        if (fixup != ESP_OK && fixup != ESP_ERR_NOT_SUPPORTED) {
            shell_transcript_appendf_ansi(SH_ERR "tone: speaker fixup failed\n" SH_RST);
            shell_record_errorf("audio", fixup, "tone speaker fixup failed");
        }
    }
    /* Belt and braces (mirrors M5Stack's own Tab5 volume path): make sure a
     * stuck digital mute cannot silence the DAC behind a healthy amp. */
    (void)esp_codec_dev_set_out_mute(s_speaker_dev, false);
    audio_amp_apply_playback();

    chunk = malloc(chunk_samples * 2u * sizeof(int16_t));
    if (chunk == NULL) {
        audio_amp_mute();
        esp_codec_dev_close(s_speaker_dev);
        return;
    }
    s_audio_started = true;

    {
        int64_t play_t0 = esp_timer_get_time();
        int64_t write_us = 0;
        int write_rc = ESP_CODEC_DEV_OK;

        while (samples_written < samples_total && !s_audio_stop) {
            uint32_t n = samples_total - samples_written;
            uint32_t i;
            int64_t w0;
            int wrc;

            if (n > chunk_samples) {
                n = chunk_samples;
            }
            for (i = 0; i < n; i++) {
                uint32_t pos = samples_written + i;
                double env = 1.0;
                double sample;
                int16_t pcm;

                if (pos < fade_samples) {
                    env = (double)pos / fade_samples;
                } else if (samples_total - pos < fade_samples) {
                    env = (double)(samples_total - pos) / fade_samples;
                }
                sample = amplitude * env * sin(two_pi_f * pos);
                if (sample > 32767.0) sample = 32767.0;
                if (sample < -32768.0) sample = -32768.0;
                pcm = (int16_t)sample;
                chunk[2u * i] = pcm;
                chunk[2u * i + 1u] = pcm;
            }

            w0 = esp_timer_get_time();
            wrc = esp_codec_dev_write(s_speaker_dev, chunk,
                                      n * 2u * sizeof(int16_t));
            write_us += esp_timer_get_time() - w0;
            if (wrc != ESP_CODEC_DEV_OK) {
                shell_transcript_appendf_ansi(SH_ERR "tone: speaker write failed\n" SH_RST);
                shell_record_errorf("audio", ESP_FAIL, "tone speaker write failed");
                write_rc = wrc;
                break;
            }
            if (samples_written == 0) {
                int16_t lo = chunk[0];
                int16_t hi = chunk[0];
                uint32_t zx = 0;
                uint32_t k;

                for (k = 1; k < 2u * n; k++) {
                    if (chunk[k] < lo) {
                        lo = chunk[k];
                    }
                    if (chunk[k] > hi) {
                        hi = chunk[k];
                    }
                    if ((chunk[k - 1] < 0) != (chunk[k] < 0)) {
                        zx++;
                    }
                }
                shell_record_infof("audio", "tone pcm: lo=%d hi=%d zerocross=%lu/2048",
                                   (int)lo, (int)hi, (unsigned long)zx);
            }
            samples_written += n;
        }

        shell_record_infof("audio", "tone done: %lu/%lu samples rc=%d write_us=%lld total_ms=%lld (t=%lldms)",
                           (unsigned long)samples_written,
                           (unsigned long)samples_total, write_rc,
                           (long long)write_us,
                           (long long)((esp_timer_get_time() - play_t0) / 1000),
                           (long long)(play_t0 / 1000));
    }

    free(chunk);
    audio_amp_mute();
    esp_codec_dev_close(s_speaker_dev);
}

/** Stream a 16-bit PCM WAV from SD. Runs on the audio task. */
static void audio_task_play_wav(const audio_request_t *req)
{
    esp_codec_dev_sample_info_t fs;
    FILE *file;
    uint8_t hdr[12];
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t data_size = 0;
    bool stereo;
    uint32_t chunk_samples = P4_CONFIG_TONE_CHUNK_SAMPLES;
    int16_t *inbuf = NULL;   /* source frames (mono or stereo) */
    int16_t *outbuf = NULL;  /* stereo output frames */
    uint32_t produced = 0;

    file = fopen(req->wav_path, "rb");
    if (file == NULL) {
        shell_transcript_appendf_ansi(SH_ERR "wavplay: cannot open %s\n" SH_RST, req->wav_path);
        return;
    }

    if (fread(hdr, 1, 12, file) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        shell_transcript_appendf_ansi(SH_ERR "wavplay: %s is not a RIFF/WAVE file\n" SH_RST, req->wav_path);
        fclose(file);
        return;
    }

    /* Scan the chunk list for fmt  and data. */
    while (fread(hdr, 1, 8, file) == 8) {
        uint32_t chunk_size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                              ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);

        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[16];

            if (chunk_size >= 16 && fread(fmt, 1, 16, file) == 16) {
                audio_format = (uint16_t)(fmt[0] | (fmt[1] << 8));

                channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
                sample_rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                              ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
                bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
                if (audio_format != 1) {
                    shell_transcript_appendf_ansi(SH_ERR "wavplay: only PCM WAVs are supported\n" SH_RST);
                    fclose(file);
                    return;
                }
            }
            if (chunk_size > 16) {
                (void)fseek(file, chunk_size - 16, SEEK_CUR);
            }
        } else if (memcmp(hdr, "data", 4) == 0) {
            data_size = chunk_size;
            break;
        } else {
            (void)fseek(file, (long)chunk_size + (long)(chunk_size & 1), SEEK_CUR);
        }
    }

    if (!audio_wav_params_ok(audio_format, channels, sample_rate, bits,
                             data_size)) {
        shell_transcript_appendf_ansi(SH_ERR "wavplay: unsupported WAV (need 16-bit PCM mono/stereo "
                                 "at 22050/44100 Hz)\n" SH_RST);
        fclose(file);
        return;
    }

    audio_fill_speaker_info(&fs);
    /* Play at the file's own rate (22050/44100 Hz, both integer-divisible):
     * resampling to the tone rate would pitch-shift the audio. The I2S
     * clocks reconfigure per open; the mic shares them only at equal rates
     * (see audio_mic_record_ms). */
    fs.sample_rate = sample_rate;
    if (audio_ensure_speaker() != ESP_OK ||
        esp_codec_dev_open(s_speaker_dev, &fs) != ESP_CODEC_DEV_OK) {
        shell_transcript_appendf_ansi(SH_ERR "wavplay: failed to open the speaker\n" SH_RST);
        fclose(file);
        return;
    }
    /* Clocks are running from here on: gate the amp for the window only. */
    {
        esp_err_t fixup = bsp_audio_codec_speaker_post_open(s_speaker_dev);

        if (fixup != ESP_OK && fixup != ESP_ERR_NOT_SUPPORTED) {
            shell_transcript_appendf_ansi(SH_ERR "wavplay: speaker fixup failed\n" SH_RST);
            shell_record_errorf("audio", fixup, "wavplay speaker fixup failed");
        }
    }
    audio_amp_apply_playback();

    stereo = (channels == 2);
    inbuf = malloc(chunk_samples * sizeof(int16_t) * (stereo ? 2u : 1u));
    /* Stereo output duplicates mono sources into both slots (the DAC expects
     * full PHILIPS frames). Size for the worst case: undersizing here
     * overruns the heap and corrupts it. */
    outbuf = malloc(chunk_samples * 2u * sizeof(int16_t));
    if (inbuf == NULL || outbuf == NULL) {
        free(inbuf);
        free(outbuf);
        fclose(file);
        audio_amp_mute();
        esp_codec_dev_close(s_speaker_dev);
        return;
    }
    s_audio_started = true;

    while (!s_audio_stop) {
        size_t frames = fread(inbuf, sizeof(int16_t) * (stereo ? 2u : 1u),
                              chunk_samples, file);
        size_t i;

        if (frames == 0) {
            break;
        }
        produced = 0;
        for (i = 0; i < frames; i++) {
            int16_t left;
            int16_t right;

            if (stereo) {
                left = inbuf[2 * i];
                right = inbuf[2 * i + 1];
            } else {
                left = right = inbuf[i];
            }
            outbuf[2 * produced] = left;
            outbuf[2 * produced + 1] = right;
            produced++;
        }
        if (produced > 0 &&
            esp_codec_dev_write(s_speaker_dev, outbuf,
                                produced * 2u * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            shell_transcript_appendf_ansi(SH_ERR "wavplay: speaker write failed\n" SH_RST);
            shell_record_errorf("audio", ESP_FAIL, "wavplay speaker write failed");
            break;
        }
    }

    free(inbuf);
    free(outbuf);
    fclose(file);
    audio_amp_mute();
    esp_codec_dev_close(s_speaker_dev);
}

/** Playback task loop. */
static void audio_play_task(void *arg)
{
    (void)arg;

    while (true) {
        audio_request_t req;

        xSemaphoreTake(s_audio_sem, portMAX_DELAY);
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        req = s_audio_request;
        xSemaphoreGive(s_audio_mutex);

        if (req.kind == AUDIO_PLAY_TONE) {
            audio_task_play_tone(&req);
        } else if (req.kind == AUDIO_PLAY_WAV) {
            audio_task_play_wav(&req);
        }

        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        s_audio_playing = false;
        s_audio_stop = false;
        s_audio_started = false;
        xSemaphoreGive(s_audio_mutex);
    }
}

/* ---- Public API ---- */

esp_err_t audio_init(void)
{
    esp_err_t error = audio_ensure_task();

    if (error != ESP_OK) {
        return error;
    }
    return audio_ensure_speaker();
}

bool audio_play_tone(int freq_hz, uint32_t duration_ms)
{
    if (audio_ensure_task() != ESP_OK) {
        shell_print_error("audio: failed to start the playback task");
        return false;
    }

    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    if (s_audio_playing) {
        xSemaphoreGive(s_audio_mutex);
        return false;
    }
    s_audio_request.kind = AUDIO_PLAY_TONE;
    s_audio_request.freq_hz = freq_hz;
    s_audio_request.duration_ms = duration_ms;
    s_audio_request.wav_path[0] = '\0';
    s_audio_playing = true;
    s_audio_stop = false;
    s_audio_started = false;
    xSemaphoreGive(s_audio_mutex);

    xSemaphoreGive(s_audio_sem);
    return true;
}

bool audio_play_wav(const char *resolved_path)
{
    if (resolved_path == NULL) {
        return false;
    }
    if (audio_ensure_task() != ESP_OK) {
        shell_print_error("audio: failed to start the playback task");
        return false;
    }

    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    if (s_audio_playing) {
        xSemaphoreGive(s_audio_mutex);
        return false;
    }
    s_audio_request.kind = AUDIO_PLAY_WAV;
    s_audio_request.freq_hz = 0;
    s_audio_request.duration_ms = 0;
    snprintf(s_audio_request.wav_path, sizeof(s_audio_request.wav_path), "%s", resolved_path);
    s_audio_playing = true;
    s_audio_stop = false;
    s_audio_started = false;
    xSemaphoreGive(s_audio_mutex);

    xSemaphoreGive(s_audio_sem);
    return true;
}

void audio_stop(void)
{
    s_audio_stop = true;
    /* Cut the hiss immediately; the play loop re-mutes on its close path too,
     * so a stop can never strand the amp on. Harmless when idle. */
    audio_amp_mute();
}

bool audio_busy(void)
{
    return s_audio_playing;
}

/** Report whether the playback task has opened the codec and streams. */
bool audio_playback_started(void)
{
    return s_audio_started;
}

esp_err_t audio_set_volume(int percent)
{
    esp_err_t error;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    error = audio_ensure_speaker();
    if (error != ESP_OK) {
        return error;
    }

    if (esp_codec_dev_set_out_vol(s_speaker_dev, percent) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }

    s_volume_percent = percent;
    /* A volume change must never strand the amp on: gain rides the codec,
     * the amp rides the playback window (mute-at-boot invariant). */
    if (!s_audio_playing) {
        audio_amp_mute();
    }
    return ESP_OK;
}

int audio_get_volume(void)
{
    return s_volume_percent;
}

/* ------------------------------------------------------------------------
 * Output routing (speaker vs headphones)
 * ---------------------------------------------------------------------- */

bool audio_output_parse(const char *text, audio_output_mode_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcasecmp(text, "auto") == 0) {
        *out = AUDIO_OUTPUT_AUTO;
        return true;
    }
    if (strcasecmp(text, "speaker") == 0) {
        *out = AUDIO_OUTPUT_SPEAKER;
        return true;
    }
    if (strcasecmp(text, "headphones") == 0 || strcasecmp(text, "headphone") == 0) {
        *out = AUDIO_OUTPUT_HEADPHONES;
        return true;
    }
    return false;
}

const char *audio_output_name(audio_output_mode_t mode)
{
    switch (mode) {
    case AUDIO_OUTPUT_SPEAKER:
        return "speaker";
    case AUDIO_OUTPUT_HEADPHONES:
        return "headphones";
    case AUDIO_OUTPUT_AUTO:
    default:
        return "auto";
    }
}

audio_output_mode_t audio_route_resolve(audio_output_mode_t mode,
                                        bool hp_inserted, bool hp_supported)
{
    if (mode == AUDIO_OUTPUT_SPEAKER) {
        return AUDIO_OUTPUT_SPEAKER;
    }
    if (mode == AUDIO_OUTPUT_HEADPHONES) {
        return AUDIO_OUTPUT_HEADPHONES;
    }
    if (hp_supported && hp_inserted) {
        return AUDIO_OUTPUT_HEADPHONES;
    }
    return AUDIO_OUTPUT_SPEAKER;
}

void audio_headphone_state(bool *inserted_out, bool *supported_out)
{
    bool inserted = false;
    bool supported = (bsp_audio_headphone_detected(&inserted) == ESP_OK);

    if (inserted_out != NULL) {
        *inserted_out = inserted;
    }
    if (supported_out != NULL) {
        *supported_out = supported;
    }
}

audio_output_mode_t audio_effective_route(void)
{
    bool inserted = false;
    bool supported = false;

    audio_headphone_state(&inserted, &supported);
    return audio_route_resolve(s_output_mode, inserted, supported);
}

/** Amp-enable predicate: on only inside an active speaker-routed window. */
bool audio_amp_should_enable(audio_output_mode_t mode, bool hp_inserted,
                             bool hp_supported, bool playing)
{
    if (!playing) {
        return false;
    }
    return audio_route_resolve(mode, hp_inserted, hp_supported) ==
           AUDIO_OUTPUT_SPEAKER;
}

/** Force the speaker amp off (idle, stop, teardown). Best-effort. */
static void audio_amp_mute(void)
{
    (void)bsp_audio_speaker_enable(false);
}

/** Enable the amp only when the live route actually selects the speaker. */
static void audio_amp_apply_playback(void)
{
    bool inserted = false;
    bool supported = false;

    audio_headphone_state(&inserted, &supported);
    (void)bsp_audio_speaker_enable(
        audio_amp_should_enable(s_output_mode, inserted, supported, true));
}

esp_err_t audio_set_output_mode(audio_output_mode_t mode)
{
    if (mode != AUDIO_OUTPUT_AUTO && mode != AUDIO_OUTPUT_SPEAKER &&
        mode != AUDIO_OUTPUT_HEADPHONES) {
        return ESP_ERR_INVALID_ARG;
    }
    s_output_mode = mode;
    /* A mode switch while idle must not power the amp (mute-at-boot
     * invariant): the route takes effect at the next open. While a stream
     * holds the codec, re-resolve live so unplugging/plugging or forcing a
     * mode flips the amp without waiting for the next play. The codec need
     * not exist for the idle path (amp-only). */
    if (s_audio_playing) {
        audio_amp_apply_playback();
    } else {
        audio_amp_mute();
    }
    return ESP_OK;
}

audio_output_mode_t audio_get_output_mode(void)
{
    return s_output_mode;
}

bool audio_wav_params_ok(uint16_t audio_format, uint16_t channels,
                         uint32_t sample_rate, uint16_t bits,
                         uint32_t data_size)
{
    return audio_format == 1 && bits == 16 &&
           (channels == 1 || channels == 2) &&
           (sample_rate == 22050 || sample_rate == 44100) &&
           data_size != 0;
}

/* ------------------------------------------------------------------------
 * Microphone diagnostics (record + hiss self-test)
 * ---------------------------------------------------------------------- */

static esp_codec_dev_handle_t s_mic_dev;

void audio_mic_accumulate(const int16_t *samples, size_t count,
                          int64_t *sum_sq_out, int *peak_out)
{
    size_t i;

    if (samples == NULL || sum_sq_out == NULL || peak_out == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        int32_t v = samples[i];
        int32_t mag = (v < 0) ? -v : v;

        *sum_sq_out += (int64_t)v * (int64_t)v;
        if (mag > *peak_out) {
            *peak_out = mag;
        }
    }
}

void audio_mic_stats_finish(int64_t sum_sq, uint64_t sample_count,
                            int peak, int *rms_out, int *peak_out)
{
    if (rms_out != NULL) {
        *rms_out = (sample_count == 0) ? 0 :
                   (int)(sqrt((double)sum_sq / (double)sample_count) + 0.5);
    }
    if (peak_out != NULL) {
        *peak_out = peak;
    }
}

void audio_mic_goertzel_feed(const int16_t *samples, size_t count,
                             uint32_t sample_rate, uint32_t freq_hz,
                             double state[3])
{
    double coeff;
    size_t i;

    if (samples == NULL || count == 0 || sample_rate == 0 ||
        freq_hz == 0 || state == NULL) {
        return;
    }
    coeff = 2.0 * cos(2.0 * 3.14159265358979323846 *
                      (double)freq_hz / (double)sample_rate);
    for (i = 0; i < count; i++) {
        double v = (double)samples[i] + coeff * state[0] - state[1];

        state[1] = state[0];
        state[0] = v;
    }
    state[2] = coeff;
}

int audio_mic_goertzel_finish(const double state[3], uint64_t sample_count)
{
    double mag;

    if (state == NULL || sample_count == 0) {
        return 0;
    }
    /* DFT bin magnitude from the two delay states, scaled to an RMS
     * amplitude in 16-bit LSB units (a full-scale sine reads ~23170). */
    mag = sqrt(state[0] * state[0] + state[1] * state[1] -
               state[0] * state[1] * state[2]);
    if (mag <= 0.0) {
        return 0;
    }
    mag = mag * 2.0 / (double)sample_count / 1.4142135623730951;
    if (mag > 32767.0) {
        mag = 32767.0;
    }
    return (int)(mag + 0.5);
}

/** Ensure the microphone codec device exists (lazy, one-time). */
static esp_err_t audio_mic_ensure(void)
{
    if (s_mic_dev != NULL) {
        return ESP_OK;
    }
    s_mic_dev = bsp_audio_codec_microphone_init();
    return (s_mic_dev != NULL) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

/** Record @p duration_ms of mono 16-bit mic audio into running statistics,
 *  optionally feeding a Goertzel tone accumulator (NULL to skip). */
static esp_err_t audio_mic_record_ms(uint32_t duration_ms, int64_t *sum_sq_out,
                                     uint64_t *samples_out, int *peak_out,
                                     double goertzel_state[3],
                                     uint32_t goertzel_freq_hz)
{
    esp_codec_dev_sample_info_t fs;
    uint64_t want_samples;
    uint64_t got_samples = 0;
    int16_t *chunk;

    if (sum_sq_out == NULL || samples_out == NULL || peak_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (audio_mic_ensure() != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    audio_fill_sample_info(&fs);
    fs.sample_rate = P4_CONFIG_MIC_SAMPLE_RATE_HZ;
    if (esp_codec_dev_open(s_mic_dev, &fs) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }    chunk = malloc(P4_CONFIG_MIC_CHUNK_SAMPLES * sizeof(int16_t));
    if (chunk == NULL) {
        esp_codec_dev_close(s_mic_dev);
        return ESP_ERR_NO_MEM;
    }

    want_samples = ((uint64_t)duration_ms * P4_CONFIG_MIC_SAMPLE_RATE_HZ) / 1000u;
    while (got_samples < want_samples) {
        uint64_t left = want_samples - got_samples;
        size_t n = (left > P4_CONFIG_MIC_CHUNK_SAMPLES) ?
                   P4_CONFIG_MIC_CHUNK_SAMPLES : (size_t)left;

        if (esp_codec_dev_read(s_mic_dev, chunk, (int)(n * sizeof(int16_t))) !=
            ESP_CODEC_DEV_OK) {
            break;
        }
        audio_mic_accumulate(chunk, n, sum_sq_out, peak_out);
        if (goertzel_state != NULL && goertzel_freq_hz != 0) {
            audio_mic_goertzel_feed(chunk, n, P4_CONFIG_MIC_SAMPLE_RATE_HZ,
                                    goertzel_freq_hz, goertzel_state);
        }
        got_samples += n;
    }

    free(chunk);
    esp_codec_dev_close(s_mic_dev);
    *samples_out = got_samples;
    return (got_samples == 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t audio_mic_level(uint32_t duration_ms, int *rms_out, int *peak_out)
{
    int64_t sum_sq = 0;
    uint64_t samples = 0;
    int peak = 0;
    int64_t rec_t0 = esp_timer_get_time();
    esp_err_t err;

    /* Deliberately allowed during playback: the mic runs at the speaker's own
     * rate (22050 Hz mono), so sharing the clocks needs no reconfig and the
     * record hears the room plus whatever the speaker emits — the audibility
     * check. (The hiss self-test below still refuses: it force-toggles the
     * amp and must own it.) */
    if (duration_ms == 0 || duration_ms > P4_CONFIG_MIC_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    err = audio_mic_record_ms(duration_ms, &sum_sq, &samples, &peak, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    audio_mic_stats_finish(sum_sq, samples, peak, rms_out, peak_out);
    shell_record_infof("audio", "mic done: rms %d peak %d (t=%lldms)",
                       rms_out != NULL ? *rms_out : -1,
                       peak_out != NULL ? *peak_out : -1,
                       (long long)(rec_t0 / 1000));
    return ESP_OK;
}

esp_err_t audio_hiss_selftest(uint32_t sample_ms, int *rms_on_out,
                              int *peak_on_out, int *rms_off_out,
                              int *peak_off_out)
{
    int64_t sum_sq = 0;
    uint64_t samples = 0;
    int peak = 0;
    esp_err_t err;

    if (s_audio_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_ms == 0 || sample_ms > P4_CONFIG_MIC_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Amp forced on: with the v1.3.0 fault present the mic hears the idle
     * hiss; with the gate fix the amp enable alone (clocks stopped, no open
     * stream) stays silent too, and playback-gated audio is out of scope of
     * this probe. Either way the amp is left OFF on every exit path. */
    (void)bsp_audio_speaker_enable(true);
    err = audio_mic_record_ms(sample_ms, &sum_sq, &samples, &peak, NULL, 0);
    audio_amp_mute();
    if (err != ESP_OK) {
        return err;
    }
    audio_mic_stats_finish(sum_sq, samples, peak, rms_on_out, peak_on_out);

    sum_sq = 0;
    samples = 0;
    peak = 0;
    err = audio_mic_record_ms(sample_ms, &sum_sq, &samples, &peak, NULL, 0);
    audio_amp_mute();
    if (err != ESP_OK) {
        return err;
    }
    audio_mic_stats_finish(sum_sq, samples, peak, rms_off_out, peak_off_out);
    return ESP_OK;
}

esp_err_t audio_mic_hear(uint32_t freq_hz, uint32_t duration_ms,
                         int *tone_rms_out, int *total_rms_out)
{
    int64_t sum_sq = 0;
    uint64_t samples = 0;
    int peak = 0;
    double goertzel[3] = { 0.0, 0.0, 0.0 };
    int total = 0;
    int64_t rec_t0 = esp_timer_get_time();
    esp_err_t err;

    /* Same shared-rate reasoning as audio_mic_level: safe during playback. */
    if (freq_hz < P4_CONFIG_TONE_FREQ_MIN || freq_hz > P4_CONFIG_TONE_FREQ_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms == 0 || duration_ms > P4_CONFIG_MIC_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    err = audio_mic_record_ms(duration_ms, &sum_sq, &samples, &peak,
                              goertzel, freq_hz);
    if (err != ESP_OK) {
        return err;
    }
    audio_mic_stats_finish(sum_sq, samples, peak, &total, NULL);
    if (total_rms_out != NULL) {
        *total_rms_out = total;
    }
    if (tone_rms_out != NULL) {
        *tone_rms_out = audio_mic_goertzel_finish(goertzel, samples);
    }
    shell_record_infof("audio", "mic hear %luHz: tone %d total %d (t=%lldms)",
                       (unsigned long)freq_hz,
                       tone_rms_out != NULL ? *tone_rms_out : -1, total,
                       (long long)(rec_t0 / 1000));
    return ESP_OK;
}

esp_err_t audio_mic_audit(uint32_t freq_hz, uint32_t sample_ms,
                          int *tone_rms_out, int *total_rms_out,
                          int *room_rms_out, int *room_tone_out)
{
    int64_t sum_sq = 0;
    uint64_t samples = 0;
    int peak = 0;
    double goertzel[3] = { 0.0, 0.0, 0.0 };
    int total = 0;
    uint32_t tone_ms;
    esp_err_t err;

    if (freq_hz < P4_CONFIG_TONE_FREQ_MIN || freq_hz > P4_CONFIG_TONE_FREQ_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_ms < 100 || sample_ms > 4000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_playing) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Room floor first (amp as-is: off when idle), broadband and in-band. */
    err = audio_mic_record_ms(500, &sum_sq, &samples, &peak, goertzel,
                              freq_hz);
    if (err != ESP_OK) {
        return err;
    }
    audio_mic_stats_finish(sum_sq, samples, peak, room_rms_out, NULL);
    if (room_tone_out != NULL) {
        *room_tone_out = audio_mic_goertzel_finish(goertzel, samples);
    }
    goertzel[0] = goertzel[1] = goertzel[2] = 0.0;

    /* Play through the real speaker path, then listen while it sounds. Wait
     * for the task to actually stream (task start can lag on a loaded
     * system; a fixed settle would record post-tone hiss and misreport);
     * the tone outlasts the record by 500 ms once started. */
    tone_ms = sample_ms + 500;
    if (!audio_play_tone((int)freq_hz, tone_ms)) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        int waited_ms = 0;

        while (!audio_playback_started() && waited_ms < 2000) {
            vTaskDelay(pdMS_TO_TICKS(20));
            waited_ms += 20;
        }
        if (!audio_playback_started()) {
            audio_stop();
            return ESP_ERR_TIMEOUT;
        }
    }
    sum_sq = 0;
    samples = 0;
    peak = 0;
    err = audio_mic_record_ms(sample_ms, &sum_sq, &samples, &peak,
                              goertzel, freq_hz);
    /* Snapshot the DAC power/mute/clock registers while the tone still
     * streams (proves the chip state behind the acoustic verdict). */
    {
        uint8_t regs[50];

        if (bsp_audio_codec_speaker_dump_regs(regs) == ESP_OK) {
            shell_record_infof("audio", "mic audit regs: dacpwr=%02x mute=%02x lrck=%02x mix=%02x/%02x",
                               regs[0x04], regs[0x19], regs[0x2b],
                               regs[0x27], regs[0x2a]);
        }
    }
    audio_stop();
    audio_amp_mute();
    if (err != ESP_OK) {
        return err;
    }
    audio_mic_stats_finish(sum_sq, samples, peak, &total, NULL);
    if (total_rms_out != NULL) {
        *total_rms_out = total;
    }
    if (tone_rms_out != NULL) {
        *tone_rms_out = audio_mic_goertzel_finish(goertzel, samples);
    }
    shell_record_infof("audio", "mic audit %luHz: tone %d total %d room %d/%d",
                       (unsigned long)freq_hz,
                       tone_rms_out != NULL ? *tone_rms_out : -1, total,
                       room_rms_out != NULL ? *room_rms_out : -1,
                       room_tone_out != NULL ? *room_tone_out : -1);
    return ESP_OK;
}

esp_err_t audio_speaker_diag(void)
{
    uint8_t regs[50];
    esp_err_t err = bsp_audio_codec_speaker_dump_regs(regs);

    if (err != ESP_OK) {
        return err;
    }
    shell_transcript_appendf_ansi(SH_LBL "audio.regs.00:" SH_RST " " SH_VAL "%02x %02x %02x %02x %02x %02x %02x %02x" SH_RST "\n",
                                  regs[0x00], regs[0x01], regs[0x02], regs[0x03],
                                  regs[0x04], regs[0x05], regs[0x06], regs[0x07]);
    shell_transcript_appendf_ansi(SH_LBL "audio.regs.08:" SH_RST " " SH_VAL "%02x %02x %02x %02x %02x %02x %02x %02x" SH_RST "\n",
                                  regs[0x08], regs[0x09], regs[0x0a], regs[0x0b],
                                  regs[0x0c], regs[0x0d], regs[0x0e], regs[0x0f]);
    shell_transcript_appendf_ansi(SH_LBL "audio.regs.10:" SH_RST " " SH_VAL "%02x %02x %02x %02x %02x %02x %02x %02x" SH_RST "\n",
                                  regs[0x10], regs[0x11], regs[0x12], regs[0x13],
                                  regs[0x14], regs[0x15], regs[0x16], regs[0x17]);
    shell_transcript_appendf_ansi(SH_LBL "audio.regs.18:" SH_RST " " SH_VAL "%02x %02x %02x %02x %02x %02x %02x %02x" SH_RST "\n",
                                  regs[0x18], regs[0x19], regs[0x1a], regs[0x1b],
                                  regs[0x1c], regs[0x1d], regs[0x1e], regs[0x1f]);
    shell_transcript_appendf_ansi(SH_LBL "audio.regs.20:" SH_RST " " SH_VAL "%02x %02x %02x %02x %02x %02x %02x %02x" SH_RST "\n",
                                  regs[0x20], regs[0x21], regs[0x22], regs[0x23],
                                  regs[0x24], regs[0x25], regs[0x26], regs[0x27]);
    shell_transcript_appendf_ansi(SH_LBL "audio.regs.28:" SH_RST " " SH_VAL "%02x %02x %02x %02x %02x %02x %02x %02x" SH_RST "\n",
                                  regs[0x28], regs[0x29], regs[0x2a], regs[0x2b],
                                  regs[0x2c], regs[0x2d], regs[0x2e], regs[0x2f]);
    shell_transcript_appendf_ansi(SH_LBL "audio.regs.30:" SH_RST " " SH_VAL "%02x %02x" SH_RST "\n",
                                  regs[0x30], regs[0x31]);
    return ESP_OK;
}
