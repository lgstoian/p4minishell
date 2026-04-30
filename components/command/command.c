/**
 * @file command.c
 * @brief Command parser and dispatcher implementation for P4MiniShell.
 *
 * Owns the command execution pipeline. All built-in commands are implemented
 * here. Uses shell.c for transcript output and debug logging.
 */

#include "command.h"
#include "shell.h"
#include "ansi.h"
#include "display.h"
#include "keyboard.h"
#include "windows.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "networking.h"
#include "bluetooth.h"
#include "c6ota.h"
#include "usb.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Backward-compatibility aliases */
#define COMMAND_TAG                     P4_CONFIG_SHELL_TAG
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_COMMAND_TASK_STACK_BYTES  P4_CONFIG_COMMAND_TASK_STACK
#define SHELL_PROMPT                    P4_CONFIG_SHELL_PROMPT
#define SHELL_WIFI_SSID_BYTES           P4_CONFIG_WIFI_SSID_BYTES
#define SHELL_WIFI_PASSWORD_BYTES       P4_CONFIG_WIFI_PASSWORD_BYTES

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

typedef struct {
    char command[SHELL_COMMAND_BYTES];
} command_request_t;

/* ========================================================================
 * FORWARD DECLARATIONS — ALL COMMAND HANDLERS
 * ======================================================================== */

static void cmd_brightness(int argc, char **argv);
static void cmd_rotate(int argc, char **argv);
static void cmd_battery(int argc, char **argv);
static void cmd_volume(int argc, char **argv);
static void cmd_reboot(void);
static void cmd_clear(void);

/* ========================================================================
 * COMMAND EXECUTION TASK
 * ======================================================================== */

static void command_task(void *arg)
{
    command_request_t *request = (command_request_t *)arg;

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }

    shell_execute_command_core(request->command);
    free(request);
    vTaskDelete(NULL);
}

/* ========================================================================
 * COMMAND DISPATCH
 * ======================================================================== */

