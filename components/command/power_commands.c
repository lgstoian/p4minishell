/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file power_commands.c
 * @brief Display/power/battery verbs (brightness/rotate/battery/power/
 * sleep/deepsleep) for P4MiniShell.
 *
 * Moved verbatim out of command.c in v0.35.4. Owns the battery ADC state
 * and the idle display-off state (portMUX-guarded, touched from several
 * tasks). The ops-table backings (command_battery_read, shell_power_*
 * API) live here too; command_init() in command.c registers them.
 * The single dispatcher in command.c calls these, it never implements them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "display.h"
#include "audio.h"
#include "networking.h"
#include "power_monitor.h"
#include "command.h"
#include "clock.h"
#include "led.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "board_bsp.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "soc/adc_channel.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Compat alias + light-sleep request flag (moved with the power verbs in
 * v0.35.4; SHELL_REBOOT_DELAY_MS is duplicated from command.c, which still
 * uses it for the reboot path). */
#define SHELL_REBOOT_DELAY_MS           P4_CONFIG_REBOOT_DELAY_MS

static bool s_light_sleep_requested;

/* Compat aliases (moved with the power verbs in v0.35.4). */
#define SHELL_BATTERY_ATTEN             P4_CONFIG_BATTERY_ATTEN
#define SHELL_BATTERY_MIN_SLEEP_FREQ_MHZ P4_CONFIG_BATTERY_MIN_SLEEP_FREQ_MHZ
#define SHELL_POWER_SLEEP_DEFAULT_SECS  P4_CONFIG_POWER_SLEEP_DEFAULT_SECS
#define SHELL_POWER_SLEEP_MAX_SECS      P4_CONFIG_POWER_SLEEP_MAX_SECS
#define SHELL_POWER_SLEEP_PRE_DELAY_MS  P4_CONFIG_POWER_SLEEP_PRE_DELAY_MS
#define SHELL_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI P4_CONFIG_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI
#define SHELL_POWER_IDLE_DISPLAY_MAX_SECS  P4_CONFIG_POWER_IDLE_DISPLAY_MAX_SECS
#define SHELL_POWER_WAKE_GPIO              P4_CONFIG_POWER_WAKE_GPIO
#define SHELL_POWER_WAKE_LEVEL             P4_CONFIG_POWER_WAKE_LEVEL

static adc_oneshot_unit_handle_t s_battery_adc_unit;
static adc_channel_t s_battery_adc_channel;
static bool s_battery_adc_ready;
static adc_cali_handle_t s_battery_cali_handle;
static bool s_battery_cali_ready;

/* ========================================================================
 * BATTERY HARDWARE
 * ======================================================================== */

static esp_err_t shell_battery_ensure_adc(void)
{
    if (s_battery_adc_ready) {
        return ESP_OK;
    }

#if !BOARD_CFG_BATTERY_ADC_PRESENT
    /* This board has no ADC battery-sense divider (for example the M5Stack
     * Tab5 uses an INA226 fuel gauge instead). Report the documented N/C state
     * instead of probing an unconnected pin. */
    return ESP_ERR_NOT_FOUND;
#else
    esp_err_t error;
    adc_unit_t unit_id;
    adc_oneshot_unit_init_cfg_t unit_cfg = {0};
    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = SHELL_BATTERY_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    error = adc_oneshot_io_to_channel(BOARD_CFG_BATTERY_ADC_GPIO, &unit_id, &s_battery_adc_channel);
    if (error != ESP_OK) {
        return error;
    }

    unit_cfg.unit_id = unit_id;
    error = adc_oneshot_new_unit(&unit_cfg, &s_battery_adc_unit);
    if (error != ESP_OK) {
        return error;
    }

    error = adc_oneshot_config_channel(s_battery_adc_unit, s_battery_adc_channel, &channel_cfg);
    if (error != ESP_OK) {
        return error;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = unit_id,
            .chan = s_battery_adc_channel,
            .atten = SHELL_BATTERY_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_battery_cali_handle) == ESP_OK) {
            s_battery_cali_ready = true;
        }
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = unit_id,
            .atten = SHELL_BATTERY_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_battery_cali_handle) == ESP_OK) {
            s_battery_cali_ready = true;
        }
    }
#endif

    s_battery_adc_ready = true;
    return ESP_OK;
#endif /* !BOARD_CFG_BATTERY_ADC_PRESENT */
}

