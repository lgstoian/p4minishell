/* SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 * @file mock_state.h
 * @brief Deterministic mock state for tools/host_power_gate.py.
 *
 * This is gate scaffolding, not shipped firmware. It records what the REAL
 * power_commands.c asks the ESP-IDF/shell APIs to do, so the gate can prove
 * arm/disarm balance and honest output without a board.
 */
#ifndef POWER_GATE_MOCK_STATE_H
#define POWER_GATE_MOCK_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "display.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "shell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t now_us;
    int touch_int_gpio;
    int keyboard_int_gpio;
    bool gpio_enabled[64];
    bool gpio_at_sleep[64]; /* snapshot at esp_light_sleep_start, before disarm */
    int gpio_enable_calls;
    int gpio_disable_calls;
    bool gpio_enable_fail;
    bool timer_armed;
    uint64_t timer_us;
    int timer_disable_calls;
    bool gpio_wake_source_enabled;
    int gpio_wake_enable_calls;
    int gpio_wake_disable_source_calls;
    bool deep_gpio_armed;
    uint64_t deep_gpio_mask;
    int light_sleep_calls;
    int deep_sleep_calls;
    esp_err_t light_sleep_result;
    esp_sleep_wakeup_cause_t wake_cause;
    display_power_state_t display_state;
    bool indev_present;
    bool indev_pressed;
    char transcript[16384];
    int errorlevel;
    int pm_light_sleep_enable;
} power_mock_t;

extern power_mock_t g_power_mock;

void mock_reset(void);

/* Shipped functions the harness drives directly (power_commands.c). */
extern int shell_power_clamp_idle_seconds(int64_t seconds);
extern int shell_power_idle_remaining_ms(int timeout_secs, int64_t now_us,
                                         int64_t last_activity_us, bool off_by_idle);
extern bool shell_power_parse_seconds(int argc, char **argv, uint32_t *seconds_out);
extern void shell_power_set_idle_timeout(int seconds);
extern int shell_power_get_idle_timeout(void);
extern int shell_power_ms_until_idle_off(void);
extern void shell_power_notify_activity(void);
extern void shell_power_idle_tick(void);
extern bool shell_power_light_sleep_requested(void);
extern bool shell_power_deep_wake_gpio_eligible(int gpio);
extern const char *shell_power_wake_cause_string(esp_sleep_wakeup_cause_t cause);
extern void shell_command_sleep(int argc, char **argv);
extern void shell_command_deepsleep(int argc, char **argv);
extern void shell_command_power(int argc, char **argv);
extern void shell_command_battery(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
