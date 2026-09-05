/**
 * @file audio.c
 * @brief ES8311 audio playback for P4MiniShell.
 *
 * `beep`, `tone`, and `wavplay` hand a play request to a small dedicated
 * task and return immediately, so batch files never block on audio. One
 * sound plays at a time; a new request while one is active is refused with
 * "audio: busy". `audio stop` sets a stop flag the play loop checks so a
 * long tone or WAV can be cut short. The codec path is mono 16-bit at
 * 22050 Hz (the BSP default), so stereo WAVs are mixed and 44100 Hz WAVs
 * are decimated by two. Speaker volume is owned here too and drives all
 * playback.
 */

#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ansi_palette.h"
#include "bsp/esp-bsp.h"
#include "p4minishell_config.h"
#include "shell.h"

#define AUDIO_TAG "audio"

/* ---- Hardware ---- */
static esp_codec_dev_handle_t s_speaker_dev;
static int s_volume_percent = P4_CONFIG_VOLUME_DEFAULT_PCT;

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
static audio_request_t s_audio_request;

static void audio_play_task(void *arg);
static void audio_task_play_tone(const audio_request_t *req);
static void audio_task_play_wav(const audio_request_t *req);

/** Default sample description for the ES8311 mono 16-bit 22050 Hz path. */
static void audio_fill_sample_info(esp_codec_dev_sample_info_t *fs)
{
    fs->sample_rate = 22050;
    fs->channel = 1;
    fs->bits_per_sample = 16;
    fs->channel_mask = 1;
    fs->mclk_multiple = 0;
}