esp_err_t command_battery_read(int *battery_mv_out, int *percent_out, int *raw_out, int *gpio_mv_out)
{
    esp_err_t error;
    int raw = 0;
    int gpio_mv = 0;
    int battery_mv;
    int percent;

#if BOARD_CFG_BATTERY_INA226_PRESENT
    /* Fuel-gauge boards (M5Stack Tab5): pack voltage/SoC come from the INA226.
     * Read-only; the ADC divider path below is not wired on these boards. */
    if (power_monitor_available() || power_monitor_init() == ESP_OK) {
        int pack_mv = 0;
        int soc = 0;
        bool charging = false;

        if (power_monitor_battery_read(&pack_mv, &soc, &charging) == ESP_OK) {
            if (pack_mv < BOARD_CFG_BATTERY_PRESENT_MV) {
                /* Gauge present but the pack is absent/near-empty on the rail. */
                return ESP_ERR_NOT_FOUND;
            }
            if (battery_mv_out != NULL) { *battery_mv_out = pack_mv; }
            if (percent_out != NULL)    { *percent_out = soc; }
            if (raw_out != NULL)        { *raw_out = 0; }
            if (gpio_mv_out != NULL)    { *gpio_mv_out = pack_mv; }
            return ESP_OK;
        }
    }
#endif

    error = shell_battery_ensure_adc();
    if (error != ESP_OK) {
        return error;
    }

    error = adc_oneshot_read(s_battery_adc_unit, s_battery_adc_channel, &raw);
    if (error != ESP_OK) {
        return error;
    }

    if (s_battery_cali_ready) {
        error = adc_cali_raw_to_voltage(s_battery_cali_handle, raw, &gpio_mv);
        if (error != ESP_OK) {
            return error;
        }
    } else {
        gpio_mv = (raw * 3300) / 4095;
    }

    battery_mv = (gpio_mv * BOARD_CFG_BATTERY_DIVIDER_NUMERATOR) / BOARD_CFG_BATTERY_DIVIDER_DENOMINATOR;

    /* Publish the raw reading first so callers can still print diagnostics. */
    if (battery_mv_out != NULL) {
        *battery_mv_out = battery_mv;
    }
    if (raw_out != NULL) {
        *raw_out = raw;
    }
    if (gpio_mv_out != NULL) {
        *gpio_mv_out = gpio_mv;
    }

    /* A pack voltage below the "present" floor means no battery / no sense
     * connection: the ADC input floats well under any real pack. Report it as
     * not connected (ESP_ERR_NOT_FOUND) so the header shows "BAT N/C" and the
     * battery verbs print N/C instead of a bogus 0%. */
    if (battery_mv < BOARD_CFG_BATTERY_PRESENT_MV) {
        if (percent_out != NULL) {
            *percent_out = 0;
        }
        return ESP_ERR_NOT_FOUND;
    }

    if (battery_mv <= BOARD_CFG_BATTERY_EMPTY_MV) {
        percent = 0;
    } else if (battery_mv >= BOARD_CFG_BATTERY_FULL_MV) {
        percent = 100;
    } else {
        percent = ((battery_mv - BOARD_CFG_BATTERY_EMPTY_MV) * 100) /
                  (BOARD_CFG_BATTERY_FULL_MV - BOARD_CFG_BATTERY_EMPTY_MV);
    }

    if (percent_out != NULL) {
        *percent_out = percent;
    }

    return ESP_OK;
}

bool command_battery_is_charging(void)
{
#if BOARD_CFG_BATTERY_INA226_PRESENT
    bool charging = false;

    if (power_monitor_available() || power_monitor_init() == ESP_OK) {
        if (power_monitor_battery_read(NULL, NULL, &charging) == ESP_OK) {
            return charging;
        }
    }
#endif
    return false;
}

static void shell_battery_print_usage(void)
{
    shell_print_usage("Usage: battery | battery diag | battery sleep <on|off|status>");
}

/* ========================================================================
 * HARDWARE COMMANDS: brightness, rotate, battery, volume
 * ======================================================================== */

void shell_command_brightness(int argc, char **argv)
{
    esp_err_t error;
    int percent;

    if (argc != 2 || !shell_parse_percentage_arg(argv[1], &percent)) {
        shell_print_usage("Usage: brightness <0-100>");
        shell_record_warningf("brightness", "Usage error for brightness command");
        return;
    }

    error = display_set_brightness(percent);
    if (error != ESP_OK) {
        shell_print_error("brightness: failed to set backlight (%s)", esp_err_to_name(error));
        shell_record_errorf("brightness", error, "Failed to set brightness to %d%%", percent);
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "brightness set to" SH_RST " " SH_NUM "%d%%" SH_RST "\n", percent);
}

void shell_command_rotate(int argc, char **argv)
{
    display_rotation_t rotation;
    esp_err_t error;

    if (argc != 2) {
        shell_print_usage("Usage: rotate <0|90|180|270>");
        shell_record_warningf("rotate", "Usage error for rotate command");
        return;
    }

    error = display_rotation_parse(argv[1], &rotation);
    if (error != ESP_OK) {
        shell_print_usage("Usage: rotate <0|90|180|270>");
        shell_record_warningf("rotate", "Invalid rotation angle: %s", argv[1]);
        return;
    }

    error = display_set_rotation(rotation);
    if (error != ESP_OK) {
        shell_print_error("rotate: failed to apply display rotation (%s)", esp_err_to_name(error));
        shell_record_errorf("rotate", error, "Failed to apply rotation %s", argv[1]);
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "rotation set to" SH_RST " " SH_NUM "%s" SH_RST " " SH_LBL "degrees and GT911 remap updated" SH_RST "\n", argv[1]);
}

