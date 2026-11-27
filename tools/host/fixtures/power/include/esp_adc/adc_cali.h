/* Host mock: calibration scheme is not compiled in the gate build. */
#ifndef POWER_GATE_ADC_CALI_H
#define POWER_GATE_ADC_CALI_H
#include "esp_err.h"
typedef void *adc_cali_handle_t;
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *out_mv);
#endif
