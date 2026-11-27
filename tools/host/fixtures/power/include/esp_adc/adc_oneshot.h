/* Host mock: ADC is never initialized in the gate (no ADC battery sense). */
#ifndef POWER_GATE_ADC_ONESHOT_H
#define POWER_GATE_ADC_ONESHOT_H
#include <stdint.h>
#include "esp_err.h"
#include "soc/adc_channel.h"
#define ADC_BITWIDTH_DEFAULT 12
typedef void *adc_oneshot_unit_handle_t;
typedef struct {
    adc_unit_t unit_id;
} adc_oneshot_unit_init_cfg_t;
typedef struct {
    int atten;
    int bitwidth;
} adc_oneshot_chan_cfg_t;
esp_err_t adc_oneshot_io_to_channel(int gpio, adc_unit_t *unit_id, adc_channel_t *channel);
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *out);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t unit, adc_channel_t channel,
                                     const adc_oneshot_chan_cfg_t *cfg);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel, int *out);
#endif