/** `battery diag`: raw INA226 registers, for on-device bring-up/verification. */
static void shell_battery_print_diag(void)
{
    power_monitor_diag_t d;

    if (!power_monitor_available() && power_monitor_init() != ESP_OK) {
        shell_print_error("battery: no fuel gauge on this board");
        batch_set_errorlevel(1);
        return;
    }
    if (power_monitor_read_diag(&d) != ESP_OK) {
        shell_print_error("battery: gauge read failed");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_HEAD "Battery gauge (INA226)" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_LBL "bus:" SH_RST " " SH_NUM "%d mV" SH_RST "\n", d.bus_mv);
    shell_transcript_appendf_ansi("  " SH_LBL "shunt:" SH_RST " " SH_NUM "%d uV" SH_RST "\n", d.shunt_uv);
    shell_transcript_appendf_ansi("  " SH_LBL "current:" SH_RST " " SH_NUM "raw=%d" SH_RST " " SH_NUM "%d mA" SH_RST "\n",
                                  d.current_raw, d.current_ma);
    shell_transcript_appendf_ansi("  " SH_LBL "power:" SH_RST " " SH_NUM "%d mW" SH_RST "\n", d.power_mw);
    shell_transcript_appendf_ansi("  " SH_LBL "config:" SH_RST " " SH_NUM "0x%04X" SH_RST " " SH_LBL "cal:" SH_RST
                                  " " SH_NUM "0x%04X" SH_RST "\n", d.config, d.cal);
    batch_set_errorlevel(0);
}

void shell_command_battery(int argc, char **argv)
{
    int battery_mv = 0;
    int percent = 0;
    int raw = 0;
    int gpio_mv = 0;
    esp_err_t error;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "diag")) {
        shell_battery_print_diag();
        return;
    }

    if (argc == 1) {
#if BOARD_CFG_BATTERY_INA226_PRESENT
        /* Fuel-gauge boards report the pack directly (read-only INA226). */
        if (power_monitor_available()) {
            int mv = 0, soc = 0, ma = 0, mw = 0;
            bool charging = false;

            if (power_monitor_read_sample(&mv, &soc, &ma, &mw, &charging) == ESP_OK) {
                bool pack_present = (mv >= BOARD_CFG_BATTERY_PRESENT_MV);
                power_monitor_charge_t charge = power_monitor_classify_charge(mv, ma);

                if (!pack_present) {
                    shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_MUTE "N/C (no pack connected)" SH_RST "\n");
                } else if (charge == POWER_MONITOR_CHARGE_CHARGING) {
                    shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_NUM "%d%%" SH_RST ", " SH_NUM "%d.%03d V" SH_RST " " SH_OK "charging" SH_RST "\n",
                                                  soc, mv / 1000, mv % 1000);
                } else if (charge == POWER_MONITOR_CHARGE_DISCHARGING) {
                    shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_NUM "%d%%" SH_RST ", " SH_NUM "%d.%03d V" SH_RST " " SH_VAL "discharging" SH_RST "\n",
                                                  soc, mv / 1000, mv % 1000);
                } else if (charge == POWER_MONITOR_CHARGE_FULL) {
                    shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_NUM "%d%%" SH_RST ", " SH_NUM "%d.%03d V" SH_RST " " SH_OK "full" SH_RST "\n",
                                                  soc, mv / 1000, mv % 1000);
                } else {
                    shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_NUM "%d%%" SH_RST ", " SH_NUM "%d.%03d V" SH_RST "\n",
                                                  soc, mv / 1000, mv % 1000);
                }
                shell_transcript_appendf_ansi(SH_LBL "battery.gauge:" SH_RST " " SH_LBL "INA226" SH_RST " " SH_NUM "%d.%03d V" SH_RST " " SH_NUM "%d mA" SH_RST " " SH_NUM "%d mW" SH_RST " " SH_LBL "charge=" SH_RST "%s" SH_RST "\n",
                                              mv / 1000, mv % 1000, ma, mw,
                                              pack_present ? power_monitor_charge_name(charge) : "n/a");
#if CONFIG_PM_ENABLE
                if (s_light_sleep_requested) {
                    shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_OK "yes" SH_RST "\n");
                } else {
                    shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_MUTE "no" SH_RST "\n");
                }
#else
                shell_print_muted("battery.sleep: unavailable because CONFIG_PM_ENABLE is off in sdkconfig");
#endif
                return;
            }
        }
