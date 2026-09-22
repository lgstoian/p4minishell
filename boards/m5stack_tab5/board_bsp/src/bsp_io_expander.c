/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp_err_check.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/m5stack_tab5.h"

#define BSP_IO_EXPANDER_TAG "bsp_io_expander"

static esp_io_expander_handle_t io_expander = NULL;  // IO Expander
static esp_io_expander_handle_t io_expander1 = NULL;  // IO Expander

esp_io_expander_handle_t bsp_io_expander_init(void)
{
    if (io_expander) {
        return io_expander;
    }
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());

    BSP_ERROR_CHECK_RETURN_NULL(esp_io_expander_new_i2c_pi4ioe5v6408(bsp_i2c_get_handle(),
                                BSP_IO_EXPANDER_ADDRESS, &io_expander));

    return io_expander;
}

esp_io_expander_handle_t bsp_io_expander1_init(void)
{
    if (io_expander1) {
        return io_expander1;
    }
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());

    BSP_ERROR_CHECK_RETURN_NULL(esp_io_expander_new_i2c_pi4ioe5v6408(bsp_i2c_get_handle(),
                                BSP_IO_EXPANDER_ADDRESS_1, &io_expander1));

    return io_expander1;
}

/* PI4IOE register map (matches the managed esp_io_expander driver). */
#define PI4IO_REG_IO_DIR     0x03
#define PI4IO_REG_OUT_SET    0x05
#define PI4IO_REG_OUT_H_IM   0x07
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN    0x0B
#define PI4IO_REG_PULL_SEL    0x0D
#define PI4IO_REG_IN_STA      0x0F
#define PI4IO_REG_INT_MASK    0x11

/* Second PI4IOE5V6408 (0x44) output pins. */
#define BSP_E2_WIFI_PIN      0   /* WLAN_PWR_EN (the ESP32-C6 rail) */
#define BSP_E2_USB_PIN       3   /* USB 5V output */
#define BSP_E2_QC_PIN        5   /* charge QuickCharge request (active low) */
#define BSP_E2_CHARGE_PIN    7   /* charge enable */
#define BSP_POWEROFF_MASK     (1u << 4)

/*
 * The managed esp_io_expander driver resets every pin to high-impedance and
 * offers no API to program OUT_H_IM/IN_DEF_STA/INT_MASK, so the second expander
 * is configured here with raw register writes exactly as the M5Stack reference
 * does. Without driving P4/P5 (as opposed to leaving them high-Z) the PMIC
 * charge/QuickCharge path never engages and the pack does not charge.
 *
 * The second expander is SINGLE-WRITER by design (bugs.md F6): P0 holds the
 * ESP32-C6 Wi-Fi rail up, so the driver-side creation path (which issues a
 * chip-wide software reset and floats every output) must never run against
 * 0x44 once the firmware is up. Feature enables for Wi-Fi/USB route here
 * through bsp_io_expander1_set_output(); the driver handle stays unused.
 *
 * This is intentionally raw + cached: `s_pi4ioe2_out` shadows OUT_SET so later
 * charge/poweroff updates preserve the rail bits. Every configuration attempt
 * is read back and retried (bugs.md F6: a silently half-configured expander
 * left the C6 rail unpowered for the whole session with no recovery).
 */
#define BSP_E2_DIR_VALUE     0xB9   /* P1,P2,P6 inputs; rest outputs */
#define BSP_E2_HIGHZ_VALUE   0x06   /* P1,P2 high-Z */
#define BSP_E2_PULLSEL_VALUE 0xB9
#define BSP_E2_PULLEN_VALUE  0xF9
#define BSP_E2_INDEF_VALUE   0x40
#define BSP_E2_INTMASK_VALUE 0xBF
#define BSP_E2_OUT_RAILS     ((1u << 0) | (1u << 3))  /* P0 Wi-Fi, P3 USB 5V */

static bool s_pi4ioe2_ready;
static uint8_t s_pi4ioe2_out = BSP_E2_OUT_RAILS;

static esp_err_t bsp_io_expander1_raw(uint8_t reg, uint8_t *val, bool write)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_IO_EXPANDER_ADDRESS_1,
        .scl_speed_hz = 100000,
    };
    esp_err_t err;

    if (bus == NULL) {
        return ESP_FAIL;
    }
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK || dev == NULL) {
        return ESP_FAIL;
    }
    if (write) {
        uint8_t buf[2] = { reg, *val };
        err = i2c_master_transmit(dev, buf, sizeof(buf), 100);
    } else {
        err = i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
    }
    i2c_master_bus_rm_device(dev);
    return err;
}

static esp_err_t bsp_io_expander1_write(uint8_t reg, uint8_t val)
{
    return bsp_io_expander1_raw(reg, &val, true);
}

