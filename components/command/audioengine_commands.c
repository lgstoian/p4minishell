/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file audioengine_commands.c
 * @brief Mixer-engine batch verbs: `audio mix|sfx|seq|env|gain|meter`.
 *
 * This is an ADD-ONLY command source. It handles only the six new `audio`
 * subverbs and returns false for everything else, so the existing dispatcher
 * still routes `audio status|stop|diag|output`, `beep`, `tone`, `wavplay`,
 * `mic` and `volume` to the original handlers unchanged. All audio work is
 * delegated to components/audio (audio.h); this file only parses arguments
 * and writes transcript lines / ERRORLEVEL.
 *
 * Wiring owned by the parent (out of this subagent's scope):
 *   command.h:   bool shell_command_audio_engine(int argc, char **argv);
 *   command.c:   before the existing `audio` dispatch -
 *                if (shell_text_equals_ignore_case(argv[0], "audio") &&
 *                    shell_command_audio_engine(argc, argv)) return true;
 *   command/CMakeLists.txt: add "audioengine_commands.c" to SRCS and keep
 *                the existing `audio` REQUIRES entry.
 *   shell_core.c help: extend the "audio" line with the six new subverbs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ansi_palette.h"
#include "audio.h"
#include "audio_engine.h"
#include "batch.h"
#include "command.h"
#include "p4minishell_config.h"
#include "shell.h"

#define AUDIO_ENGINE_USAGE \
    "Usage: audio mix [status|reset|tone <freq> <ms> [gain] [/at:ms] [/bus:music|sfx]|duck <pct>|play [ms]] | " \
    "audio sfx <freq> <ms> [gain] [/at:ms] | audio seq <freq:dur@start[:gain]>... [/bus:music|sfx] | " \
    "audio env [<a>,<d>,<s>,<r>|flat|pluck|pad] | audio gain [<0-100>] | audio meter [reset]"

static bool parse_long_arg(const char *text, long *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0') {
        return false;
    }
    value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static bool option_value(const char *arg, const char *prefix, const char **out)
{
    size_t length = strlen(prefix);

    if (arg == NULL || strncasecmp(arg, prefix, length) != 0 ||
        arg[length] == '\0') {
        return false;
    }
    *out = arg + length;
    return true;
}

static bool parse_bus(const char *text, audio_engine_bus_t *out)
{
    if (text == NULL) {
        return false;
    }
    if (shell_text_equals_ignore_case(text, "sfx")) {
        *out = AUDIO_ENGINE_BUS_SFX;
        return true;
    }
    if (shell_text_equals_ignore_case(text, "music")) {
        *out = AUDIO_ENGINE_BUS_MUSIC;
        return true;
    }
    return false;
}

/** Convert a millisecond offset into the engine's absolute sample index. */
static uint32_t ms_to_sample(long ms)
{
    if (ms <= 0) {
        return 0u;
    }
    return (uint32_t)(((uint64_t)ms * (uint64_t)audio_mix_sample_rate()) /
                      1000u);
}

static void audio_mix_usage_error(void)
{
    shell_print_usage(AUDIO_ENGINE_USAGE);
    batch_set_errorlevel(2);
}

/** `audio mix status|reset|tone|duck|play`. */
static void audio_engine_mix_cmd(int argc, char **argv)
{
    if (argc == 2 || (argc == 3 &&
                      shell_text_equals_ignore_case(argv[2], "status"))) {
        audio_engine_env_t env = audio_mix_env_get_default();

        shell_transcript_appendf_ansi(SH_LBL "audio.mix.ch:" SH_RST " " SH_NUM "%u" SH_RST "/" SH_NUM "%u" SH_RST "\n",
                                      (unsigned)audio_mix_channel_count(),
                                      (unsigned)AUDIO_ENGINE_MAX_CHANNELS);
        shell_transcript_appendf_ansi(SH_LBL "audio.mix.ms:" SH_RST " " SH_NUM "%lu" SH_RST "\n",
                                      (unsigned long)audio_mix_length_ms());
        shell_transcript_appendf_ansi(SH_LBL "audio.mix.gain:" SH_RST " " SH_NUM "%d%%" SH_RST "\n",
                                      audio_mix_gain_get());
        shell_transcript_appendf_ansi(SH_LBL "audio.mix.duck:" SH_RST " " SH_NUM "%d%%" SH_RST "\n",
                                      audio_mix_duck_get());
        shell_transcript_appendf_ansi(SH_LBL "audio.mix.env:" SH_RST " " SH_VAL "%s" SH_RST " a=%lu d=%lu s=%d%% r=%lu\n",
                                      audio_engine_env_preset_name(&env),
                                      (unsigned long)env.attack_ms,
                                      (unsigned long)env.decay_ms,
                                      env.sustain_pct,
                                      (unsigned long)env.release_ms);
        batch_set_errorlevel(0);
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[2], "reset")) {
        if (audio_mix_reset() != ESP_OK) {
            shell_print_error("audio mix: cannot reset the mixer");
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "audio.mix:" SH_RST " reset\n");
        batch_set_errorlevel(0);
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[2], "duck")) {
        audio_mix_usage_error();
        return;
    }
    if (argc == 4 && shell_text_equals_ignore_case(argv[2], "duck")) {
        int percent;

        if (!shell_parse_percentage_arg(argv[3], &percent)) {
            shell_print_error("audio mix: duck must be 0-100%%");
            batch_set_errorlevel(2);
            return;
        }
        audio_mix_duck_set(percent);
        shell_transcript_appendf_ansi(SH_LBL "audio.mix.duck:" SH_RST " " SH_NUM "%d%%" SH_RST "\n",
                                      percent);
        batch_set_errorlevel(0);
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[2], "play")) {
        if (!audio_mix_play(0u)) {
            shell_print_error("audio mix: audio busy - wait for the current sound to finish");
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "audio.mix:" SH_RST " playing " SH_NUM "%lu" SH_RST " ms\n",
                                      (unsigned long)audio_mix_length_ms());
        batch_set_errorlevel(0);
        return;
    }
    if (argc == 4 && shell_text_equals_ignore_case(argv[2], "play")) {
        long ms;

        if (!parse_long_arg(argv[3], &ms) || ms < 10 ||
            ms > P4_CONFIG_TONE_DURATION_MAX_MS) {
            shell_print_error("audio mix: duration must be 10..%d ms",
                              P4_CONFIG_TONE_DURATION_MAX_MS);
            batch_set_errorlevel(2);
            return;
        }
        if (!audio_mix_play((uint32_t)ms)) {
            shell_print_error("audio mix: audio busy - wait for the current sound to finish");
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "audio.mix:" SH_RST " playing " SH_NUM "%ld" SH_RST " ms\n",
                                      ms);
        batch_set_errorlevel(0);
        return;
    }

    if (argc >= 5 && shell_text_equals_ignore_case(argv[2], "tone")) {
        long freq;
        long ms;
        int gain = 100;
        long at_ms = 0;
        audio_engine_bus_t bus = AUDIO_ENGINE_BUS_MUSIC;
        audio_engine_env_t env = audio_mix_env_get_default();
        int index;
        int i;

        if (!parse_long_arg(argv[3], &freq) || freq < P4_CONFIG_TONE_FREQ_MIN ||
            freq > P4_CONFIG_TONE_FREQ_MAX ||
            !parse_long_arg(argv[4], &ms) || ms < 10 ||
            ms > P4_CONFIG_TONE_DURATION_MAX_MS) {
            shell_print_error("audio mix: freq %d..%d Hz, duration 10..%d ms",
                              P4_CONFIG_TONE_FREQ_MIN,
                              P4_CONFIG_TONE_FREQ_MAX,
                              P4_CONFIG_TONE_DURATION_MAX_MS);
            batch_set_errorlevel(2);
            return;
        }
        {
            bool have_gain = false;

            for (i = 5; i < argc; i++) {
                const char *value = NULL;

                if (option_value(argv[i], "/at:", &value)) {
                    if (!parse_long_arg(value, &at_ms) || at_ms < 0 ||
                        at_ms > 60000) {
                        shell_print_error("audio mix: /at must be 0..60000 ms");
                        batch_set_errorlevel(2);
                        return;
                    }
                } else if (option_value(argv[i], "/bus:", &value)) {
                    if (!parse_bus(value, &bus)) {
                        shell_print_error("audio mix: /bus expects music or sfx");
                        batch_set_errorlevel(2);
                        return;
                    }
                } else if (!have_gain &&
                           shell_parse_percentage_arg(argv[i], &gain)) {
                    have_gain = true;
                } else {
                    audio_mix_usage_error();
                    return;
                }
            }
        }
        index = audio_mix_add_tone(ms_to_sample(at_ms), ms_to_sample(ms),
                                   (int)freq, gain, bus, &env);
        if (index < 0) {
            shell_print_error("audio mix: no free channel (max %u)",
                              (unsigned)AUDIO_ENGINE_MAX_CHANNELS);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "audio.mix.ch:" SH_RST " " SH_NUM "%d" SH_RST " freq=" SH_NUM "%ld" SH_RST " ms=" SH_NUM "%ld" SH_RST " at=" SH_NUM "%ld" SH_RST " bus=" SH_VAL "%s" SH_RST "\n",
                                      index, freq, ms, at_ms,
                                      bus == AUDIO_ENGINE_BUS_SFX ? "sfx" : "music");
        shell_transcript_appendf_ansi(SH_LBL "audio.mix.env:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                      audio_engine_env_preset_name(&env));
        batch_set_errorlevel(0);
        return;
    }

    audio_mix_usage_error();
}

