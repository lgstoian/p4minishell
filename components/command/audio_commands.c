/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file audio_commands.c
 * @brief Audio verbs (beep/tone/wavplay/audio/volume) for P4MiniShell.
 *
 * Argument parsing and transcript output live here; all codec, volume, and
 * background-playback logic lives in components/audio (audio.h). Follows the
 * same split as db_commands.c / alarm_commands.c: the single dispatcher in
 * command.c calls these, it never implements them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "shell.h"
#include "batch.h"
#include "storage.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "audio.h"
#include "command.h"
#include "p4minishell_config.h"
#include "esp_err.h"

void shell_command_volume(int argc, char **argv)
{
    int percent;
    esp_err_t error;

    /* Bare `volume` prints the current codec volume (a query form). */
    if (argc == 1) {
        shell_transcript_appendf_ansi(SH_LBL "volume:" SH_RST " " SH_NUM "%d%%" SH_RST "\n",
                                 audio_get_volume());
        batch_set_errorlevel(0);
        return;
    }

    if (argc != 2 || !shell_parse_percentage_arg(argv[1], &percent)) {
        shell_print_usage("Usage: volume [<0-100>]");
        shell_record_warningf("volume", "Usage error for volume command");
        batch_set_errorlevel(2);
        return;
    }

    error = audio_set_volume(percent);
    if (error != ESP_OK) {
        shell_print_error("volume: failed to initialize the codec speaker path (%s)", esp_err_to_name(error));
        shell_record_errorf("volume", error, "Failed to initialize speaker device");
        batch_set_errorlevel(1);
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "volume set to" SH_RST " " SH_NUM "%d%%" SH_RST "\n", percent);
    batch_set_errorlevel(0);
}

/* ========================================================================
 * AUDIO PLAYBACK COMMANDS: beep, tone, wavplay, audio
 * ======================================================================== */