/** Ensure the speaker codec device exists (lazy, one-time). */
static esp_err_t audio_ensure_speaker(void)
{
    int attempt;

    if (s_speaker_dev != NULL) {
        return ESP_OK;
    }

    /* The codec/I2S path needs DMA-capable heap, which can be contended
     * while Wi-Fi, USB and the SD stack initialize (a boot pre-warm in
     * app_main normally wins this race). Retry briefly instead of failing
     * once; the pressure subsides within seconds as boot settles. */
    for (attempt = 0; attempt < 3; attempt++) {
        s_speaker_dev = bsp_audio_codec_speaker_init();
        if (s_speaker_dev != NULL) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_FAIL;
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

    if (xTaskCreate(audio_play_task, "audio_play",
                    P4_CONFIG_AUDIO_TASK_STACK, NULL,
                    P4_CONFIG_AUDIO_TASK_PRIORITY, &s_audio_task) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Play a generated sine tone for @p duration_ms. Runs on the audio task. */
static void audio_task_play_tone(const audio_request_t *req)
{
    esp_codec_dev_sample_info_t fs;
    const double two_pi = 2.0 * 3.14159265358979323846;
    double two_pi_f = two_pi * (double)req->freq_hz / 22050.0;
    double amplitude = ((double)P4_CONFIG_TONE_AMPLITUDE_PCT / 100.0) * 32767.0;
    uint32_t samples_total = (uint32_t)(((uint64_t)req->duration_ms * 22050u) / 1000u);
    uint32_t fade_samples = 22050u / 200u;          /* 5 ms fade in/out */
    uint32_t chunk_samples = P4_CONFIG_TONE_CHUNK_SAMPLES;
    uint32_t samples_written = 0;
    int16_t *chunk;

    audio_fill_sample_info(&fs);
    if (audio_ensure_speaker() != ESP_OK ||
        esp_codec_dev_open(s_speaker_dev, &fs) != ESP_CODEC_DEV_OK) {
        shell_transcript_appendf_ansi(SH_ERR "tone: failed to open the speaker\n" SH_RST);
        return;
    }

    chunk = malloc(chunk_samples * sizeof(int16_t));
    if (chunk == NULL) {
        esp_codec_dev_close(s_speaker_dev);
        return;
    }

    while (samples_written < samples_total && !s_audio_stop) {
        uint32_t n = samples_total - samples_written;
        uint32_t i;

        if (n > chunk_samples) {
            n = chunk_samples;
        }
        for (i = 0; i < n; i++) {
            uint32_t pos = samples_written + i;
            double env = 1.0;
            double sample;

            if (pos < fade_samples) {
                env = (double)pos / fade_samples;
            } else if (samples_total - pos < fade_samples) {
                env = (double)(samples_total - pos) / fade_samples;
            }
            sample = amplitude * env * sin(two_pi_f * pos);
            if (sample > 32767.0) sample = 32767.0;
            if (sample < -32768.0) sample = -32768.0;
            chunk[i] = (int16_t)sample;
        }

        if (esp_codec_dev_write(s_speaker_dev, chunk, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            break;
        }
        samples_written += n;
    }

    free(chunk);
    esp_codec_dev_close(s_speaker_dev);
}

/** Stream a 16-bit PCM WAV from SD. Runs on the audio task. */
static void audio_task_play_wav(const audio_request_t *req)
{
    esp_codec_dev_sample_info_t fs;
    FILE *file;
    uint8_t hdr[12];
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t data_size = 0;
    bool stereo;
    bool decimate;
    uint32_t chunk_samples = P4_CONFIG_TONE_CHUNK_SAMPLES;
    int16_t *inbuf = NULL;   /* source frames (mono or stereo) */
    int16_t *outbuf = NULL;  /* mono 22050 output */
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
                uint16_t audio_format = (uint16_t)(fmt[0] | (fmt[1] << 8));

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

    if (bits != 16 || (channels != 1 && channels != 2) ||
        (sample_rate != 22050 && sample_rate != 44100) ||
        data_size == 0) {
        shell_transcript_appendf_ansi(SH_ERR "wavplay: unsupported WAV (need 16-bit PCM mono/stereo "
                                 "at 22050/44100 Hz)\n" SH_RST);
        fclose(file);
        return;
    }

    audio_fill_sample_info(&fs);
    if (audio_ensure_speaker() != ESP_OK ||
        esp_codec_dev_open(s_speaker_dev, &fs) != ESP_CODEC_DEV_OK) {
        shell_transcript_appendf_ansi(SH_ERR "wavplay: failed to open the speaker\n" SH_RST);
        fclose(file);
        return;
    }

    stereo = (channels == 2);
    decimate = (sample_rate == 44100);
    inbuf = malloc(chunk_samples * sizeof(int16_t) * (stereo ? 2u : 1u));
    outbuf = malloc((chunk_samples / 2) * sizeof(int16_t));
    if (inbuf == NULL || outbuf == NULL) {
        free(inbuf);
        free(outbuf);
        fclose(file);
        esp_codec_dev_close(s_speaker_dev);
        return;
    }

    while (!s_audio_stop) {
        size_t frames = fread(inbuf, sizeof(int16_t) * (stereo ? 2u : 1u),
                              chunk_samples, file);
        size_t i;

        if (frames == 0) {
            break;
        }
        produced = 0;
        for (i = 0; i < frames; i += decimate ? 2u : 1u) {
            int32_t sample;

            if (stereo) {
                sample = ((int32_t)inbuf[2 * i] + (int32_t)inbuf[2 * i + 1]) / 2;
            } else {
                sample = inbuf[i];
            }
            outbuf[produced++] = (int16_t)sample;
        }
        if (produced > 0 &&
            esp_codec_dev_write(s_speaker_dev, outbuf, produced * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            break;
        }
    }

    free(inbuf);
    free(outbuf);
    fclose(file);
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
    xSemaphoreGive(s_audio_mutex);

    xSemaphoreGive(s_audio_sem);
    return true;
}

void audio_stop(void)
{
    s_audio_stop = true;
}

bool audio_busy(void)
{
    return s_audio_playing;
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
    return ESP_OK;
}

int audio_get_volume(void)
{
    return s_volume_percent;
}
