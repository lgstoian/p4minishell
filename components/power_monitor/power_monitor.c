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

#include <math.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"

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
 * On the Tab5 the shunt reads charge current as NEGATIVE (verified: the pack
 * voltage rises while the current is negative), so positive current is the
 * pack supplying the system. */
#ifndef BOARD_CFG_BATTERY_CHARGE_CURRENT_MA
#define BOARD_CFG_BATTERY_CHARGE_CURRENT_MA 20
#endif
#ifndef BOARD_CFG_BATTERY_FULL_MV
#define BOARD_CFG_BATTERY_FULL_MV 0
#endif

power_monitor_charge_t power_monitor_classify_charge(int pack_mv, int current_ma)
{
    /* On the Tab5 the INA226 shunt reads charge current as NEGATIVE (verified:
     * the pack voltage rises while the current is negative, and it moves toward
     * zero as system load rises), so positive current is the pack supplying the
     * system. */
    if (current_ma >= (int)BOARD_CFG_BATTERY_CHARGE_CURRENT_MA) {
        return POWER_MONITOR_CHARGE_DISCHARGING;
    }
#if BOARD_CFG_BATTERY_FULL_MV > 0
    /* At/above the full threshold with no meaningful current: charged. */
    if (current_ma > -(int)BOARD_CFG_BATTERY_CHARGE_CURRENT_MA &&
        pack_mv >= (int)BOARD_CFG_BATTERY_FULL_MV) {
        return POWER_MONITOR_CHARGE_FULL;
    }
#else
    (void)pack_mv;
#endif
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

bool power_monitor_pack_present(int pack_mv, int current_ma,
                                const int *recent_mv, int recent_count,
                                int chg_stat, int chg_stat_prev)
{
    const int present_mv = BOARD_CFG_BATTERY_PRESENT_MV;
    int window = 1 + recent_count;
    int minv;
    int maxv;
    int i;

    if (pack_mv < present_mv) {
        /* A real pack never reads this low while the gauge is powered. */
        return false;
    }

#if BOARD_CFG_BATTERY_FULL_MV > 0
    {
        int rail_floor = (int)BOARD_CFG_BATTERY_FULL_MV - P4_CONFIG_BATTERY_RAIL_BAND_TOL_MV;

        if (rail_floor < present_mv) {
            rail_floor = present_mv;
        }
        if (pack_mv < rail_floor) {
            /* Clearly inside the pack range: trust it immediately. */
            return true;
        }
    }

    /* Ambiguous rail band (>= FULL_MV - tolerance). With no pack the node
     * floats here and swings to the low cluster; a real pack is stable. */
    if (window < P4_CONFIG_BATTERY_PRESENT_STABLE_SAMPLES) {
        return false;
    }
    minv = pack_mv;
    maxv = pack_mv;
    for (i = 0; i < recent_count; i++) {
        if (recent_mv[i] < present_mv) {
            return false;
        }
        if (recent_mv[i] < minv) {
            minv = recent_mv[i];
        }
        if (recent_mv[i] > maxv) {
            maxv = recent_mv[i];
        }
    }
    if (maxv - minv > P4_CONFIG_BATTERY_PRESENT_MAX_SWING_MV) {
        return false;
    }

    /* Corroboration (F24-B): a charge-status line that toggles while the
     * voltage sits in the rail band with ~0 current is the charger blinking
     * with no pack to charge. Gated on ~0 current so a charging pack (toggling
     * status, non-zero current) is never rejected. */
    if (chg_stat >= 0 && chg_stat_prev >= 0 && chg_stat != chg_stat_prev &&
        current_ma > -(int)BOARD_CFG_BATTERY_CHARGE_CURRENT_MA &&
        current_ma < (int)BOARD_CFG_BATTERY_CHARGE_CURRENT_MA) {
        return false;
    }
    return true;
#else
    (void)current_ma;
    (void)recent_mv;
    (void)recent_count;
    (void)chg_stat;
    (void)chg_stat_prev;
    (void)window;
    return true;
#endif
}

#if BOARD_CFG_BATTERY_INA226_PRESENT

static struct {
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t lock;
    bool present;
    float current_lsb;   /* A per LSB */
    uint16_t cal;        /* programmed calibration register */
    int hist[P4_CONFIG_BATTERY_PRESENT_STABLE_SAMPLES]; /* prior pack_mv, newest first */
    int hist_count;
    int chg_stat_prev;   /* previous IP2326 CHG_STAT level, -1 when unavailable */
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

    {
        /* Match the M5Stack reference calibration: Current_LSB is rounded UP to
         * the next 0.1 mA, then CAL = 0.00512 / (Current_LSB * Rshunt). */
        float max_a = (float)BOARD_CFG_BATTERY_INA226_MAX_CURRENT_MA / 1000.0f;
        float shunt_ohms = (float)BOARD_CFG_BATTERY_INA226_SHUNT_MILLIOHM / 1000.0f;
        float min_lsb = max_a / 32767.0f;
        uint32_t cal;

        s_pm.current_lsb = ceilf(min_lsb / 0.0001f) * 0.0001f;
        if (s_pm.current_lsb <= 0.0f) {
            s_pm.current_lsb = 0.0001f;
        }
        cal = (uint32_t)(0.00512f / (s_pm.current_lsb * shunt_ohms));
        if (cal == 0) {
            cal = 1;
        }
        if (cal > 0xFFFFu) {
            cal = 0xFFFFu;
        }
        s_pm.cal = (uint16_t)cal;
        error = pm_write16(INA226_REG_CALIBRATION, s_pm.cal);
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
    int current_ma = 0;
    int chg_stat = -1;
    bool pack_present;
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
    current_ma = (int)((int16_t)cur_raw * s_pm.current_lsb * 1000.0f);

    /* Corroborating charge-status line (F24-B); -1 when the board has none. */
    (void)board_bsp_charge_status_level(&chg_stat);

    /* Decide pack presence BEFORE publishing: a floating rail (no pack) must
     * not be reported as a full pack (bugs.md F24). */
    pack_present = power_monitor_pack_present(pack_mv, current_ma,
                                              s_pm.hist, s_pm.hist_count,
                                              chg_stat, s_pm.chg_stat_prev);

    /* Push this sample into the presence window (newest first). */
    {
        int keep = P4_CONFIG_BATTERY_PRESENT_STABLE_SAMPLES - 1;
        int i;

        if (keep < 1) {
            keep = 1;
        }
        if (s_pm.hist_count > keep) {
            s_pm.hist_count = keep;
        }
        for (i = s_pm.hist_count; i > 0; i--) {
            s_pm.hist[i] = s_pm.hist[i - 1];
        }
        s_pm.hist[0] = pack_mv;
        if (s_pm.hist_count < keep) {
            s_pm.hist_count++;
        }
    }
    s_pm.chg_stat_prev = chg_stat;

    if (pack_mv_out != NULL) {
        *pack_mv_out = pack_mv;
    }
    if (percent_out != NULL) {
        *percent_out = pm_mv_to_percent(pack_mv);
    }
    if (current_ma_out != NULL) {
        *current_ma_out = current_ma;
    }
    if (power_mw_out != NULL) {
        *power_mw_out = (int)((float)pow_raw * (25.0f * s_pm.current_lsb) * 1000.0f);
    }
    if (charging_out != NULL) {
        *charging_out = (power_monitor_classify_charge(pack_mv, current_ma) ==
                         POWER_MONITOR_CHARGE_CHARGING);
    }

    if (!pack_present) {
        error = ESP_ERR_NOT_FOUND;
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

esp_err_t power_monitor_read_diag(power_monitor_diag_t *out)
{
    uint16_t bus_raw = 0;
    uint16_t shunt_raw = 0;
    uint16_t cur_raw = 0;
    uint16_t pow_raw = 0;
    uint16_t config = 0;
    esp_err_t error;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_pm.present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_pm.lock != NULL) {
        xSemaphoreTake(s_pm.lock, portMAX_DELAY);
    }
    error = pm_read16(INA226_REG_BUS_VOLT, &bus_raw);
    if (error == ESP_OK) {
        (void)pm_read16(INA226_REG_SHUNT_VOLT, &shunt_raw);
        (void)pm_read16(INA226_REG_CURRENT, &cur_raw);
        (void)pm_read16(INA226_REG_POWER, &pow_raw);
        (void)pm_read16(INA226_REG_CONFIG, &config);

        out->bus_mv = (int)(((uint32_t)bus_raw * 125u) / 100u);
        out->shunt_uv = (int)((int16_t)shunt_raw) * 25 / 10;  /* 2.5 uV/LSB */
        out->current_raw = (int)(int16_t)cur_raw;
        out->current_ma = (int)((int16_t)cur_raw * s_pm.current_lsb * 1000.0f);
        out->power_mw = (int)((float)pow_raw * (25.0f * s_pm.current_lsb) * 1000.0f);
        out->config = config;
        out->cal = s_pm.cal;
        (void)board_bsp_charge_status_level(&out->chg_stat);
    }
    if (s_pm.lock != NULL) {
        xSemaphoreGive(s_pm.lock);
    }
    return error;
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

esp_err_t power_monitor_read_diag(power_monitor_diag_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_FOUND;
}

#endif /* BOARD_CFG_BATTERY_INA226_PRESENT */
