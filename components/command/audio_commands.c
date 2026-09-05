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
        shell_print_error("volume: failed to initialize the ES8311 speaker path (%s)", esp_err_to_name(error));
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

void shell_command_audio(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: audio status | audio stop");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "status")) {
        if (audio_busy()) {
            shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " " SH_WARN "playing" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " " SH_MUTE "idle" SH_RST "\n");
        }
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "stop")) {
        audio_stop();
        shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " stop requested\n");
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: audio status | audio stop");
    batch_set_errorlevel(2);
}
