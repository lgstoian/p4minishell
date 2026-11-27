/* Host mock of board_caps.h: no ADC divider, no INA226. */
#ifndef POWER_GATE_BOARD_CAPS_H
#define POWER_GATE_BOARD_CAPS_H
#include <stdbool.h>
bool board_caps_has_battery_adc(void);
bool board_caps_has_ina226(void);
#endif
