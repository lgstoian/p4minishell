/**
 * @file config_cmd.c
 * @brief `config` command: read/write the CONFIG.SYS settings file.
 *
 * The shell settings that live only in RAM (brightness, rotation, volume,
 * prompt template, Wi-Fi auto-connect policy, display-idle timeout) plus the
 * OSK/HEADER boot prefs are persisted into the SAME CONFIG.SYS file the boot
 * component parses at startup. `config KEY=VALUE` applies a setting now AND
 * updates CONFIG.SYS, so the next boot re-applies it with zero new boot code.
 *
 * The directive line-editing helpers (config_directive_get/upsert/remove) are
 * pure text functions and are covered by the unit test project.
 */

#include "config_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "storage.h"
#include "storage_commands.h"
#include "shell.h"
#include "ansi_palette.h"
#include "batch.h"
#include "command.h"
#include "audio.h"
#include "display.h"
#include "header.h"
#include "keyboard.h"
#include "networking.h"
#include "wifi_known.h"

#define SHELL_COMMAND_BYTES         P4_CONFIG_COMMAND_BYTES

/* Settings keys are at most 16 characters ("WIFI_AUTOCONNECT"). */
#define CONFIG_KEY_BYTES            32

/* ========================================================================
 * TRACKED SETTINGS
 * ======================================================================== */

typedef struct {
    const char *key;                 /* CONFIG.SYS directive name, upper case */
    const char *default_value;       /* directive value at factory default */
    bool (*apply)(const char *value);       /* apply directive value now */
    void (*render)(char *out, size_t out_size); /* render current value */
} config_setting_t;

static bool config_apply_brightness(const char *value)
{
    char *end = NULL;
    long pct;

    if (value == NULL || *value == '\0') {
        return false;
    }
    pct = strtol(value, &end, 10);
    if (*end != '\0' || pct < 0 || pct > 100) {
        return false;
    }
    display_set_brightness((int)pct);
    return true;
}

static void config_render_brightness(char *out, size_t out_size)
{
    snprintf(out, out_size, "%d", display_get_brightness());
}

static bool config_apply_rotate(const char *value)
{
    display_rotation_t rotation;

    if (value == NULL || *value == '\0' ||
        display_rotation_parse(value, &rotation) != ESP_OK) {
        return false;
    }
    display_set_rotation(rotation);
    return true;
}

static void config_render_rotate(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", display_rotation_to_string(display_get_rotation()));
}

static bool config_apply_volume(const char *value)
{
    char *end = NULL;
    long pct;

    if (value == NULL || *value == '\0') {
        return false;
    }
    pct = strtol(value, &end, 10);
    if (*end != '\0' || pct < 0 || pct > 100) {
        return false;
    }
    audio_set_volume((int)pct);
    return true;
}

static void config_render_volume(char *out, size_t out_size)
{
    snprintf(out, out_size, "%d", audio_get_volume());
}

static bool config_apply_prompt(const char *value)
{
    if (value == NULL) {
        return false;
    }
    shell_prompt_set_template(value);
    return true;
}

static void config_render_prompt(char *out, size_t out_size)
{
    const char *template_text = shell_prompt_get_template();

    snprintf(out, out_size, "%s", template_text != NULL ? template_text : "");
}

static bool config_apply_on_off_setting(const char *value, void (*set_on)(void),
                                        void (*set_off)(void))
{
    if (value == NULL) {
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        set_on();
        return true;
    }
    if (shell_text_equals_ignore_case(value, "OFF")) {
        set_off();
        return true;
    }
    return false;
}

static bool config_apply_wifi_autoconnect(const char *value)
{
    if (value == NULL) {
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        networking_wifi_set_boot_autoconnect(true);
        return true;
    }
    if (shell_text_equals_ignore_case(value, "OFF")) {
        networking_wifi_set_boot_autoconnect(false);
        return true;
    }
    return false;
}

static void config_render_wifi_autoconnect(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", networking_wifi_get_boot_autoconnect() ? "ON" : "OFF");
}

