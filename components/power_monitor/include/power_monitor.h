/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file power_monitor.h
 * @brief Battery fuel-gauge abstraction (Tab5 INA226 pack monitor).
 *
 * The M5Stack Tab5 reports pack voltage/current/power through an INA226 on the
 * shared SYS I2C bus (addr 0x41, 5 mOhm shunt). This module is READ-ONLY: it
 * only configures the INA226 measurement registers and reads them. It never
 * touches the PMIC/charge-enable rails, so it cannot damage the pack.
 *
 * On boards without an INA226 the module compiles to a no-op and
 * `power_monitor_available()` returns false; the ADC path in the command layer
 * still handles those boards.
 */

#ifndef POWER_MONITOR_H
#define POWER_MONITOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Probe and configure the fuel gauge. Idempotent.
 *  Returns ESP_OK when the gauge is present and configured, ESP_ERR_NOT_FOUND
 *  when the board has none / it does not answer, or another error on I2C. */
esp_err_t power_monitor_init(void);

/** True once `power_monitor_init()` found and configured the gauge. */
bool power_monitor_available(void);

/** Pack charge state, inferred from the INA226 current (and voltage). */
typedef enum {
    POWER_MONITOR_CHARGE_UNKNOWN = 0, /*!< No gauge / no reading */
    POWER_MONITOR_CHARGE_CHARGING,    /*!< Current flows into the pack */
    POWER_MONITOR_CHARGE_DISCHARGING, /*!< Current flows out of the pack */
    POWER_MONITOR_CHARGE_IDLE,        /*!< No meaningful current */
    POWER_MONITOR_CHARGE_FULL,        /*!< At/above the full threshold, no current */
} power_monitor_charge_t;

/**
 * Classify the charge state from a sample (pure, unit-tested).
 *
 * @param pack_mv   Pack voltage in millivolts.
 * @param current_ma Signed current: positive flows into the pack.
 */
power_monitor_charge_t power_monitor_classify_charge(int pack_mv, int current_ma);

/** Human-readable charge-state name ("charging", "discharging", ...). */
const char *power_monitor_charge_name(power_monitor_charge_t state);

/**
 * Read one battery sample.
 *
 * @param pack_mv_out  Pack voltage in millivolts (NULL to skip).
 * @param percent_out  State of charge 0..100 from the pack voltage (NULL to
 *                     skip). Reports 0..100; never negative.
 * @param charging_out True when current flows INTO the pack (NULL to skip).
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND/ESP_ERR_INVALID_STATE when no
 *         gauge is available, or an I2C error.
 */
esp_err_t power_monitor_battery_read(int *pack_mv_out, int *percent_out,
                                     bool *charging_out);

/**
 * Read a full battery sample (voltage, state of charge, current, power,
 * charging). Any output pointer may be NULL. Current is signed (positive into
 * the pack); power is the INA226 power register.
 */
esp_err_t power_monitor_read_sample(int *pack_mv_out, int *percent_out,
                                    int *current_ma_out, int *power_mw_out,
                                    bool *charging_out);

/** Raw INA226 registers for diagnostics (`battery diag`). */
typedef struct {
    int bus_mv;       /*!< Bus voltage (1.25 mV/LSB) */
    int shunt_uv;     /*!< Shunt voltage, signed (2.5 uV/LSB) */
    int current_raw;  /*!< Current register, signed */
    int current_ma;   /*!< Current in mA (current_raw * current_lsb) */
    int power_mw;     /*!< Power register in mW */
    uint16_t config;  /*!< CONFIG register */
    uint16_t cal;     /*!< Programmed CALIBRATION register */
} power_monitor_diag_t;

/** Read raw INA226 diagnostics. ESP_ERR_NOT_FOUND when no gauge is available. */
esp_err_t power_monitor_read_diag(power_monitor_diag_t *out);

#ifdef __cplusplus
}
#endif

#endif /* POWER_MONITOR_H */