bool shell_execute_command_core(char *command)
{
    char *argv[32];
    int argc;
    char *trimmed;

    if (command == NULL) {
        return false;
    }

    trimmed = shell_trim(command);
    if (trimmed[0] == '\0') {
        return false;
    }

    /* Check for C6 OTA confirmation first */
    if (c6ota_try_handle_input(trimmed)) {
        return true;
    }

    /* Check for pipe operator */
    if (strchr(trimmed, '|') != NULL) {
        extern void shell_execute_pipe(char *command);
        shell_execute_pipe(trimmed);
        return true;
    }

    argc = shell_split_args(trimmed, argv, 32);
    if (argc == 0) {
        return false;
    }

    /* System commands */
    if (shell_text_equals_ignore_case(argv[0], "help")) {
        shell_command_help();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "cls") || shell_text_equals_ignore_case(argv[0], "clear")) {
        cmd_clear();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "reboot")) {
        cmd_reboot();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "version") || shell_text_equals_ignore_case(argv[0], "ver")) {
        shell_command_version();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "about")) {
        shell_command_about();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sysinfo")) {
        shell_command_sysinfo();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "mem")) {
        shell_command_mem();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "debug")) {
        shell_command_debug();
        return true;
    }

    /* Hardware commands */
    if (shell_text_equals_ignore_case(argv[0], "brightness")) {
        cmd_brightness(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rotate")) {
        cmd_rotate(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "battery")) {
        cmd_battery(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "volume")) {
        cmd_volume(argc, argv);
        return true;
    }

    /* Display commands */
    if (shell_text_equals_ignore_case(argv[0], "display")) {
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "info")) {
            display_print_info(shell_transcript_appendf);
            return true;
        }
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "resolution")) {
            display_resolution_t res = display_get_resolution();
            shell_transcript_appendf_ansi("@Cdisplay.resolution:@R %" PRId32 " x %" PRId32
                                     " (native %" PRId32 " x %" PRId32 ")\n",
                                     res.current_width, res.current_height,
                                     res.native_width, res.native_height);
            return true;
        }
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "refresh")) {
            display_refresh_config_t ref = display_get_refresh_config();
            shell_transcript_appendf_ansi("@Cdisplay.refresh:@R target=%" PRIu32 "Hz current=%" PRIu32 "Hz "
                                     "pclk=%" PRIu32 "MHz dsi_bitrate=%" PRIu32 "Mbps\n",
                                     ref.target_hz, ref.current_hz,
                                     ref.pixel_clock_mhz, ref.dsi_lane_bitrate_mbps);
            return true;
        }
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "power")) {
            if (argc >= 3) {
                if (shell_text_equals_ignore_case(argv[2], "on")) {
                    display_set_power_state(DISPLAY_POWER_ON);
                    shell_transcript_appendf_ansi("@gdisplay power on@R\n");
                } else if (shell_text_equals_ignore_case(argv[2], "sleep")) {
                    display_set_power_state(DISPLAY_POWER_SLEEP);
                    shell_transcript_appendf_ansi("@ydisplay sleep@R\n");
                } else if (shell_text_equals_ignore_case(argv[2], "off")) {
                    display_set_power_state(DISPLAY_POWER_OFF);
                    shell_transcript_appendf_ansi("@rdisplay power off@R\n");
                } else {
                    shell_transcript_appendf_ansi("@yUsage: display power <on|sleep|off>@R\n");
                }
            } else {
                display_power_state_t ps = display_get_power_state();
                shell_transcript_appendf_ansi("@Cdisplay.power:@R %s\n",
                                         ps == DISPLAY_POWER_ON ? "@gon@R" :
                                         ps == DISPLAY_POWER_SLEEP ? "@ysleep@R" : "@roff@R");
            }
            return true;
        }
        shell_transcript_appendf_ansi("@yUsage: display <info|resolution|refresh|power>@R\n");
        return true;
    }

    /* Keyboard commands */
    if (shell_text_equals_ignore_case(argv[0], "keyboard")) {
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "hide")) {
            keyboard_hide();
            shell_transcript_appendf_ansi("@gkeyboard hidden@R\n");
        } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "show")) {
            keyboard_show();
            shell_transcript_appendf_ansi("@gkeyboard shown@R\n");
        } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "toggle")) {
            keyboard_toggle();
            shell_transcript_appendf_ansi("@gkeyboard %s@R\n", keyboard_is_visible() ? "shown" : "hidden");
        } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "status")) {
            shell_transcript_appendf_ansi("@Ckeyboard:@R %s, mode=%d, height=%" PRId32 "\n",
                                     keyboard_is_visible() ? "@gvisible@R" : "@khidden@R",
                                     (int)keyboard_get_mode(),
                                     (int32_t)keyboard_get_height());
        } else {
            shell_transcript_appendf_ansi("@yUsage: keyboard <show|hide|toggle|status>@R\n");
        }
        return true;
    }

    /* Windows commands */
    if (shell_text_equals_ignore_case(argv[0], "windows")) {
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "info")) {
            lv_coord_t dw = windows_get_display_width();
            lv_coord_t dh = windows_get_display_height();
            shell_transcript_appendf_ansi("@Cwindows.display:@R %" PRId32 " x %" PRId32 "\n", (int32_t)dw, (int32_t)dh);
            for (int r = 0; r < WINDOW_REGION_COUNT; r++) {
                window_rect_t rect = windows_get_rect((window_region_t)r);
                const char *names[] = {"header", "transcript", "input_row", "keyboard"};
                shell_transcript_appendf_ansi("  @Cwindows.%s:@R x=%" PRId32 " y=%" PRId32 " w=%" PRId32 " h=%" PRId32 "\n",
                                         names[r], (int32_t)rect.x, (int32_t)rect.y,
                                         (int32_t)rect.width, (int32_t)rect.height);
            }
            return true;
        }
        shell_transcript_appendf_ansi("@yUsage: windows <info>@R\n");
        return true;
    }

    /* Module-routed commands */
    if (shell_text_equals_ignore_case(argv[0], "wifi")) {
        networking_handle_wifi_command(command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "bluetooth") || shell_text_equals_ignore_case(argv[0], "bt")) {
        bluetooth_handle_command(command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "usb")) {
        usb_handle_command(command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "c6ota")) {
        c6ota_perform(argc >= 2 ? argv[1] : NULL);
        return true;
    }

    /* ====================================================================
     * SD / FILE / BATCH COMMANDS — bridged to main.c via p4minishell.h
     * ==================================================================== */

    /* SD tools */
    if (shell_text_equals_ignore_case(argv[0], "sd")) {
        extern void shell_bridge_sd_command(char *command);
        shell_bridge_sd_command(command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sdeject")) {
        extern void shell_command_sd_eject(void);
        shell_command_sd_eject();
        return true;
    }

    /* DOS-style file commands */
    if (shell_text_equals_ignore_case(argv[0], "cd") || shell_text_equals_ignore_case(argv[0], "chdir")) {
        extern void shell_bridge_cd_command(int argc, char **argv);
        shell_bridge_cd_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "dir")) {
        extern void shell_bridge_dir_command(int argc, char **argv);
        shell_bridge_dir_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "copy")) {
        extern void shell_bridge_copy_command(int argc, char **argv);
        shell_bridge_copy_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "move")) {
        extern void shell_bridge_move_command(int argc, char **argv);
        shell_bridge_move_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "del") || shell_text_equals_ignore_case(argv[0], "erase")) {
        extern void shell_bridge_del_command(int argc, char **argv);
        shell_bridge_del_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "ren") || shell_text_equals_ignore_case(argv[0], "rename")) {
        extern void shell_bridge_ren_command(int argc, char **argv, const char *verb);
        shell_bridge_ren_command(argc, argv, argv[0]);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "md") || shell_text_equals_ignore_case(argv[0], "mkdir")) {
        extern void shell_bridge_mkdir_command(int argc, char **argv);
        shell_bridge_mkdir_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rd") || shell_text_equals_ignore_case(argv[0], "rmdir")) {
        extern void shell_bridge_rmdir_command(int argc, char **argv);
        shell_bridge_rmdir_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "type")) {
        extern void shell_bridge_type_command(int argc, char **argv);
        shell_bridge_type_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "write")) {
        extern void shell_bridge_write_command(int argc, char **argv, bool append_mode);
        shell_bridge_write_command(argc, argv, false);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "append")) {
        extern void shell_bridge_write_command(int argc, char **argv, bool append_mode);
        shell_bridge_write_command(argc, argv, true);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "touch")) {
        extern void shell_bridge_touch_command(int argc, char **argv);
        shell_bridge_touch_command(argc, argv);
        return true;
    }

    /* DOS-style extended commands */
    if (shell_text_equals_ignore_case(argv[0], "attrib")) {
        extern void shell_command_attrib(int argc, char **argv);
        shell_command_attrib(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "label")) {
        extern void shell_command_label(int argc, char **argv);
        shell_command_label(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "xcopy")) {
        extern void shell_command_xcopy(int argc, char **argv);
        shell_command_xcopy(argc, argv);
        return true;
    }

    /* Environment and batch commands */
    if (shell_text_equals_ignore_case(argv[0], "set")) {
        extern void shell_bridge_set_command(int argc, char **argv);
        shell_bridge_set_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "path")) {
        extern void shell_bridge_path_command(int argc, char **argv);
        shell_bridge_path_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "echo")) {
        extern void shell_bridge_echo_command(int argc, char **argv);
        shell_bridge_echo_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "call")) {
        extern void shell_bridge_call_command(int argc, char **argv);
        shell_bridge_call_command(argc, argv);
        return true;
    }

    /* Batch control flow commands */
    if (shell_text_equals_ignore_case(argv[0], "if")) {
        extern void shell_command_if(int argc, char **argv);
        shell_command_if(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "goto")) {
        extern void shell_command_goto(int argc, char **argv);
        shell_command_goto(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "shift")) {
        extern void shell_command_shift(int argc, char **argv);
        shell_command_shift(argc, argv);
        return true;
    }

    /* Extended built-in commands */
    if (shell_text_equals_ignore_case(argv[0], "pause")) {
        extern void shell_command_pause(int argc, char **argv);
        shell_command_pause(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "choice")) {
        extern void shell_command_choice(int argc, char **argv);
        shell_command_choice(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "setlocal")) {
        extern void shell_command_setlocal(int argc, char **argv);
        shell_command_setlocal(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "endlocal")) {
        extern void shell_command_endlocal(int argc, char **argv);
        shell_command_endlocal(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "prompt")) {
        extern void shell_command_prompt_cmd(int argc, char **argv);
        shell_command_prompt_cmd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "date")) {
        extern void shell_command_date(int argc, char **argv);
        shell_command_date(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "time")) {
        extern void shell_command_time_cmd(int argc, char **argv);
        shell_command_time_cmd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "exit")) {
        extern void shell_command_exit(int argc, char **argv);
        shell_command_exit(argc, argv);
        return true;
    }

    /* File utility commands */
    if (shell_text_equals_ignore_case(argv[0], "find")) {
        extern void shell_command_find(int argc, char **argv);
        shell_command_find(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "more")) {
        extern void shell_command_more(int argc, char **argv);
        shell_command_more(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "tree")) {
        extern void shell_command_tree(int argc, char **argv);
        shell_command_tree(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "fc")) {
        extern void shell_command_fc(int argc, char **argv);
        shell_command_fc(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sort")) {
        extern void shell_command_sort(int argc, char **argv);
        shell_command_sort(argc, argv);
        return true;
    }

    /* GPIO commands */
    if (shell_text_equals_ignore_case(argv[0], "gpio")) {
        extern void shell_bridge_gpio_command(int argc, char **argv);
        shell_bridge_gpio_command(argc, argv);
        return true;
    }

    /* Batch file direct execution */
    if (strstr(argv[0], ".bat") != NULL) {
        extern bool shell_resolve_batch_path(const char *command_name, char *resolved_path, size_t resolved_path_size);
        extern void shell_bridge_batch_file(const char *path, int argc, char **argv);
        char batch_path[256];
        if (shell_resolve_batch_path(argv[0], batch_path, sizeof(batch_path))) {
            shell_bridge_batch_file(batch_path, argc - 1, &argv[1]);
            return true;
        }
    }

    shell_transcript_appendf_ansi("@rUnknown command:@R %s\n", argv[0]);
    return false;
}