static bool config_apply_display_timeout(const char *value)
{
    char *end = NULL;
    long secs;

    if (value == NULL || *value == '\0') {
        return false;
    }
    if (shell_text_equals_ignore_case(value, "OFF")) {
        shell_power_set_idle_timeout(0);
        return true;
    }
    secs = strtol(value, &end, 10);
    if (*end != '\0' || secs < 0) {
        return false;
    }
    shell_power_set_idle_timeout((int)secs);
    return true;
}

static void config_render_display_timeout(char *out, size_t out_size)
{
    snprintf(out, out_size, "%d", shell_power_get_idle_timeout());
}

static bool config_apply_osk(const char *value)
{
    return config_apply_on_off_setting(value, keyboard_show, keyboard_hide);
}

static void config_render_osk(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", keyboard_is_visible() ? "ON" : "OFF");
}

static bool config_apply_header(const char *value)
{
    bool on;

    if (value == NULL) {
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        on = true;
    } else if (shell_text_equals_ignore_case(value, "OFF")) {
        on = false;
    } else {
        return false;
    }
    header_set_visible(on);
    /* The layout depends on the header region height; relayout now. */
    display_schedule_ui_rebuild();
    return true;
}

static void config_render_header(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", header_get_visible() ? "ON" : "OFF");
}

static const config_setting_t config_settings[] = {
    { "BRIGHTNESS",       STR(P4_CONFIG_DISPLAY_DEFAULT_BRIGHTNESS), config_apply_brightness,       config_render_brightness },
    { "ROTATE",           "0",                                        config_apply_rotate,           config_render_rotate },
    { "VOLUME",           STR(P4_CONFIG_VOLUME_DEFAULT_PCT),         config_apply_volume,           config_render_volume },
    { "PROMPT",           P4_CONFIG_PROMPT_DEFAULT_TEMPLATE,         config_apply_prompt,           config_render_prompt },
    { "WIFI_AUTOCONNECT", "ON",                                      config_apply_wifi_autoconnect, config_render_wifi_autoconnect },
    { "DISPLAY_TIMEOUT",  "0",                                       config_apply_display_timeout,  config_render_display_timeout },
    { "OSK",              "ON",                                      config_apply_osk,              config_render_osk },
    { "HEADER",           "ON",                                      config_apply_header,           config_render_header },
};

#define CONFIG_SETTING_COUNT (sizeof(config_settings) / sizeof(config_settings[0]))