#endif
        error = command_battery_read(&battery_mv, &percent, &raw, &gpio_mv);
        if (error == ESP_ERR_NOT_FOUND) {
            /* No battery / sense connection: the documented N/C state. */
            shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_MUTE "N/C (no battery connected)" SH_RST "\n");
            shell_transcript_appendf_ansi(SH_LBL "battery.detail:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "gpio_mv=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "scaled_mv=" SH_RST SH_NUM "%d" SH_RST "\n",
                                     BOARD_CFG_BATTERY_ADC_GPIO,
                                     raw,
                                     gpio_mv,
                                     battery_mv);
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("battery: failed to read ADC on GPIO %d (%s)",
                                     BOARD_CFG_BATTERY_ADC_GPIO,
                                     esp_err_to_name(error));
            shell_record_errorf("battery", error, "Failed to read battery ADC");
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_NUM "%d%%" SH_RST ", " SH_NUM "%d.%03d V" SH_RST "\n", percent, battery_mv / 1000, battery_mv % 1000);
        shell_transcript_appendf_ansi(SH_LBL "battery.detail:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "gpio_mv=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "scaled_mv=" SH_RST SH_NUM "%d" SH_RST "\n",
                                 BOARD_CFG_BATTERY_ADC_GPIO,
                                 raw,
                                 gpio_mv,
                                 battery_mv);
        shell_transcript_appendf_ansi(SH_LBL "calibrated=" SH_RST "%s" SH_RST "\n",
                                 s_battery_cali_ready ? "yes" : "no");
#if CONFIG_PM_ENABLE
        if (s_light_sleep_requested) {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_OK "yes" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_MUTE "no" SH_RST "\n");
        }
#else
        shell_print_muted("battery.sleep: unavailable because CONFIG_PM_ENABLE is off in sdkconfig");
#endif
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "sleep") &&
        shell_text_equals_ignore_case(argv[2], "status")) {
        if (s_light_sleep_requested) {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_OK "yes" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_MUTE "no" SH_RST "\n");
        }
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "sleep")) {
        if (!shell_text_equals_ignore_case(argv[2], "on") && !shell_text_equals_ignore_case(argv[2], "off")) {
            shell_battery_print_usage();
            shell_record_warningf("battery", "Invalid battery sleep argument: %s", argv[2]);
            return;
        }
#if CONFIG_PM_ENABLE
        esp_pm_config_t pm_config = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = SHELL_BATTERY_MIN_SLEEP_FREQ_MHZ,
            .light_sleep_enable = shell_text_equals_ignore_case(argv[2], "on"),
        };

        error = esp_pm_configure(&pm_config);
        if (error != ESP_OK) {
            shell_transcript_appendf("battery.sleep: failed to apply power management (%s)\n", esp_err_to_name(error));
            shell_record_errorf("battery", error, "Failed to update light sleep request");
            return;
        }

        s_light_sleep_requested = pm_config.light_sleep_enable;
        if (s_light_sleep_requested) {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep" SH_RST " " SH_OK "enabled" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep" SH_RST " " SH_MUTE "disabled" SH_RST "\n");
        }
#else
        shell_print_muted("battery.sleep: unavailable because CONFIG_PM_ENABLE is off in sdkconfig");
#endif
        return;
    }

    shell_battery_print_usage();
    shell_record_warningf("battery", "Usage error for battery command");
}

/* ========================================================================
 * POWER COMMANDS: power, sleep, deepsleep
 *
 * `power`     reports power-management state (PM, light sleep request,
 *             display, Wi-Fi, battery, last wake cause).
 * `sleep`     enters light sleep: RAM is retained and the shell resumes
 *             on wake with all state intact.
 * `deepsleep` enters deep sleep: RAM is lost and the device reboots on
 *             wake (same path as `reboot`).
 *
 * Battery telemetry goes through command_battery_read() (the same ADC path
 * the `battery` command and header use), and Wi-Fi/hosted state is torn down
 * through networking_wifi_shutdown() (the same path C6 OTA uses).
 * ======================================================================== */

/** Pure wake-cause enum->string (unit-tested). */
const char *shell_power_wake_cause_string(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:             return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1:             return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER:            return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:         return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:              return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO:             return "gpio";
        case ESP_SLEEP_WAKEUP_UART:             return "uart";
        case ESP_SLEEP_WAKEUP_UART1:            return "uart1";
        case ESP_SLEEP_WAKEUP_UART2:            return "uart2";
        case ESP_SLEEP_WAKEUP_WIFI:             return "wifi";
        case ESP_SLEEP_WAKEUP_COCPU:            return "cocpu";
        case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:  return "cocpu-trap";
        case ESP_SLEEP_WAKEUP_BT:               return "bluetooth";
        case ESP_SLEEP_WAKEUP_USB:              return "usb";
        default:                                return "none";
    }
}

/** Read and print the battery through the shared ADC path. */
static bool shell_power_report_battery(const char *label)
{
    int battery_mv = 0;
    int percent = 0;
    int raw = 0;
    int gpio_mv = 0;
    esp_err_t error;

    error = command_battery_read(&battery_mv, &percent, &raw, &gpio_mv);
    if (error == ESP_ERR_NOT_FOUND) {
        /* No battery / sense connection: the documented N/C state. */
        shell_transcript_appendf_ansi(SH_LBL "%s:" SH_RST " " SH_LBL "battery" SH_RST " " SH_MUTE "N/C" SH_RST "\n",
                                 label);
        return false;
    }
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_LBL "%s:" SH_RST " " SH_LBL "battery" SH_RST " " SH_MUTE "unavailable" SH_RST
                                 " (" SH_WARN "%s" SH_RST ")\n",
                                 label, esp_err_to_name(error));
        return false;
    }
    shell_transcript_appendf_ansi(SH_LBL "%s:" SH_RST " " SH_LBL "battery" SH_RST " " SH_NUM "%d%%" SH_RST
                             " (" SH_NUM "%d.%03d V" SH_RST ")\n",
                             label, percent, battery_mv / 1000, battery_mv % 1000);
    return true;
}

