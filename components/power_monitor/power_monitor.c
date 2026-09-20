/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file power_monitor.c
 * @brief INA226 battery fuel gauge (M5Stack Tab5 pack monitor), read-only.
 *
 * Protocol from the TI INA226 datasheet (SBOS547) and M5Stack's
 * power_monitor_ina226 driver: 16-bit big-endian registers, 1.25 mV/LSB bus
 * voltage, 2.5 uV/LSB shunt voltage, current = signed * current_lsb with
 * current_lsb = max_current/32768 and CAL = 0.00512/(current_lsb * shunt).
 *
 * SAFETY: this driver only touches the INA226's own CONFIG/CALIBRATION
 * registers and reads its measurement registers. It never writes the PMIC or
 * the charge-enable rails, so it cannot over-charge or damage the pack.
 */

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#include "power_monitor.h"
#include "board_config.h"
#include "board_bsp.h"
#include "p4minishell_config.h"

#define POWER_MONITOR_TAG "power_monitor"

/* INA226 register map. */
#define INA226_REG_CONFIG      0x00
#define INA226_REG_SHUNT_VOLT  0x01
#define INA226_REG_BUS_VOLT    0x02
#define INA226_REG_POWER       0x03
#define INA226_REG_CURRENT     0x04
#define INA226_REG_CALIBRATION 0x05
#define INA226_REG_MFR_ID      0xFE
#define INA226_REG_DIE_ID      0xFF

#define INA226_MFR_ID_VALUE    0x5449u   /* "TI" */
#define INA226_DIE_ID_VALUE    0x2260u

/* AVG=16 samples, 1.1 ms bus/shunt conversion, continuous shunt+bus. */
#define INA226_CONFIG_VALUE    0x4527u

/* Signed-current deadband (mA) that separates charging/discharging from idle.
 * Positive current flows INTO the pack (matches the M5Stack reference). */
#ifndef BOARD_CFG_BATTERY_CHARGE_CURRENT_MA
#define BOARD_CFG_BATTERY_CHARGE_CURRENT_MA 20
#endif
#ifndef BOARD_CFG_BATTERY_FULL_MV
#define BOARD_CFG_BATTERY_FULL_MV 0
#endif

power_monitor_charge_t power_monitor_classify_charge(int pack_mv, int current_ma)
{
    if (current_ma <= -(int)BOARD_CFG_BATTERY_CHARGE_CURRENT_MA) {
        return POWER_MONITOR_CHARGE_DISCHARGING;
    }
#if BOARD_CFG_BATTERY_FULL_MV > 0
    /* At/above the full threshold with no meaningful current: charged. */
    if (current_ma < (int)BOARD_CFG_BATTERY_CHARGE_CURRENT_MA &&
        pack_mv >= (int)BOARD_CFG_BATTERY_FULL_MV) {
        return POWER_MONITOR_CHARGE_FULL;
    }
#else
    (void)pack_mv;
#endif
    /* Not discharging (current flows in, or external power holds the pack at a
     * standstill): the vendor UI treats this as "charging". */
    return POWER_MONITOR_CHARGE_CHARGING;
}

const char *power_monitor_charge_name(power_monitor_charge_t state)
{
    switch (state) {
    case POWER_MONITOR_CHARGE_CHARGING:    return "charging";
    case POWER_MONITOR_CHARGE_DISCHARGING: return "discharging";
    case POWER_MONITOR_CHARGE_IDLE:        return "idle";
    case POWER_MONITOR_CHARGE_FULL:        return "full";
    default:                               return "unknown";
    }
}

#if BOARD_CFG_BATTERY_INA226_PRESENT

static struct {
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t lock;
    bool present;
    float current_lsb;   /* A per LSB */
} s_pm;

static esp_err_t pm_read16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];
    esp_err_t error = i2c_master_transmit_receive(s_pm.dev, &reg, 1, buf, 2,
                                                  P4_CONFIG_BATTERY_I2C_TIMEOUT_MS);

    if (error == ESP_OK) {
        *out = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return error;
}

static esp_err_t pm_write16(uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };

    return i2c_master_transmit(s_pm.dev, buf, sizeof(buf),
                               P4_CONFIG_BATTERY_I2C_TIMEOUT_MS);
}

esp_err_t power_monitor_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_device_config_t dev_cfg;
    esp_err_t error;
    uint16_t mfr = 0;
    uint16_t die = 0;

#if !BOARD_CFG_BATTERY_INA226_PRESENT
    return ESP_ERR_NOT_FOUND;