static const config_setting_t *config_find(const char *key)
{
    size_t i;

    for (i = 0; i < CONFIG_SETTING_COUNT; i++) {
        if (shell_text_equals_ignore_case(config_settings[i].key, key)) {
            return &config_settings[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * PURE DIRECTIVE LINE EDITING
 * ======================================================================== */

int config_directive_get(const char *text, const char *key, char *out, size_t out_size)
{
    /* The pure INI line editor lives in components/storage (storage_ini.c) so
     * the batch `ini` command and the applib state group share it; the config
     * command keeps its public name as a thin wrapper. */
    return storage_ini_get_value(text, key, out, out_size);
}

bool config_directive_remove(char *text, size_t cap, const char *key)
{
    return storage_ini_remove(text, cap, key);
}

bool config_directive_upsert(char *text, size_t cap, const char *key, const char *value)
{
    return storage_ini_upsert(text, cap, key, value);
}

/* ========================================================================
 * CONFIG.SYS FILE I/O (guarded, atomic)
 * ======================================================================== */

static void config_file_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", BSP_SD_MOUNT_POINT, P4_CONFIG_BOOT_CONFIG_SYS_NAME);
}

/** Read CONFIG.SYS into a heap buffer (cap+1 bytes, NUL-terminated). */
static char *config_read_file(size_t *out_len)
{
    char path[128];
    char *buffer = NULL;
    shell_sd_session_t session;
    FILE *file = NULL;

    if (out_len != NULL) {
        *out_len = 0;
    }

    if (shell_sd_begin(&session) != ESP_OK) {
        return NULL;
    }
    config_file_path(path, sizeof(path));
    file = fopen(path, "r");
    if (file == NULL) {
        shell_sd_end(&session, "config");
        return NULL;
    }

    buffer = malloc(P4_CONFIG_CONFIG_MAX_BYTES + 1);
    if (buffer == NULL) {
        fclose(file);
        shell_sd_end(&session, "config");
        return NULL;
    }
    {
        size_t got = fread(buffer, 1, P4_CONFIG_CONFIG_MAX_BYTES, file);

        buffer[got] = '\0';
        if (out_len != NULL) {
            *out_len = got;
        }
    }

    fclose(file);
    shell_sd_end(&session, "config");
    return buffer;
}

/** Write the full CONFIG.SYS text atomically (temp file + rename). */
static bool config_write_file(const char *text)
{
    char path[128];
    char tmp[160];
    shell_sd_session_t session;
    FILE *file = NULL;
    size_t text_len = strlen(text);

    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    config_file_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    {
        uint64_t needed = (uint64_t)text_len + 512;
        uint64_t reclaim = storage_get_file_size(path);

        if (!storage_check_free_space(needed, reclaim, "config")) {
            shell_sd_end(&session, "config");
            return false;
        }
    }

    file = fopen(tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, "config");
        return false;
    }
    if (fwrite(text, 1, text_len, file) != text_len || fflush(file) != 0 || fclose(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        remove(tmp);
        shell_sd_end(&session, "config");
        return false;
    }
    file = NULL;

    /* FATFS f_rename refuses to overwrite an existing target. */
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) {
            remove(tmp);
            shell_sd_end(&session, "config");
            return false;
        }
    }

    shell_sd_end(&session, "config");
    return true;
}

/* ========================================================================
 * COMMAND IMPLEMENTATION
 * ======================================================================== */

static void config_show_all(void)
{
    char *file_text = NULL;
    size_t file_len = 0;
    size_t i;

    file_text = config_read_file(&file_len);

    shell_transcript_appendf_ansi(SH_SUBHEAD "CONFIG.SYS settings" SH_RST "\n");
    for (i = 0; i < CONFIG_SETTING_COUNT; i++) {
        const config_setting_t *setting = &config_settings[i];
        char current[128];
        int persisted = -1;

        setting->render(current, sizeof(current));
        if (file_text != NULL) {
            persisted = config_directive_get(file_text, setting->key, NULL, 0);
        }
        if (persisted >= 0) {
            shell_transcript_appendf_ansi("  " SH_EXE "%s" SH_RST "=%s  (default %s)" SH_MUTE " [saved]" SH_RST "\n",
                                          setting->key, current, setting->default_value);
        } else {
            shell_transcript_appendf_ansi("  " SH_EXE "%s" SH_RST "=%s  (default %s)\n",
                                          setting->key, current, setting->default_value);
        }
    }
    free(file_text);
}

static void config_show_one(const config_setting_t *setting)
{
    char *file_text = NULL;
    size_t file_len = 0;
    char current[128];
    char persisted[128];
    int plen = -1;

    file_text = config_read_file(&file_len);
    setting->render(current, sizeof(current));
    if (file_text != NULL) {
        plen = config_directive_get(file_text, setting->key, persisted, sizeof(persisted));
    }

    if (plen >= 0) {
        shell_transcript_appendf_ansi("  " SH_EXE "%s" SH_RST "=%s  (default %s, saved)\n",
                                      setting->key, persisted, setting->default_value);
    } else {
        shell_transcript_appendf_ansi("  " SH_EXE "%s" SH_RST "=%s  (default %s, not saved)\n",
                                      setting->key, current, setting->default_value);
    }
    free(file_text);
}

/** Get an empty, editable CONFIG.SYS buffer (or the existing file text). */
static char *config_open_file_text(void)
{
    char *file_text = config_read_file(NULL);

    if (file_text == NULL) {
        file_text = malloc(P4_CONFIG_CONFIG_MAX_BYTES + 1);
        if (file_text != NULL) {
            file_text[0] = '\0';
        }
    }
    return file_text;
}

static void config_set(const config_setting_t *setting, const char *value)
{
    char *file_text;

    if (!setting->apply(value)) {
        shell_print_usage("config: invalid value for %s", setting->key);
        batch_set_errorlevel(2);
        return;
    }

    file_text = config_open_file_text();
    if (file_text == NULL) {
        shell_print_error("config: out of memory");
        batch_set_errorlevel(1);
        return;
    }
    if (!config_directive_upsert(file_text, P4_CONFIG_CONFIG_MAX_BYTES, setting->key, value)) {
        shell_print_error("config: CONFIG.SYS too large to update");
        free(file_text);
        batch_set_errorlevel(1);
        return;
    }
    if (!config_write_file(file_text)) {
        shell_print_error("config: failed to write %s", P4_CONFIG_BOOT_CONFIG_SYS_NAME);
        free(file_text);
        batch_set_errorlevel(1);
        return;
    }
    shell_print_ok("config: %s saved to %s", setting->key, P4_CONFIG_BOOT_CONFIG_SYS_NAME);
    free(file_text);
    batch_set_errorlevel(0);
}

static void config_save_all(void)
{
    char *file_text;
    size_t i;

    file_text = config_open_file_text();
    if (file_text == NULL) {
        shell_print_error("config: out of memory");
        batch_set_errorlevel(1);
        return;
    }
    for (i = 0; i < CONFIG_SETTING_COUNT; i++) {
        char current[128];

        config_settings[i].render(current, sizeof(current));
        if (!config_directive_upsert(file_text, P4_CONFIG_CONFIG_MAX_BYTES,
                                     config_settings[i].key, current)) {
            shell_print_error("config: CONFIG.SYS too large to update");
            free(file_text);
            batch_set_errorlevel(1);
            return;
        }
    }
    if (!config_write_file(file_text)) {
        shell_print_error("config: failed to write %s", P4_CONFIG_BOOT_CONFIG_SYS_NAME);
        free(file_text);
        batch_set_errorlevel(1);
        return;
    }
    shell_print_ok("config: current settings saved to %s", P4_CONFIG_BOOT_CONFIG_SYS_NAME);
    free(file_text);
    batch_set_errorlevel(0);
}

static void config_reset_one(const config_setting_t *setting)
{
    char *file_text;
    bool changed = false;

    (void)setting->apply(setting->default_value);

    file_text = config_read_file(NULL);
    if (file_text != NULL) {
        changed = config_directive_remove(file_text, P4_CONFIG_CONFIG_MAX_BYTES, setting->key);
        if (changed) {
            if (!config_write_file(file_text)) {
                shell_print_error("config: failed to write %s", P4_CONFIG_BOOT_CONFIG_SYS_NAME);
                free(file_text);
                batch_set_errorlevel(1);
                return;
            }
        }
        free(file_text);
    }

    shell_print_ok("config: %s reset to default (%s)", setting->key, setting->default_value);
    batch_set_errorlevel(0);
}

static void config_reset_all(void)
{
    char *file_text = config_read_file(NULL);
    bool changed = false;
    size_t i;

    for (i = 0; i < CONFIG_SETTING_COUNT; i++) {
        (void)config_settings[i].apply(config_settings[i].default_value);
    }

    if (file_text != NULL) {
        for (i = 0; i < CONFIG_SETTING_COUNT; i++) {
            if (config_directive_remove(file_text, P4_CONFIG_CONFIG_MAX_BYTES, config_settings[i].key)) {
                changed = true;
            }
        }
        if (changed && !config_write_file(file_text)) {
            shell_print_error("config: failed to write %s", P4_CONFIG_BOOT_CONFIG_SYS_NAME);
            free(file_text);
            batch_set_errorlevel(1);
            return;
        }
        free(file_text);
    }

    shell_print_ok("config: all settings reset to defaults");
    batch_set_errorlevel(0);
}

static void config_factory(void)
{
    static const char *const wipe_files[] = {
        P4_CONFIG_BOOT_CONFIG_SYS_NAME,
        P4_CONFIG_BOOT_AUTOEXEC_BAT_NAME,
        P4_CONFIG_WIFI_KNOWN_FILE,
        P4_CONFIG_ALIAS_PROFILE,
        P4_CONFIG_HISTORY_PROFILE,
    };
    char path[128];
    shell_sd_session_t session;
    size_t i;

    if (!shell_confirm_destructive(
            "config factory",
            "config: factory reset restores defaults and deletes CONFIG.SYS, AUTOEXEC.BAT, WIFI.KNOWN, ALIASES.BAT, and HISTORY.TXT",
            NULL)) {
        batch_set_errorlevel(1);
        return;
    }

    for (i = 0; i < CONFIG_SETTING_COUNT; i++) {
        (void)config_settings[i].apply(config_settings[i].default_value);
    }

    shell_history_clear();

    {
        char names[P4_CONFIG_ALIAS_MAX][P4_CONFIG_ALIAS_NAME_BYTES];
        char value[P4_CONFIG_ALIAS_VALUE_BYTES];
        int count = shell_alias_count();
        int k;

        if (count > (int)P4_CONFIG_ALIAS_MAX) {
            count = (int)P4_CONFIG_ALIAS_MAX;
        }
        for (k = 0; k < count; k++) {
            if (!shell_alias_get_by_index(k, names[k], sizeof(names[k]),
                                          value, sizeof(value))) {
                break;
            }
        }
        for (k = 0; k < count; k++) {
            (void)shell_alias_set(names[k], "");
        }
    }

    (void)networking_wifi_known_clear();
    (void)networking_wifi_set_boot_credentials("", "");

    if (shell_sd_begin(&session) == ESP_OK) {
        for (i = 0; i < sizeof(wipe_files) / sizeof(wipe_files[0]); i++) {
            snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, wipe_files[i]);
            (void)remove(path);
        }
        shell_sd_end(&session, "config");
    }

    shell_print_ok("config: factory reset complete - defaults restored, files will be regenerated at next boot");
    batch_set_errorlevel(0);
}