/** One programming attempt: write the full M5Stack sequence, then read back. */
static esp_err_t bsp_io_expander1_try_config(void)
{
    static const struct {
        uint8_t reg;
        uint8_t val;
    } seq[] = {
        { PI4IO_REG_IO_DIR,     BSP_E2_DIR_VALUE },
        { PI4IO_REG_OUT_H_IM,   BSP_E2_HIGHZ_VALUE },
        { PI4IO_REG_PULL_SEL,   BSP_E2_PULLSEL_VALUE },
        { PI4IO_REG_PULL_EN,    BSP_E2_PULLEN_VALUE },
        { PI4IO_REG_IN_DEF_STA, BSP_E2_INDEF_VALUE },
        { PI4IO_REG_INT_MASK,   BSP_E2_INTMASK_VALUE },
    };
    esp_err_t err;
    size_t i;

    for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        err = bsp_io_expander1_write(seq[i].reg, seq[i].val);
        if (err != ESP_OK) {
            return err;
        }
        uint8_t readback = 0;
        err = bsp_io_expander1_raw(seq[i].reg, &readback, false);
        if (err != ESP_OK) {
            return err;
        }
        if (readback != seq[i].val) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    err = bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t out_readback = 0;
    err = bsp_io_expander1_raw(PI4IO_REG_OUT_SET, &out_readback, false);
    if (err != ESP_OK) {
        return err;
    }
    if (out_readback != s_pi4ioe2_out) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

#define BSP_E2_CONFIG_ATTEMPTS 3

/** Configure the second expander once (M5Stack sequence; no chip reset).
 *  Every register is read back; a failed attempt is retried a bounded number
 *  of times so a half-configured expander (unpowered Wi-Fi rail) cannot go
 *  unnoticed (bugs.md F6). Returns ESP_OK only when the chip state is
 *  verified, and is re-attempted on later calls while unverified. */
static esp_err_t bsp_io_expander1_ensure(void)
{
    esp_err_t err = ESP_FAIL;
    int attempt;

    if (s_pi4ioe2_ready) {
        return ESP_OK;
    }
    (void)bsp_i2c_init();
    for (attempt = 0; attempt < BSP_E2_CONFIG_ATTEMPTS; attempt++) {
        err = bsp_io_expander1_try_config();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(BSP_IO_EXPANDER_TAG, "second IO expander attempt %d failed: %s",
                 attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err == ESP_OK) {
        s_pi4ioe2_ready = true;
        ESP_LOGI(BSP_IO_EXPANDER_TAG, "second IO expander configured (M5Stack init, verified)");
    } else {
        ESP_LOGE(BSP_IO_EXPANDER_TAG, "second IO expander NOT configured: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t bsp_io_expander1_set_output(uint8_t pin, bool level)
{
    esp_err_t err;

    if (pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    err = bsp_io_expander1_ensure();
    if (err != ESP_OK) {
        return err;
    }
    if (level) {
        s_pi4ioe2_out |= (uint8_t)(1u << pin);
    } else {
        s_pi4ioe2_out &= (uint8_t)~(1u << pin);
    }
    err = bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out);
    if (err != ESP_OK) {
        ESP_LOGE(BSP_IO_EXPANDER_TAG, "OUT_SET write failed (P%u=%d): %s",
                 (unsigned)pin, (int)level, esp_err_to_name(err));
    }
    return err;
}

void bsp_set_charge_qc_en(bool enable)
{
    if (bsp_io_expander1_ensure() != ESP_OK) {
        ESP_LOGE(BSP_IO_EXPANDER_TAG, "charge QC enable (%d) skipped: expander unconfigured",
                 (int)enable);
        return;
    }
    /* P5 drives QuickCharge request and is active low. */
    (void)bsp_io_expander1_set_output(BSP_E2_QC_PIN, !enable);
}

void bsp_set_charge_en(bool enable)
{
    if (bsp_io_expander1_ensure() != ESP_OK) {
        ESP_LOGE(BSP_IO_EXPANDER_TAG, "charge enable (%d) skipped: expander unconfigured",
                 (int)enable);
        return;
    }
    (void)bsp_io_expander1_set_output(BSP_E2_CHARGE_PIN, enable);
}

void bsp_generate_poweroff_signal(void)
{
    /* Pulse P4 of the second PI4IOE5V6408 (0x44) three times, as the M5Stack
     * reference does, so a missed edge cannot leave the board on. */
    int i;

    (void)bsp_io_expander1_ensure();
    ESP_LOGW(BSP_IO_EXPANDER_TAG, "Generating poweroff signal");

    for (i = 0; i < 3; i++) {
        bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out | (uint8_t)BSP_POWEROFF_MASK);
        vTaskDelay(pdMS_TO_TICKS(100));
        bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int bsp_get_charge_status_level(void)
{
    /* E2.P6 is the IP2326 CHG_STAT_LED line, configured as an input. Reading it
     * is a corroborating signal for pack presence (bugs.md F24-B); the value is
     * the raw pin level (0/1) or -1 when the expander is unavailable. */
    uint8_t v = 0;

    (void)bsp_io_expander1_ensure();
    if (!s_pi4ioe2_ready) {
        return -1;
    }
    if (bsp_io_expander1_raw(PI4IO_REG_IN_STA, &v, false) != ESP_OK) {
        return -1;
    }
    return (v >> 6) & 0x1;
}
