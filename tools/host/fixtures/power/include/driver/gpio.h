/* Host mock for the ESP-IDF GPIO wake API. Mock state lives in mock_state.c. */
#ifndef POWER_GATE_GPIO_H
#define POWER_GATE_GPIO_H
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
#define GPIO_MODE_INPUT       0
#define GPIO_PULLUP_ENABLE    1
#define GPIO_PULLUP_DISABLE   0
#define GPIO_PULLDOWN_ENABLE  1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE     0
#define GPIO_INTR_LOW_LEVEL   1
#define GPIO_INTR_HIGH_LEVEL  2
#define ESP_GPIO_WAKEUP_GPIO_LOW  0
#define ESP_GPIO_WAKEUP_GPIO_HIGH 1
typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_wakeup_enable(gpio_num_t gpio, int intr_type);
esp_err_t gpio_wakeup_disable(gpio_num_t gpio);
#ifdef __cplusplus
}
#endif
#endif