/**
 * Parse an optional duration argument in seconds. With no argument the
 * configured default is used; out-of-range values are clamped so a typo
 * cannot put the board to sleep for days.
 */
/** Pure argv->seconds parser (unit-tested): default, clamp, reject. */
bool shell_power_parse_seconds(int argc, char **argv, uint32_t *seconds_out)
{
    char *end;
    long value;

    if (argc < 2) {
        *seconds_out = SHELL_POWER_SLEEP_DEFAULT_SECS;
        return true;
    }
    value = strtol(argv[1], &end, 10);
    if (*end != '\0' || value < 0) {
        return false;
    }
    if (value > SHELL_POWER_SLEEP_MAX_SECS) {
        value = SHELL_POWER_SLEEP_MAX_SECS;
    }
    *seconds_out = (uint32_t)value;
    return true;
}

/** Tear down Wi-Fi/hosted state so the radio cannot keep the SoC awake. */
static esp_err_t shell_power_shutdown_wifi(void)
{
    esp_err_t error = networking_wifi_shutdown();

    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_LBL "power:" SH_RST " " SH_LBL "wifi shutdown failed" SH_RST
                                 " (" SH_WARN "%s" SH_RST ")\n",
                                 esp_err_to_name(error));
    }
    return error;
}

/* ========================================================================
 * IDLE DISPLAY-OFF (`power idle`) + ACTIVITY WAKE
 * ========================================================================
 * After the configured idle timeout without user input the display backlight
 * is switched off; the next touch, USB keyboard/mouse, or serial command wakes
 * it again. Idle-off only drops the backlight (`display_set_power_state`), so
 * the panel, GT911, and USB host keep running and the shell state is
 * untouched - waking is a clean backlight-on plus a header notification.
 */

static portMUX_TYPE s_power_idle_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_power_idle_off_secs;         /* 0 = disabled */
static int64_t s_power_last_activity_us;  /* esp_timer_get_time() */
static bool s_power_display_off_by_idle;

void shell_power_set_idle_timeout(int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds > SHELL_POWER_IDLE_DISPLAY_MAX_SECS) {
        seconds = SHELL_POWER_IDLE_DISPLAY_MAX_SECS;
    }
    portENTER_CRITICAL(&s_power_idle_lock);
    s_power_idle_off_secs = seconds;
    portEXIT_CRITICAL(&s_power_idle_lock);
}

int shell_power_get_idle_timeout(void)
{
    int seconds;

    portENTER_CRITICAL(&s_power_idle_lock);
    seconds = s_power_idle_off_secs;
    portEXIT_CRITICAL(&s_power_idle_lock);
    return seconds;
}

int shell_power_ms_until_idle_off(void)
{
    int seconds;
    int64_t last_us;
    int64_t elapsed_ms;
    int64_t remaining_ms;

    portENTER_CRITICAL(&s_power_idle_lock);
    seconds = s_power_idle_off_secs;
    last_us = s_power_last_activity_us;
    if (s_power_display_off_by_idle) {
        seconds = 0;
    }
    portEXIT_CRITICAL(&s_power_idle_lock);

    if (seconds <= 0) {
        return 0;
    }

    elapsed_ms = (esp_timer_get_time() - last_us) / 1000;
    remaining_ms = (int64_t)seconds * 1000 - elapsed_ms;
    if (remaining_ms <= 0) {
        /* Deadline already passed: return a tiny value so the caller polls
         * promptly and runs the idle tick that switches the display off. */
        return 1;
    }
    /* Clamp to the maximum configurable window so the return value (int) is
     * always representable. */
    if (remaining_ms > (int64_t)SHELL_POWER_IDLE_DISPLAY_MAX_SECS * 1000) {
        remaining_ms = (int64_t)SHELL_POWER_IDLE_DISPLAY_MAX_SECS * 1000;
    }
    return (int)remaining_ms;
}

void shell_power_notify_activity(void)
{
    bool wake = false;

    portENTER_CRITICAL(&s_power_idle_lock);
    s_power_last_activity_us = esp_timer_get_time();
    if (s_power_display_off_by_idle) {
        s_power_display_off_by_idle = false;
        wake = true;
    }
    portEXIT_CRITICAL(&s_power_idle_lock);

    if (wake) {
        /* Route through the transcript (the normal, safe output path) rather
         * than an async header render, so waking never perturbs LVGL layout. */
        display_set_power_state(DISPLAY_POWER_ON);
        shell_transcript_appendf_ansi(SH_MUTE "power: display on\n" SH_RST);
    }
}