#endif

    if (s_pm.present) {
        return ESP_OK;
    }
    if (s_pm.lock == NULL) {
        s_pm.lock = xSemaphoreCreateMutex();
    }

    bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = BOARD_CFG_BATTERY_INA226_ADDR;
    dev_cfg.scl_speed_hz = BOARD_CFG_I2C_CLK_SPEED_HZ;
    error = i2c_master_bus_add_device(bus, &dev_cfg, &s_pm.dev);
    if (error != ESP_OK) {
        s_pm.dev = NULL;
        return error;
    }

    error = pm_read16(INA226_REG_MFR_ID, &mfr);
    if (error != ESP_OK || mfr != INA226_MFR_ID_VALUE) {
        goto not_found;
    }
    error = pm_read16(INA226_REG_DIE_ID, &die);
    if (error != ESP_OK || die != INA226_DIE_ID_VALUE) {
        goto not_found;
    }

    /* Reset then program CONFIG + CALIBRATION. */
    (void)pm_write16(INA226_REG_CONFIG, 0x8000u);
    vTaskDelay(pdMS_TO_TICKS(2));
    error = pm_write16(INA226_REG_CONFIG, INA226_CONFIG_VALUE);
    if (error != ESP_OK) {
        goto fail;
    }

    s_pm.current_lsb = ((float)BOARD_CFG_BATTERY_INA226_MAX_CURRENT_MA / 1000.0f) / 32768.0f;
    {
        float shunt_ohms = (float)BOARD_CFG_BATTERY_INA226_SHUNT_MILLIOHM / 1000.0f;
        uint32_t cal = (uint32_t)(0.00512f / (s_pm.current_lsb * shunt_ohms));
        if (cal == 0) {
            cal = 1;
        }
        if (cal > 0xFFFFu) {
            cal = 0xFFFFu;
        }
        error = pm_write16(INA226_REG_CALIBRATION, (uint16_t)cal);
        if (error != ESP_OK) {
            goto fail;
        }
    }

    s_pm.present = true;
    ESP_LOGI(POWER_MONITOR_TAG, "INA226 ready at 0x%02X (mfr=0x%04X die=0x%04X)",
             BOARD_CFG_BATTERY_INA226_ADDR, mfr, die);
    return ESP_OK;

not_found:
    i2c_master_bus_rm_device(s_pm.dev);
    s_pm.dev = NULL;
    return ESP_ERR_NOT_FOUND;
fail:
    i2c_master_bus_rm_device(s_pm.dev);
    s_pm.dev = NULL;
    return error;
}

bool power_monitor_available(void)
{
    return s_pm.present;
}

static int pm_mv_to_percent(int pack_mv)
{
    const int empty = BOARD_CFG_BATTERY_EMPTY_MV;
    const int full = BOARD_CFG_BATTERY_FULL_MV;

    if (pack_mv <= empty) {
        return 0;
    }
    if (pack_mv >= full) {
        return 100;
    }
    if (full <= empty) {
        return 0;
    }
    return (pack_mv - empty) * 100 / (full - empty);
}

esp_err_t power_monitor_read_sample(int *pack_mv_out, int *percent_out,
                                    int *current_ma_out, int *power_mw_out,
                                    bool *charging_out)
{
    uint16_t bus_raw = 0;
    uint16_t cur_raw = 0;
    uint16_t pow_raw = 0;
    int pack_mv;
    esp_err_t error;

    if (!s_pm.present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_pm.lock != NULL) {
        xSemaphoreTake(s_pm.lock, portMAX_DELAY);
    }

    error = pm_read16(INA226_REG_BUS_VOLT, &bus_raw);
    if (error != ESP_OK) {
        goto out;
    }
    (void)pm_read16(INA226_REG_CURRENT, &cur_raw);
    (void)pm_read16(INA226_REG_POWER, &pow_raw);

    pack_mv = (int)(((uint32_t)bus_raw * 125u) / 100u);   /* 1.25 mV/LSB */

    if (pack_mv_out != NULL) {
        *pack_mv_out = pack_mv;
    }
    if (percent_out != NULL) {
        *percent_out = pm_mv_to_percent(pack_mv);
    }
    if (current_ma_out != NULL) {
        *current_ma_out = (int)((int16_t)cur_raw * s_pm.current_lsb * 1000.0f);
    }
    if (power_mw_out != NULL) {
        *power_mw_out = (int)((float)pow_raw * (25.0f * s_pm.current_lsb) * 1000.0f);
    }
    if (charging_out != NULL) {
        int current_ma = (int)((int16_t)cur_raw * s_pm.current_lsb * 1000.0f);

        *charging_out = (power_monitor_classify_charge(pack_mv, current_ma) ==
                         POWER_MONITOR_CHARGE_CHARGING);
    }

out:
    if (s_pm.lock != NULL) {
        xSemaphoreGive(s_pm.lock);
    }
    return error;
}

esp_err_t power_monitor_battery_read(int *pack_mv_out, int *percent_out,
                                     bool *charging_out)
{
    return power_monitor_read_sample(pack_mv_out, percent_out, NULL, NULL,
                                     charging_out);
}

#else  /* !BOARD_CFG_BATTERY_INA226_PRESENT */

esp_err_t power_monitor_init(void)
{
    return ESP_ERR_NOT_FOUND;
}

bool power_monitor_available(void)
{
    return false;
}

esp_err_t power_monitor_read_sample(int *pack_mv_out, int *percent_out,
                                    int *current_ma_out, int *power_mw_out,
                                    bool *charging_out)
{
    (void)pack_mv_out;
    (void)percent_out;
    (void)current_ma_out;
    (void)power_mw_out;
    (void)charging_out;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t power_monitor_battery_read(int *pack_mv_out, int *percent_out,
                                     bool *charging_out)
{
    return power_monitor_read_sample(pack_mv_out, percent_out, NULL, NULL,
                                     charging_out);
}

#endif /* BOARD_CFG_BATTERY_INA226_PRESENT */