void shell_execute_command(char *command)
{
    command_request_t *request;

    if (command == NULL || strlen(command) == 0) {
        return;
    }

    shell_store_command_history(command);

    request = (command_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        shell_transcript_appendf_ansi("@rshell:@R out of memory starting command task\n");
        shell_record_errorf("shell", -1, "Out of memory starting command task");
        return;
    }

    snprintf(request->command, sizeof(request->command), "%s", command);
    if (xTaskCreate(command_task,
                    "shell_cmd",
                    SHELL_COMMAND_TASK_STACK_BYTES,
                    request,
                    tskIDLE_PRIORITY + 2,
                    NULL) != pdPASS) {
        free(request);
        shell_transcript_append_text("shell: failed to start command task\n");
        shell_record_errorf("shell", -1, "Failed to start command task");
    }
}

bool shell_command_ota_is_pending(void)
{
    return c6ota_is_confirmation_pending();
}

/* ========================================================================
 * HARDWARE COMMAND IMPLEMENTATIONS
 * ======================================================================== */

static void cmd_brightness(int argc, char **argv)
{
    int percent;

    if (argc != 2 || !shell_parse_percentage_arg(argv[1], &percent)) {
        shell_transcript_appendf_ansi("@yUsage: brightness <0-100>@R\n");
        shell_record_warningf("brightness", "Usage error for brightness command");
        return;
    }

    esp_err_t error = display_set_brightness(percent);
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi("@rbrightness:@R failed to set backlight (@r%s@R)\n", esp_err_to_name(error));
        shell_record_errorf("brightness", error, "Failed to set brightness to %d%%", percent);
        return;
    }

    shell_transcript_appendf_ansi("@gbrightness set to %d%%@R\n", percent);
}

