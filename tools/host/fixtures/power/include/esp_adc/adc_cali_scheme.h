/* Host mock: scheme structs only (no scheme support compiled). */
#ifndef POWER_GATE_ADC_CALI_SCHEME_H
#define POWER_GATE_ADC_CALI_SCHEME_H
#include "soc/adc_channel.h"
typedef struct {
    adc_unit_t unit_id;
    adc_channel_t chan;
    int atten;
    int bitwidth;
} adc_cali_curve_fitting_config_t;
typedef struct {
    adc_unit_t unit_id;
    int atten;
    int bitwidth;
} adc_cali_line_fitting_config_t;
#endif