void shell_command_beep(int argc, char **argv)
{
    if (argc != 1) {
        shell_print_usage("Usage: beep");
        batch_set_errorlevel(2);
        return;
    }
    if (!audio_play_tone(P4_CONFIG_BEEP_FREQ_HZ, P4_CONFIG_BEEP_DURATION_MS)) {
        shell_print_error("beep: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "beep:" SH_RST " " SH_NUM "%d" SH_RST " Hz, " SH_NUM "%u" SH_RST " ms\n",
                             P4_CONFIG_BEEP_FREQ_HZ, (unsigned)P4_CONFIG_BEEP_DURATION_MS);
    batch_set_errorlevel(0);
}

void shell_command_tone(int argc, char **argv)
{
    char *end;
    long freq;
    uint32_t duration_ms = P4_CONFIG_TONE_DURATION_DEFAULT_MS;

    if (argc < 2 || argc > 3) {
        shell_print_usage("Usage: tone <freq> [ms]");
        batch_set_errorlevel(2);
        return;
    }

    freq = strtol(argv[1], &end, 10);
    if (*end != '\0' || freq < P4_CONFIG_TONE_FREQ_MIN || freq > P4_CONFIG_TONE_FREQ_MAX) {
        shell_print_error("tone: frequency must be %d..%d Hz",
                          P4_CONFIG_TONE_FREQ_MIN, P4_CONFIG_TONE_FREQ_MAX);
        batch_set_errorlevel(2);
        return;
    }

    if (argc == 3) {
        long ms = strtol(argv[2], &end, 10);

        if (*end != '\0' || ms < 10 || ms > P4_CONFIG_TONE_DURATION_MAX_MS) {
            shell_print_error("tone: duration must be 10..%d ms", P4_CONFIG_TONE_DURATION_MAX_MS);
            batch_set_errorlevel(2);
            return;
        }
        duration_ms = (uint32_t)ms;
    }

    if (!audio_play_tone((int)freq, duration_ms)) {
        shell_print_error("tone: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "tone:" SH_RST " " SH_NUM "%d" SH_RST " Hz for " SH_NUM "%u" SH_RST " ms\n",
                             (int)freq, (unsigned)duration_ms);
    batch_set_errorlevel(0);
}

void shell_command_wavplay(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    uint64_t file_size;

    if (argc != 2) {
        shell_print_usage("Usage: wavplay <file.wav>");
        batch_set_errorlevel(2);
        return;
    }

    if (shell_fs_resolve_path(argv[1], resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("wavplay: invalid path %s", argv[1]);
        batch_set_errorlevel(2);
        return;
    }

    file_size = storage_get_file_size(resolved);
    if (file_size == 0) {
        shell_print_error("wavplay: file not found %s", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (file_size > P4_CONFIG_WAV_MAX_BYTES) {
        shell_print_error("wavplay: file is too large (max %d bytes)", (int)P4_CONFIG_WAV_MAX_BYTES);
        batch_set_errorlevel(2);
        return;
    }

    if (!audio_play_wav(resolved)) {
        shell_print_error("wavplay: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "wavplay:" SH_RST " " SH_PATH "%s" SH_RST "\n", resolved);
    batch_set_errorlevel(0);
}

/** `audio output [auto|speaker|headphones] [/b] [/v:NAME]`. Query form prints
 *  the mode and effective route; a mode word applies it first. Follows the
 *  csv/timer option pattern: /b anywhere gives one plain route line for
 *  `for /f`, /v:NAME captures the route into the environment. */
static void shell_audio_output_cmd(int argc, char **argv)
{
    bool bare = false;
    char var[64];
    bool has_var = false;
    const char *mode_arg = NULL;
    int i;

    var[0] = '\0';
    for (i = 2; i < argc; i++) {
        const char *arg = argv[i];

        if (arg == NULL || arg[0] == '\0') {
            continue;
        }
        if (arg[0] == '/') {
            if (strcasecmp(arg, "/b") == 0) {
                bare = true;
            } else if (strncasecmp(arg, "/v:", 3) == 0 && strlen(arg) > 3 &&
                       strlen(arg) < sizeof(var)) {
                snprintf(var, sizeof(var), "%s", arg + 3);
                has_var = true;
            } else {
                shell_print_usage("Usage: audio output [auto|speaker|headphones] [/b] [/v:NAME]");
                batch_set_errorlevel(2);
                return;
            }
        } else if (mode_arg == NULL) {
            mode_arg = arg;
        } else {
            shell_print_usage("Usage: audio output [auto|speaker|headphones] [/b] [/v:NAME]");
            batch_set_errorlevel(2);
            return;
        }
    }

    if (mode_arg != NULL) {
        audio_output_mode_t mode;

        if (!audio_output_parse(mode_arg, &mode)) {
            shell_print_error("audio output: expected auto|speaker|headphones");
            batch_set_errorlevel(2);
            return;
        }
        if (audio_set_output_mode(mode) != ESP_OK) {
            shell_print_error("audio output: cannot apply mode");
            batch_set_errorlevel(1);
            return;
        }
    }

    {
        const char *route_name = audio_output_name(audio_effective_route());

        if (has_var && shell_env_set(var, route_name) != ESP_OK) {
            shell_print_error("audio output: cannot set variable %s", var);
            batch_set_errorlevel(1);
            return;
        }
        if (bare) {
            shell_transcript_appendf("%s\n", route_name);
        } else {
            shell_transcript_appendf_ansi(SH_LBL "audio.output:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                          audio_output_name(audio_get_output_mode()));
            shell_transcript_appendf_ansi(SH_LBL "audio.route:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                          route_name);
        }
        batch_set_errorlevel(0);
    }
}

void shell_command_audio(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: audio status | audio stop | audio diag | audio output [auto|speaker|headphones] [/b] [/v:NAME]");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "status")) {
        bool inserted = false;
        bool supported = false;

        if (audio_busy()) {
            shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " " SH_WARN "playing" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " " SH_MUTE "idle" SH_RST "\n");
        }
        audio_headphone_state(&inserted, &supported);
        shell_transcript_appendf_ansi(SH_LBL "audio.output:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                      audio_output_name(audio_get_output_mode()));
        shell_transcript_appendf_ansi(SH_LBL "audio.route:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                      audio_output_name(audio_effective_route()));
        shell_transcript_appendf_ansi(SH_LBL "audio.jack:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                      supported ? (inserted ? "in" : "out") : "n/a");
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "stop")) {
        audio_stop();
        shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " stop requested\n");
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "output")) {
        shell_audio_output_cmd(argc, argv);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "diag")) {
        if (audio_speaker_diag() != ESP_OK) {
            shell_print_error("audio diag: no dumpable codec on this board");
            batch_set_errorlevel(1);
            return;
        }
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: audio status | audio stop | audio diag | audio output [auto|speaker|headphones] [/b] [/v:NAME]");
    batch_set_errorlevel(2);
}

/* ========================================================================
 * MICROPHONE DIAGNOSTICS: mic level [ms] | mic selftest
 * ======================================================================== */

/** `mic level [ms]` — record the mic and report RMS/peak sample levels. */
static void shell_mic_level_cmd(int argc, char **argv)
{
    uint32_t duration_ms = P4_CONFIG_MIC_DEFAULT_MS;
    int rms = 0;
    int peak = 0;
    esp_err_t err;
    char *end = NULL;
    long ms;

    if (argc > 3) {
        shell_print_usage("Usage: mic level [ms]");
        batch_set_errorlevel(2);
        return;
    }
    if (argc == 3) {
        ms = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || ms < 10 ||
            ms > P4_CONFIG_MIC_MAX_MS) {
            shell_print_error("mic: duration must be 10..%d ms",
                              (int)P4_CONFIG_MIC_MAX_MS);
            batch_set_errorlevel(2);
            return;
        }
        duration_ms = (uint32_t)ms;
    }

    err = audio_mic_level(duration_ms, &rms, &peak);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        shell_print_error("mic: no microphone on this board");
        batch_set_errorlevel(1);
        return;
    }
    if (err != ESP_OK) {
        shell_print_error("mic: record failed (%s)", esp_err_to_name(err));
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "mic.rms:" SH_RST " " SH_NUM "%d" SH_RST "\n", rms);
    shell_transcript_appendf_ansi(SH_LBL "mic.peak:" SH_RST " " SH_NUM "%d" SH_RST "\n", peak);
    batch_set_errorlevel(0);
}

/** `mic selftest` — amp-on vs amp-off mic comparison (hiss probe). */
static void shell_mic_selftest_cmd(void)
{
    int rms_on = 0;
    int peak_on = 0;
    int rms_off = 0;
    int peak_off = 0;
    esp_err_t err;

    err = audio_hiss_selftest(P4_CONFIG_MIC_DEFAULT_MS, &rms_on, &peak_on,
                              &rms_off, &peak_off);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        shell_print_error("mic: no microphone on this board");
        batch_set_errorlevel(1);
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        shell_print_error("mic: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    if (err != ESP_OK) {
        shell_print_error("mic: selftest failed (%s)", esp_err_to_name(err));
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "mic.amp_on.rms:" SH_RST " " SH_NUM "%d" SH_RST "\n", rms_on);
    shell_transcript_appendf_ansi(SH_LBL "mic.amp_on.peak:" SH_RST " " SH_NUM "%d" SH_RST "\n", peak_on);
    shell_transcript_appendf_ansi(SH_LBL "mic.amp_off.rms:" SH_RST " " SH_NUM "%d" SH_RST "\n", rms_off);
    shell_transcript_appendf_ansi(SH_LBL "mic.amp_off.peak:" SH_RST " " SH_NUM "%d" SH_RST "\n", peak_off);
    /* Heuristic verdict: the speaker hiss dwarfs the room floor, so a large
     * amp-on excess means the speaker path is noisy while idle. */
    if (rms_on > rms_off * 2 + 300) {
        shell_transcript_appendf_ansi(SH_LBL "mic:" SH_RST " " SH_ERR "HISS - speaker path noisy with amp on" SH_RST "\n");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "mic:" SH_RST " " SH_OK "clean - no amp hiss above the room floor" SH_RST "\n");
    batch_set_errorlevel(0);
}

/** `mic hear <freq> [ms]` — frequency-selective tone detector. */
static void shell_mic_hear_cmd(int argc, char **argv)
{
    uint32_t duration_ms = P4_CONFIG_MIC_DEFAULT_MS;
    int tone_rms = 0;
    int total_rms = 0;
    esp_err_t err;
    char *end = NULL;
    long freq;
    long ms;

    if (argc < 3 || argc > 4) {
        shell_print_usage("Usage: mic hear <freq> [ms]");
        batch_set_errorlevel(2);
        return;
    }
    freq = strtol(argv[2], &end, 10);
    if (end == NULL || *end != '\0' || freq < P4_CONFIG_TONE_FREQ_MIN ||
        freq > P4_CONFIG_TONE_FREQ_MAX) {
        shell_print_error("mic: frequency must be %d..%d Hz",
                          (int)P4_CONFIG_TONE_FREQ_MIN,
                          (int)P4_CONFIG_TONE_FREQ_MAX);
        batch_set_errorlevel(2);
        return;
    }
    if (argc == 4) {
        ms = strtol(argv[3], &end, 10);
        if (end == NULL || *end != '\0' || ms < 10 ||
            ms > P4_CONFIG_MIC_MAX_MS) {
            shell_print_error("mic: duration must be 10..%d ms",
                              (int)P4_CONFIG_MIC_MAX_MS);
            batch_set_errorlevel(2);
            return;
        }
        duration_ms = (uint32_t)ms;
    }

    err = audio_mic_hear((uint32_t)freq, duration_ms, &tone_rms, &total_rms);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        shell_print_error("mic: no microphone on this board");
        batch_set_errorlevel(1);
        return;
    }
    if (err != ESP_OK) {
        shell_print_error("mic: hear failed (%s)", esp_err_to_name(err));
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "mic.hear.tone:" SH_RST " " SH_NUM "%d" SH_RST "\n", tone_rms);
    shell_transcript_appendf_ansi(SH_LBL "mic.hear.total:" SH_RST " " SH_NUM "%d" SH_RST "\n", total_rms);
    /* Verdict: a real tone dominates its band; hiss spreads everywhere. */
    if (tone_rms > total_rms / 2 && tone_rms > 50) {
        shell_transcript_appendf_ansi(SH_LBL "mic:" SH_RST " " SH_OK "heard %ld Hz" SH_RST "\n", freq);
        batch_set_errorlevel(0);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "mic:" SH_RST " " SH_WARN "no %ld Hz tone above the noise" SH_RST "\n", freq);
    batch_set_errorlevel(1);
}

/** `mic audit [freq]` — end-to-end speaker audibility proof in one command. */
static void shell_mic_audit_cmd(int argc, char **argv)
{
    long freq = 880;
    int tone_rms = 0;
    int total_rms = 0;
    int room_rms = 0;
    int room_tone = 0;
    int attempt;
    esp_err_t err;
    char *end = NULL;

    if (argc > 3) {
        shell_print_usage("Usage: mic audit [freq]");
        batch_set_errorlevel(2);
        return;
    }
    if (argc == 3) {
        freq = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || freq < P4_CONFIG_TONE_FREQ_MIN ||
            freq > P4_CONFIG_TONE_FREQ_MAX) {
            shell_print_error("mic: frequency must be %d..%d Hz",
                              (int)P4_CONFIG_TONE_FREQ_MIN,
                              (int)P4_CONFIG_TONE_FREQ_MAX);
            batch_set_errorlevel(2);
            return;
        }
    }

    err = ESP_ERR_TIMEOUT;
    for (attempt = 0; attempt < 3; attempt++) {
        int tr = 0;
        int to = 0;
        int rr = 0;
        int rt = 0;

        err = audio_mic_audit((uint32_t)freq, 1000, &tr, &to, &rr, &rt);
        if (err != ESP_OK) {
            break;
        }
        if (attempt == 0 || tr > tone_rms) {
            tone_rms = tr;
            total_rms = to;
            room_rms = rr;
            room_tone = rt;
        }
        /* A clean dominant detection ends the audit early. */
        if (tr > to / 2 && tr > 50 && tr > rt * 2 + 50) {
            break;
        }
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        shell_print_error("mic: no microphone on this board");
        batch_set_errorlevel(1);
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        shell_print_error("mic: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    if (err != ESP_OK) {
        shell_print_error("mic: audit failed (%s)", esp_err_to_name(err));
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "mic.audit.room:" SH_RST " " SH_NUM "%d" SH_RST "\n", room_rms);
    shell_transcript_appendf_ansi(SH_LBL "mic.audit.tone:" SH_RST " " SH_NUM "%d" SH_RST "\n", tone_rms);
    shell_transcript_appendf_ansi(SH_LBL "mic.audit.total:" SH_RST " " SH_NUM "%d" SH_RST "\n", total_rms);
    /* Verdict: the tone dominates its own band during playback far beyond
     * the same band at rest. Broadband-only energy (hiss) never passes. */
    if (tone_rms > total_rms / 2 && tone_rms > 50 &&
        tone_rms > room_tone * 2 + 50) {
        shell_transcript_appendf_ansi(SH_LBL "mic:" SH_RST " " SH_OK "speaker plays %ld Hz" SH_RST "\n", freq);
        batch_set_errorlevel(0);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "mic:" SH_RST " " SH_ERR "speaker silent at %ld Hz" SH_RST "\n", freq);
    batch_set_errorlevel(1);
}

void shell_command_mic(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: mic level [ms] | mic hear <freq> [ms] | mic audit [freq] | mic selftest");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "level")) {
        shell_mic_level_cmd(argc, argv);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "hear")) {
        shell_mic_hear_cmd(argc, argv);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "audit")) {
        shell_mic_audit_cmd(argc, argv);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "selftest")) {
        shell_mic_selftest_cmd();
        return;
    }
    shell_print_usage("Usage: mic level [ms] | mic hear <freq> [ms] | mic audit [freq] | mic selftest");
    batch_set_errorlevel(2);
}
