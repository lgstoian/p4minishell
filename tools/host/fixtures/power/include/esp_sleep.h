/* Host mock for ESP-IDF sleep wake sources. All state is tracked in mock_state.c. */
#ifndef POWER_GATE_ESP_SLEEP_H
#define POWER_GATE_ESP_SLEEP_H
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_TOUCHPAD,
    ESP_SLEEP_WAKEUP_ULP,
    ESP_SLEEP_WAKEUP_GPIO,
    ESP_SLEEP_WAKEUP_UART,
    ESP_SLEEP_WAKEUP_UART1,
    ESP_SLEEP_WAKEUP_UART2,
    ESP_SLEEP_WAKEUP_WIFI,
    ESP_SLEEP_WAKEUP_COCPU,
    ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG,
    ESP_SLEEP_WAKEUP_BT,
    ESP_SLEEP_WAKEUP_USB,
} esp_sleep_wakeup_cause_t;
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_us);
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t source);
esp_err_t esp_sleep_enable_gpio_wakeup(void);
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t gpio_mask, int wake_level);
esp_err_t esp_light_sleep_start(void);
void esp_deep_sleep_start(void);
#ifdef __cplusplus
}
#endif
#endif