void shell_power_idle_tick(void)
{
    int seconds;
    int64_t now_us = esp_timer_get_time();
    int64_t last_us;
    bool off_by_idle;
    bool do_off = false;

    portENTER_CRITICAL(&s_power_idle_lock);
    seconds = s_power_idle_off_secs;
    last_us = s_power_last_activity_us;
    off_by_idle = s_power_display_off_by_idle;
    portEXIT_CRITICAL(&s_power_idle_lock);

    if (display_get_power_state() == DISPLAY_POWER_ON && !off_by_idle) {
        if (seconds > 0 && (now_us - last_us) >= (int64_t)seconds * 1000000LL) {
            do_off = true;
        }
    } else if (off_by_idle) {
        /* The display is off because of idle: a fresh touch press is activity.
         * This runs on the LVGL task, so indev access is safe. */
        lv_indev_t *indev = NULL;

        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                shell_power_notify_activity();
                break;
            }
        }
    }

    if (do_off) {
        portENTER_CRITICAL(&s_power_idle_lock);
        s_power_display_off_by_idle = true;
        portEXIT_CRITICAL(&s_power_idle_lock);
        display_set_power_state(DISPLAY_POWER_SLEEP);
        shell_transcript_appendf_ansi(SH_MUTE "power: display off (idle)\n" SH_RST);
    }
}

/** Configure a user-wired GPIO to wake light/deep sleep, if one is set. */
static void shell_power_enable_gpio_wake(void)
{
    int wake_gpio = (int)SHELL_POWER_WAKE_GPIO;

    if (wake_gpio < 0) {
        return;
    }

    {
        gpio_config_t wake_io;

        memset(&wake_io, 0, sizeof(wake_io));
        wake_io.pin_bit_mask = 1ULL << wake_gpio;
        wake_io.mode = GPIO_MODE_INPUT;
        wake_io.pull_up_en = (SHELL_POWER_WAKE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        wake_io.pull_down_en = (SHELL_POWER_WAKE_LEVEL == 1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        wake_io.intr_type = GPIO_INTR_DISABLE;

        gpio_config(&wake_io);
        if (gpio_wakeup_enable((gpio_num_t)wake_gpio,
                               SHELL_POWER_WAKE_LEVEL ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL) == ESP_OK) {
            (void)esp_sleep_enable_gpio_wakeup();
            shell_transcript_appendf_ansi(SH_LBL "sleep:" SH_RST " GPIO wake on " SH_NUM "GPIO%d" SH_RST
                                     " (" SH_VAL "%s" SH_RST ")\n",
                                     wake_gpio,
                                     SHELL_POWER_WAKE_LEVEL ? "high" : "low");
        } else {
            shell_print_warning("sleep: failed to enable GPIO wake on GPIO%d", wake_gpio);
        }
    }
}

/** Report the touch-wake situation honestly before entering light sleep. */
static void shell_power_report_wake_capability(void)
{
#if BOARD_CFG_LCD_TOUCH_INT_GPIO == GPIO_NUM_NC
    shell_transcript_appendf_ansi(SH_MUTE "sleep: touch wake unavailable (GT911 INT is not wired on this board)\n");
    shell_transcript_appendf_ansi(SH_MUTE "       set P4_CONFIG_POWER_WAKE_GPIO for a button/switch, or use `power idle`\n");
#else
    shell_transcript_appendf_ansi(SH_MUTE "sleep: touch wake available (GT911 INT on GPIO%d)\n",
                             (int)BOARD_CFG_LCD_TOUCH_INT_GPIO);
#endif
}

void shell_command_power(int argc, char **argv)
{
    esp_sleep_wakeup_cause_t wake_cause;
    display_power_state_t display_state;

    /* `power idle <seconds|off>` configures the idle display-off timeout;
     * `power idle` prints it. */
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "idle")) {
        if (argc >= 3) {
            if (shell_text_equals_ignore_case(argv[2], "off")) {
                shell_power_set_idle_timeout(0);
            } else {
                char *end = NULL;
                long parsed = strtol(argv[2], &end, 10);

                if (*end != '\0' || parsed < 0 || parsed > SHELL_POWER_IDLE_DISPLAY_MAX_SECS) {
                    shell_print_error("power: invalid idle timeout %s (0..%d, or `off`)",
                                      argv[2], SHELL_POWER_IDLE_DISPLAY_MAX_SECS);
                    shell_record_warningf("power", "Invalid power idle argument: %s", argv[2]);
                    batch_set_errorlevel(2);
                    return;
                }
                shell_power_set_idle_timeout((int)parsed);
            }
            shell_power_notify_activity();
            if (shell_power_get_idle_timeout() > 0) {
                shell_transcript_appendf_ansi(SH_LBL "power.idle:" SH_RST " display off after " SH_NUM "%d" SH_RST " s\n",
                                         shell_power_get_idle_timeout());
            } else {
                shell_transcript_appendf_ansi(SH_LBL "power.idle:" SH_RST " " SH_MUTE "display idle-off disabled" SH_RST "\n");
            }
            batch_set_errorlevel(0);
            return;
        }
        if (shell_power_get_idle_timeout() > 0) {
            shell_transcript_appendf_ansi(SH_LBL "power.idle:" SH_RST " display off after " SH_NUM "%d" SH_RST " s\n",
                                     shell_power_get_idle_timeout());
        } else {
            shell_transcript_appendf_ansi(SH_LBL "power.idle:" SH_RST " " SH_MUTE "display idle-off disabled" SH_RST "\n");
        }
        batch_set_errorlevel(0);
        return;
    }

    if (argc >= 2 && !shell_text_equals_ignore_case(argv[1], "status")) {
        shell_print_usage("Usage: power [status] | power idle [seconds|off]");
        shell_record_warningf("power", "Usage error for power command");
        batch_set_errorlevel(2);
        return;
    }
    batch_set_errorlevel(0);

    shell_power_report_battery("power");

    shell_transcript_appendf_ansi(SH_LBL "power.pm:" SH_RST " ");
#if CONFIG_PM_ENABLE
    shell_transcript_appendf_ansi(SH_OK "enabled" SH_RST " ");
    if (s_light_sleep_requested) {
        shell_transcript_appendf_ansi(SH_LBL "light sleep" SH_RST " " SH_OK "requested" SH_RST "\n");
    } else {
        shell_transcript_appendf_ansi(SH_LBL "light sleep" SH_RST " " SH_MUTE "off" SH_RST "\n");
    }
#else
    shell_transcript_appendf_ansi(SH_MUTE "disabled" SH_RST " (light sleep needs CONFIG_PM_ENABLE)\n");
#endif

    display_state = display_get_power_state();
    shell_transcript_appendf_ansi(SH_LBL "power.display:" SH_RST " ");
    if (display_state == DISPLAY_POWER_ON) {
        shell_transcript_appendf_ansi(SH_OK "on" SH_RST "\n");
    } else if (display_state == DISPLAY_POWER_SLEEP) {
        shell_transcript_appendf_ansi(SH_WARN "sleep" SH_RST "\n");
    } else {
        shell_transcript_appendf_ansi(SH_ERR "off" SH_RST "\n");
    }

    if (shell_power_get_idle_timeout() > 0) {
        shell_transcript_appendf_ansi(SH_LBL "power.idle_off:" SH_RST " " SH_NUM "%d" SH_RST " s\n",
                                 shell_power_get_idle_timeout());
    } else {
        shell_transcript_appendf_ansi(SH_LBL "power.idle_off:" SH_RST " " SH_MUTE "off" SH_RST "\n");
    }
    if (SHELL_POWER_WAKE_GPIO != GPIO_NUM_NC) {
        shell_transcript_appendf_ansi(SH_LBL "power.wake_gpio:" SH_RST " " SH_NUM "GPIO%d" SH_RST "\n",
                                 (int)SHELL_POWER_WAKE_GPIO);
    }

    shell_transcript_appendf_ansi(SH_LBL "power.wifi:" SH_RST " " SH_NUM "%s" SH_RST "\n",
                             networking_wifi_is_connected() ? "connected" : "down");

    wake_cause = esp_sleep_get_wakeup_cause();
    shell_transcript_appendf_ansi(SH_LBL "power.wake:" SH_RST " " SH_NUM "%s" SH_RST "\n",
                             shell_power_wake_cause_string(wake_cause));
    shell_transcript_appendf_ansi(SH_MUTE "Tip: `battery sleep on` enables automatic light sleep when idle.\n");
    shell_transcript_appendf_ansi(SH_MUTE "Tip: `power idle 60` turns the display off after 60 s of inactivity.\n");
}

