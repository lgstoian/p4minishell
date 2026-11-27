/* Host mock for esp_pm.h (CONFIG_PM_ENABLE is on in the gate build). */
#ifndef POWER_GATE_ESP_PM_H
#define POWER_GATE_ESP_PM_H
#include <stdbool.h>
#include "esp_err.h"
#ifndef CONFIG_PM_ENABLE
#define CONFIG_PM_ENABLE 1
#endif
#ifndef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#endif
typedef struct {
    int max_freq_mhz;
    int min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;
esp_err_t esp_pm_configure(const esp_pm_config_t *config);
#endif
