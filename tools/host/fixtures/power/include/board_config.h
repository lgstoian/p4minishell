/* Host mock of board_config.h - pin values supplied per board by the fixture. */
#ifndef POWER_GATE_BOARD_CONFIG_H
#define POWER_GATE_BOARD_CONFIG_H
#define BOARD_CFG_BATTERY_ADC_GPIO              9
#define BOARD_CFG_BATTERY_DIVIDER_NUMERATOR      2
#define BOARD_CFG_BATTERY_DIVIDER_DENOMINATOR    1
#define BOARD_CFG_BATTERY_PRESENT_MV             3000
#define BOARD_CFG_BATTERY_EMPTY_MV               3300
#define BOARD_CFG_BATTERY_FULL_MV                4200
#ifndef BOARD_CFG_TAB5KBD_INT_GPIO
#define BOARD_CFG_TAB5KBD_INT_GPIO               (-1)
#endif
#endif