/** Clear the configured GPIO wake so a still-active level cannot re-trigger. */
static void shell_power_disable_gpio_wake(void)
{
    int wake_gpio = (int)SHELL_POWER_WAKE_GPIO;

    if (wake_gpio >= 0) {
        (void)gpio_wakeup_disable((gpio_num_t)wake_gpio);
    }
}

void shell_command_sleep(int argc, char **argv)
{
    uint32_t seconds;
    esp_err_t error;
    networking_wifi_restore_state_t wifi_restore;
    bool wifi_restore_needed = false;

    memset(&wifi_restore, 0, sizeof(wifi_restore));
    if (!shell_power_parse_seconds(argc, argv, &seconds)) {
        shell_print_usage("Usage: sleep [seconds]");
        shell_record_warningf("sleep", "Usage error for sleep command");
        return;
    }

    shell_power_report_battery("sleep");

    if (seconds == 0) {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        shell_transcript_appendf_ansi(SH_WARN "sleep: no timer set; wake only from an external wake source" SH_RST "\n");
    } else {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
        shell_transcript_appendf_ansi(SH_LBL "sleep:" SH_RST " timer wake in " SH_NUM "%u" SH_RST " s\n",
                                 (unsigned)seconds);
    }

    /* Wake capability: the GT911 INT line is not wired on this board, so
     * report that honestly and offer the configured GPIO wake as the
     * external alternative. */
    shell_power_report_wake_capability();
    shell_power_enable_gpio_wake();

#if SHELL_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI
    /* Remember the runtime/connection intent so the wake path can bring the
     * radio back without a manual `wifi connect` (capture must precede the
     * shutdown, which clears the cached target credentials). */
    networking_wifi_capture_restore_state(&wifi_restore);
    wifi_restore_needed = wifi_restore.should_restore_runtime;
    shell_power_shutdown_wifi();
#else
    shell_transcript_appendf_ansi(SH_MUTE "sleep: keeping Wi-Fi state (light sleep wifi shutdown disabled)\n");
#endif

    display_set_power_state(DISPLAY_POWER_OFF);
    audio_stop();

    /* Give the transcript and LVGL task time to paint before sleeping. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_POWER_SLEEP_PRE_DELAY_MS));

    error = esp_light_sleep_start();
    shell_power_disable_gpio_wake();
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_ERR "sleep: light sleep failed" SH_RST " (" SH_WARN "%s" SH_RST ")\n",
                                 esp_err_to_name(error));
    } else {
        shell_transcript_appendf_ansi(SH_LBL "sleep:" SH_RST " woke up (" SH_LBL "cause" SH_RST "=" SH_NUM "%s" SH_RST ")\n",
                                 shell_power_wake_cause_string(esp_sleep_get_wakeup_cause()));
    }

    display_set_power_state(DISPLAY_POWER_ON);
    shell_power_notify_activity();

#if SHELL_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI
    if (wifi_restore_needed) {
        /* Bring the radio back automatically (line-current palmtop behaviour)
         * instead of leaving the user to run `wifi connect` by hand. */
        networking_wifi_request_wake_restore(&wifi_restore);
        shell_transcript_appendf_ansi(SH_MUTE "sleep: restoring Wi-Fi in the background\n");
    } else {
        shell_transcript_appendf_ansi(SH_MUTE "sleep: Wi-Fi was shut down; use `wifi connect` to reconnect.\n");
    }