void shell_command_config(int argc, char **argv)
{
    const config_setting_t *setting;

    if (argc == 1) {
        config_show_all();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "save")) {
        if (argc != 2) {
            shell_print_usage("Usage: config save");
            batch_set_errorlevel(2);
            return;
        }
        config_save_all();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "reset")) {
        if (argc == 2) {
            config_reset_all();
        } else if (argc == 3) {
            setting = config_find(argv[2]);
            if (setting == NULL) {
                shell_print_usage("config: unknown setting %s", argv[2]);
                batch_set_errorlevel(2);
                return;
            }
            config_reset_one(setting);
        } else {
            shell_print_usage("Usage: config reset [setting]");
            batch_set_errorlevel(2);
        }
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "factory")) {
        if (argc != 2) {
            shell_print_usage("Usage: config factory");
            batch_set_errorlevel(2);
            return;
        }
        config_factory();
        return;
    }

    /* The rest is "KEY", "KEY=VALUE", or "KEY value ...". */
    {
        char key[CONFIG_KEY_BYTES];
        const char *inline_value = NULL;
        char *joined = NULL;
        const char *value;
        size_t key_len;

        key[0] = '\0';
        {
            const char *eq = strchr(argv[1], '=');

            if (eq != NULL) {
                key_len = (size_t)(eq - argv[1]);
                if (key_len == 0 || key_len >= sizeof(key)) {
                    shell_print_usage("config: malformed KEY=VALUE");
                    batch_set_errorlevel(2);
                    return;
                }
                memcpy(key, argv[1], key_len);
                key[key_len] = '\0';
                inline_value = eq + 1;
            } else {
                key_len = strlen(argv[1]);
                if (key_len >= sizeof(key)) {
                    shell_print_usage("config: unknown setting %s", argv[1]);
                    batch_set_errorlevel(2);
                    return;
                }
                memcpy(key, argv[1], key_len);
                key[key_len] = '\0';
            }
        }

        setting = config_find(key);
        if (setting == NULL) {
            shell_print_usage("config: unknown setting %s", key);
            batch_set_errorlevel(2);
            return;
        }

        if (argc == 2 && inline_value == NULL) {
            config_show_one(setting);
            return;
        }

        /* The value may be split across several tokens (PROMPT templates
         * contain spaces). Join everything after the key. */
        joined = malloc(SHELL_COMMAND_BYTES);
        if (joined == NULL) {
            shell_print_error("config: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        if (inline_value != NULL) {
            size_t used;

            snprintf(joined, SHELL_COMMAND_BYTES, "%s", inline_value);
            used = strlen(joined);
            if (argc > 2 && used + 1 < SHELL_COMMAND_BYTES) {
                joined[used++] = ' ';
                shell_join_args(argv, 2, argc, joined + used, SHELL_COMMAND_BYTES - used);
            }
        } else {
            shell_join_args(argv, 2, argc, joined, SHELL_COMMAND_BYTES);
        }
        value = joined;

        config_set(setting, value);
        free(joined);
    }
}