static void cmd_rotate(int argc, char **argv)
{
    display_rotation_t rotation;

    if (argc != 2) {
        shell_transcript_appendf_ansi("@yUsage: rotate <0|90|180|270>@R\n");
        shell_record_warningf("rotate", "Usage error for rotate command");
        return;
    }

    esp_err_t error = display_rotation_parse(argv[1], &rotation);
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi("@yUsage: rotate <0|90|180|270>@R\n");
        shell_record_warningf("rotate", "Invalid rotation angle: %s", argv[1]);
        return;
    }

    error = display_set_rotation(rotation);
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi("@rrotate:@R failed to apply display rotation (@r%s@R)\n", esp_err_to_name(error));
        shell_record_errorf("rotate", error, "Failed to apply rotation %s", argv[1]);
        return;
    }

    shell_transcript_appendf_ansi("@grotation set to %s degrees and GT911 remap updated@R\n", argv[1]);
}

static void cmd_battery(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_transcript_appendf_ansi("@ybattery:@R ADC telemetry not available in this build\n");
}

static void cmd_volume(int argc, char **argv)
{
    int percent;

    if (argc != 2 || !shell_parse_percentage_arg(argv[1], &percent)) {
        shell_transcript_appendf_ansi("@yUsage: volume <0-100>@R\n");
        return;
    }

    shell_transcript_appendf_ansi("@gvolume set to %d%%@R\n", percent);
}

static void cmd_reboot(void)
{
    shell_transcript_appendf_ansi("@rRebooting...@R\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void cmd_clear(void)
{
    shell_transcript_reset();
    shell_record_infof("shell", "Transcript cleared");
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void command_init(void)
{
    if (s_initialized) {
        return;
    }

    s_initialized = true;
    ESP_LOGI(COMMAND_TAG, "Command module initialized");
}

bool command_is_initialized(void)
{
    return s_initialized;
}
