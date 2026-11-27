/* SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 * @file mock_state.c
 * @brief Deterministic mocks for the host power gate. Gate scaffolding only.
 */
#include "mock_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "adc_cali.h"
#include "adc_oneshot.h"
#include "audio.h"
#include "batch.h"
#include "board_bsp.h"
#include "board_caps.h"
#include "clock.h"
#include "command.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "led.h"
#include "networking.h"
#include "power_monitor.h"

power_mock_t g_power_mock;

static lv_indev_t s_mock_indev = { 0 };

void mock_reset(void)
{
    memset(&g_power_mock, 0, sizeof(g_power_mock));
    g_power_mock.touch_int_gpio = -1;
    g_power_mock.keyboard_int_gpio = -1;
    g_power_mock.display_state = DISPLAY_POWER_ON;
    g_power_mock.light_sleep_result = ESP_OK;
    g_power_mock.wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
}

/* ---- deterministic time / shell transcript ---- */

int64_t esp_timer_get_time(void) { return g_power_mock.now_us; }

static void mock_append(const char *prefix, const char *fmt, va_list args)
{
    size_t used = strlen(g_power_mock.transcript);
    size_t room = sizeof(g_power_mock.transcript) - used - 1;

    if (room == 0) {
        return;
    }
    if (prefix != NULL) {
        snprintf(g_power_mock.transcript + used, room, "%s", prefix);
        used = strlen(g_power_mock.transcript);
        room = sizeof(g_power_mock.transcript) - used - 1;
    }
    if (room == 0) {
        return;
    }
    (void)vsnprintf(g_power_mock.transcript + used, room, fmt, args);
}

void shell_transcript_appendf_ansi(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mock_append(NULL, fmt, args);
    va_end(args);
}

void shell_transcript_appendf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mock_append(NULL, fmt, args);
    va_end(args);
}

void shell_print_usage(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mock_append("[usage] ", fmt, args);
    va_end(args);
}

void shell_print_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mock_append("[error] ", fmt, args);
    va_end(args);
}

void shell_print_warning(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mock_append("[warn] ", fmt, args);
    va_end(args);
}

void shell_print_muted(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mock_append(NULL, fmt, args);
    va_end(args);
}

void shell_record_warningf(const char *domain, const char *fmt, ...)
{
    (void)domain;
    (void)fmt;
}

void shell_record_errorf(const char *domain, esp_err_t error, const char *fmt, ...)
{
    (void)domain;
    (void)error;
    (void)fmt;
}

bool shell_parse_percentage_arg(const char *text, int *percentage_out)
{
    (void)text;
    (void)percentage_out;
    return false;
}

bool shell_text_equals_ignore_case(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcasecmp(left, right) == 0;
}

void batch_set_errorlevel(int level) { g_power_mock.errorlevel = level; }

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "mock-error";
}

/* ---- display / LVGL indev ---- */

esp_err_t display_set_brightness(int percent) { (void)percent; return ESP_OK; }
esp_err_t display_rotation_parse(const char *str, display_rotation_t *out)
{
    (void)str;
    if (out != NULL) { *out = DISPLAY_ROTATION_0; }
    return ESP_OK;
}
esp_err_t display_set_rotation(display_rotation_t rotation) { (void)rotation; return ESP_OK; }
esp_err_t display_set_power_state(display_power_state_t state)
{
    g_power_mock.display_state = state;
    return ESP_OK;
}
display_power_state_t display_get_power_state(void) { return g_power_mock.display_state; }
int display_get_touch_int_gpio(void) { return g_power_mock.touch_int_gpio; }

lv_indev_t *lv_indev_get_next(lv_indev_t *indev)
{
    if (indev == NULL && g_power_mock.indev_present) {
        return &s_mock_indev;
    }
    return NULL;
}

int lv_indev_get_state(lv_indev_t *indev)
{
    (void)indev;
    return g_power_mock.indev_pressed ? LV_INDEV_STATE_PRESSED : 0;
}

/* ---- GPIO / sleep wake sources ---- */

void vTaskDelay(uint32_t ticks) { (void)ticks; }

esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }

esp_err_t gpio_wakeup_enable(gpio_num_t gpio, int intr_type)
{
    (void)intr_type;
    g_power_mock.gpio_enable_calls++;
    if (g_power_mock.gpio_enable_fail) {
        return ESP_FAIL;
    }
    if (gpio >= 0 && gpio < 64) {
        g_power_mock.gpio_enabled[gpio] = true;
    }
    return ESP_OK;
}

esp_err_t gpio_wakeup_disable(gpio_num_t gpio)
{
    g_power_mock.gpio_disable_calls++;
    if (gpio >= 0 && gpio < 64) {
        g_power_mock.gpio_enabled[gpio] = false;
    }
    return ESP_OK;
}

esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_us)
{
    g_power_mock.timer_armed = true;
    g_power_mock.timer_us = time_us;
    return ESP_OK;
}

esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t source)
{
    if (source == ESP_SLEEP_WAKEUP_TIMER) {
        g_power_mock.timer_armed = false;
        g_power_mock.timer_disable_calls++;
    } else if (source == ESP_SLEEP_WAKEUP_GPIO) {
        g_power_mock.gpio_wake_source_enabled = false;
        g_power_mock.gpio_wake_disable_source_calls++;
    }
    return ESP_OK;
}

esp_err_t esp_sleep_enable_gpio_wakeup(void)
{
    g_power_mock.gpio_wake_source_enabled = true;
    g_power_mock.gpio_wake_enable_calls++;
    return ESP_OK;
}

esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t gpio_mask, int wake_level)
{
    (void)wake_level;
    g_power_mock.deep_gpio_mask = gpio_mask;
    g_power_mock.deep_gpio_armed = (gpio_mask != 0);
    return ESP_OK;
}

esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) { return g_power_mock.wake_cause; }

esp_err_t esp_light_sleep_start(void)
{
    g_power_mock.light_sleep_calls++;
    memcpy(g_power_mock.gpio_at_sleep, g_power_mock.gpio_enabled,
           sizeof(g_power_mock.gpio_at_sleep));
    return g_power_mock.light_sleep_result;
}

void esp_deep_sleep_start(void) { g_power_mock.deep_sleep_calls++; }

esp_err_t esp_pm_configure(const esp_pm_config_t *config)
{
    g_power_mock.pm_light_sleep_enable = config->light_sleep_enable;
    return ESP_OK;
}

/* ---- peripherals the power path reports but the gate does not emulate ---- */

bool board_caps_has_battery_adc(void) { return false; }
bool board_caps_has_ina226(void) { return false; }
esp_err_t power_monitor_init(void) { return ESP_ERR_NOT_FOUND; }
bool power_monitor_available(void) { return false; }
power_monitor_charge_t power_monitor_classify_charge(int pack_mv, int current_ma)
{
    (void)pack_mv; (void)current_ma; return POWER_MONITOR_CHARGE_UNKNOWN;
}
const char *power_monitor_charge_name(power_monitor_charge_t state)
{
    (void)state; return "unknown";
}
bool power_monitor_pack_present(int pack_mv, int current_ma, const int *recent_mv,
                                int recent_count, int chg_stat, int chg_stat_prev)
{
    (void)pack_mv; (void)current_ma; (void)recent_mv;
    (void)recent_count; (void)chg_stat; (void)chg_stat_prev;
    return false;
}
esp_err_t power_monitor_battery_read(int *pack_mv_out, int *percent_out, bool *charging_out)
{
    (void)pack_mv_out; (void)percent_out; (void)charging_out; return ESP_ERR_NOT_FOUND;
}
esp_err_t power_monitor_read_sample(int *pack_mv_out, int *percent_out,
                                    int *current_ma_out, int *power_mw_out,
                                    bool *charging_out)
{
    (void)pack_mv_out; (void)percent_out; (void)current_ma_out;
    (void)power_mw_out; (void)charging_out; return ESP_ERR_NOT_FOUND;
}
esp_err_t power_monitor_read_diag(power_monitor_diag_t *out)
{
    (void)out; return ESP_ERR_NOT_FOUND;
}

esp_err_t adc_oneshot_io_to_channel(int gpio, adc_unit_t *unit_id, adc_channel_t *channel)
{
    (void)gpio; (void)unit_id; (void)channel; return ESP_ERR_NOT_FOUND;
}
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg,
                               adc_oneshot_unit_handle_t *out)
{
    (void)cfg; (void)out; return ESP_ERR_NOT_FOUND;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t unit, adc_channel_t channel,
                                     const adc_oneshot_chan_cfg_t *cfg)
{
    (void)unit; (void)channel; (void)cfg; return ESP_ERR_NOT_FOUND;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel, int *out)
{
    (void)unit; (void)channel; (void)out; return ESP_ERR_NOT_FOUND;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *out_mv)
{
    (void)handle; (void)raw; (void)out_mv; return ESP_ERR_NOT_FOUND;
}

bool networking_wifi_is_connected(void) { return false; }
esp_err_t networking_wifi_shutdown(void) { return ESP_OK; }
void networking_wifi_capture_restore_state(networking_wifi_restore_state_t *out)
{
    if (out != NULL) { out->should_restore_runtime = false; out->mock_field = 0; }
}
void networking_wifi_request_wake_restore(const networking_wifi_restore_state_t *state)
{
    (void)state;
}

void audio_stop(void) {}
void command_history_save_now(void) {}
void clock_rtc_anchor_now(void) {}
esp_err_t led_all_off(void) { return ESP_OK; }
esp_err_t board_bsp_poweroff(void) { return ESP_OK; }