/** `audio sfx <freq> <ms> [gain] [/at:ms]` - one shot on the ducking SFX bus. */
static void audio_engine_sfx_cmd(int argc, char **argv)
{
    long freq;
    long ms;
    long at_ms = 0;
    int gain = 100;
    audio_engine_env_t env;
    int index;
    int i;

    if (argc < 5) {
        audio_mix_usage_error();
        return;
    }
    if (!audio_engine_env_parse("pluck", &env)) {
        shell_print_error("audio sfx: envelope preset unavailable");
        batch_set_errorlevel(1);
        return;
    }
    if (!parse_long_arg(argv[2], &freq) || freq < P4_CONFIG_TONE_FREQ_MIN ||
        freq > P4_CONFIG_TONE_FREQ_MAX ||
        !parse_long_arg(argv[3], &ms) || ms < 10 ||
        ms > P4_CONFIG_TONE_DURATION_MAX_MS) {
        shell_print_error("audio sfx: freq %d..%d Hz, duration 10..%d ms",
                          P4_CONFIG_TONE_FREQ_MIN, P4_CONFIG_TONE_FREQ_MAX,
                          P4_CONFIG_TONE_DURATION_MAX_MS);
        batch_set_errorlevel(2);
        return;
    }
    for (i = 4; i < argc; i++) {
        const char *value = NULL;

        if (option_value(argv[i], "/at:", &value)) {
            if (!parse_long_arg(value, &at_ms) || at_ms < 0 ||
                at_ms > 60000) {
                shell_print_error("audio sfx: /at must be 0..60000 ms");
                batch_set_errorlevel(2);
                return;
            }
        } else if (!shell_parse_percentage_arg(argv[i], &gain)) {
            audio_mix_usage_error();
            return;
        }
    }
    index = audio_mix_add_tone(ms_to_sample(at_ms), ms_to_sample(ms),
                               (int)freq, gain, AUDIO_ENGINE_BUS_SFX, &env);
    if (index < 0) {
        shell_print_error("audio sfx: no free channel (max %u)",
                          (unsigned)AUDIO_ENGINE_MAX_CHANNELS);
        batch_set_errorlevel(1);
        return;
    }
    if (!audio_mix_play(0u)) {
        shell_print_error("audio sfx: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "audio.sfx.ch:" SH_RST " " SH_NUM "%d" SH_RST " bus=" SH_VAL "sfx" SH_RST "\n",
                                  index);
    shell_transcript_appendf_ansi(SH_LBL "audio.sfx:" SH_RST " " SH_NUM "%ld" SH_RST " Hz " SH_NUM "%ld" SH_RST " ms\n",
                                  freq, ms);
    batch_set_errorlevel(0);
}

/** `audio seq <freq:dur@start[:gain]>... [/bus:music|sfx]`. */
static void audio_engine_seq_cmd(int argc, char **argv)
{
    audio_engine_bus_t bus = AUDIO_ENGINE_BUS_MUSIC;
    audio_engine_env_t env = audio_mix_env_get_default();
    char order[192];
    size_t used = 0;
    int added = 0;
    int i;

    order[0] = '\0';
    for (i = 2; i < argc; i++) {
        const char *value = NULL;
        audio_engine_seq_event_t event;
        int index;

        if (option_value(argv[i], "/bus:", &value)) {
            if (!parse_bus(value, &bus)) {
                shell_print_error("audio seq: /bus expects music or sfx");
                batch_set_errorlevel(2);
                return;
            }
            continue;
        }
        if (!audio_engine_seq_parse(argv[i], audio_mix_sample_rate(), &event)) {
            shell_print_error("audio seq: bad event %s (want freq:dur@start[:gain])",
                              argv[i]);
            batch_set_errorlevel(2);
            return;
        }
        index = audio_mix_add_event(&event, bus, &env);
        if (index < 0) {
            shell_print_error("audio seq: no free channel (max %u)",
                              (unsigned)AUDIO_ENGINE_MAX_CHANNELS);
            batch_set_errorlevel(1);
            return;
        }
        {
            int n = snprintf(order + used, sizeof(order) - used, "%s%d@%lu",
                             used == 0 ? "" : ",", event.freq_hz,
                             (unsigned long)(event.start_sample * 1000u /
                                             audio_mix_sample_rate()));

            if (n > 0 && (size_t)n < sizeof(order) - used) {
                used += (size_t)n;
            }
        }
        added++;
    }
    if (added == 0) {
        audio_mix_usage_error();
        return;
    }
    if (!audio_mix_play(0u)) {
        shell_print_error("audio seq: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "audio.seq.events:" SH_RST " " SH_NUM "%d" SH_RST "\n",
                                  added);
    shell_transcript_appendf_ansi(SH_LBL "audio.seq.order:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                  order);
    batch_set_errorlevel(0);
}

/** `audio env [a,d,s,r|flat|pluck|pad]`. */
static void audio_engine_env_cmd(int argc, char **argv)
{
    audio_engine_env_t env;

    if (argc == 2) {
        env = audio_mix_env_get_default();
        shell_transcript_appendf_ansi(SH_LBL "audio.env:" SH_RST " " SH_VAL "%s" SH_RST " a=%lu d=%lu s=%d%% r=%lu\n",
                                      audio_engine_env_preset_name(&env),
                                      (unsigned long)env.attack_ms,
                                      (unsigned long)env.decay_ms,
                                      env.sustain_pct,
                                      (unsigned long)env.release_ms);
        batch_set_errorlevel(0);
        return;
    }
    if (argc != 3 || !audio_engine_env_parse(argv[2], &env)) {
        shell_print_usage("Usage: audio env [<attack_ms>,<decay_ms>,<sustain_pct>,<release_ms>|flat|pluck|pad]");
        batch_set_errorlevel(2);
        return;
    }
    audio_mix_env_set_default(&env);
    shell_transcript_appendf_ansi(SH_LBL "audio.env:" SH_RST " " SH_VAL "%s" SH_RST " a=%lu d=%lu s=%d%% r=%lu\n",
                                  audio_engine_env_preset_name(&env),
                                  (unsigned long)env.attack_ms,
                                  (unsigned long)env.decay_ms,
                                  env.sustain_pct,
                                  (unsigned long)env.release_ms);
    batch_set_errorlevel(0);
}

/** `audio gain [<0-100>]` - mixer master, NOT the codec `volume` level. */
static void audio_engine_gain_cmd(int argc, char **argv)
{
    int percent;

    if (argc == 2) {
        shell_transcript_appendf_ansi(SH_LBL "audio.mix.gain:" SH_RST " " SH_NUM "%d%%" SH_RST "\n",
                                      audio_mix_gain_get());
        batch_set_errorlevel(0);
        return;
    }
    if (argc != 3 || !shell_parse_percentage_arg(argv[2], &percent)) {
        shell_print_usage("Usage: audio gain [<0-100>] (mixer level; use volume for the codec)");
        batch_set_errorlevel(2);
        return;
    }
    audio_mix_gain_set(percent);
    shell_transcript_appendf_ansi(SH_LBL "audio.mix.gain:" SH_RST " " SH_NUM "%d%%" SH_RST "\n",
                                  audio_mix_gain_get());
    batch_set_errorlevel(0);
}

/** `audio meter [reset]` - peak/RMS of the last rendered mixer chunk. */
static void audio_engine_meter_cmd(int argc, char **argv)
{
    int rms = 0;
    int peak = 0;

    if (argc == 3 && shell_text_equals_ignore_case(argv[2], "reset")) {
        audio_mix_meter_reset();
        shell_transcript_appendf_ansi(SH_LBL "audio.meter:" SH_RST " reset\n");
        batch_set_errorlevel(0);
        return;
    }
    if (argc != 2) {
        shell_print_usage("Usage: audio meter [reset]");
        batch_set_errorlevel(2);
        return;
    }
    audio_mix_meter(&rms, &peak);
    shell_transcript_appendf_ansi(SH_LBL "audio.meter.peak:" SH_RST " " SH_NUM "%d" SH_RST "\n",
                                  peak);
    shell_transcript_appendf_ansi(SH_LBL "audio.meter.rms:" SH_RST " " SH_NUM "%d" SH_RST "\n",
                                  rms);
    batch_set_errorlevel(0);
}

bool shell_command_audio_engine(int argc, char **argv)
{
    if (argv == NULL || argc < 2 || argv[1] == NULL) {
        return false;
    }
    if (shell_text_equals_ignore_case(argv[1], "mix")) {
        audio_engine_mix_cmd(argc, argv);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "sfx")) {
        audio_engine_sfx_cmd(argc, argv);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "seq")) {
        audio_engine_seq_cmd(argc, argv);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "env")) {
        audio_engine_env_cmd(argc, argv);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "gain")) {
        audio_engine_gain_cmd(argc, argv);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "meter")) {
        audio_engine_meter_cmd(argc, argv);
        return true;
    }
    /* status / stop / diag / output and every other verb stay untouched. */
    return false;
}