#endif
}

void shell_command_deepsleep(int argc, char **argv)
{
    uint32_t seconds;

    if (!shell_power_parse_seconds(argc, argv, &seconds)) {
        shell_print_usage("Usage: deepsleep [seconds]");
        shell_record_warningf("deepsleep", "Usage error for deepsleep command");
        return;
    }

    shell_power_report_battery("deepsleep");

    shell_transcript_appendf_ansi(SH_WARN "deepsleep: RAM state (env, aliases, cwd, variables) is lost on wake" SH_RST "\n");
    if (seconds == 0) {
        shell_transcript_appendf_ansi(SH_WARN "deepsleep: no timer set; wake requires an external wake source" SH_RST "\n");
    } else {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
        shell_transcript_appendf_ansi(SH_LBL "deepsleep:" SH_RST " timer wake in " SH_NUM "%u" SH_RST " s\n",
                                 (unsigned)seconds);
    }

    shell_power_shutdown_wifi();
    display_set_power_state(DISPLAY_POWER_OFF);
    audio_stop();

    /* Let the transcript and LVGL task paint before the chip resets. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_POWER_SLEEP_PRE_DELAY_MS));
    shell_transcript_appendf_ansi(SH_LBL "deepsleep:" SH_RST " entering deep sleep\n");
    vTaskDelay(pdMS_TO_TICKS(SHELL_REBOOT_DELAY_MS));

    esp_deep_sleep_start();
}

void shell_command_shutdown(int argc, char **argv)
{
    esp_err_t error;

    (void)argc;
    (void)argv;

    shell_transcript_appendf_ansi(SH_ERR "Shutting down..." SH_RST "\n");
    shell_power_report_battery("shutdown");

    /* Persist the state that would otherwise be lost (recall history is
     * debounced; the wall-time anchor seeds the next boot). */
    command_history_save_now();
    clock_rtc_anchor_now();

    /* Best-effort: stop the radio, blank the panel/audio, and darken every
     * status LED so the board does not glow after the rails drop. */
    shell_power_shutdown_wifi();
    display_set_power_state(DISPLAY_POWER_OFF);
    audio_stop();
    (void)led_all_off();

    /* Let the transcript, the I2C LED write, and the SD flush settle. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_POWER_SLEEP_PRE_DELAY_MS));

    shell_transcript_appendf_ansi(SH_LBL "shutdown:" SH_RST " cutting power\n");
    vTaskDelay(pdMS_TO_TICKS(SHELL_REBOOT_DELAY_MS));

    error = board_bsp_poweroff();
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_WARN "shutdown: power latch failed (%s)" SH_RST "\n",
                                      esp_err_to_name(error));
    }

    /* Reached only when the board has no working power latch: idle as low as we
     * can instead of rebooting back into a running system. */
    shell_transcript_appendf_ansi(SH_WARN "shutdown: no hardware power latch; entering deep sleep" SH_RST "\n");
    vTaskDelay(pdMS_TO_TICKS(SHELL_REBOOT_DELAY_MS));
    esp_deep_sleep_start();
}
