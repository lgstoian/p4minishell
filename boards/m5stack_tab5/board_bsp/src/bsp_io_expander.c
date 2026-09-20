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
#define PI4IO_REG_PULL_SEL   0x0D
#define PI4IO_REG_INT_MASK   0x11

/* Second PI4IOE5V6408 (0x44) outputs: P5 = charge QuickCharge enable (active
 * low), P7 = charge enable, P4 = PMIC power-off latch, P0 = Wi-Fi rail, P3 =
 * USB 5V rail. */
#define BSP_CHARGE_QC_EN_MASK (1u << 5)
#define BSP_CHARGE_EN_MASK    (1u << 7)
#define BSP_POWEROFF_MASK     (1u << 4)

/*
 * The managed esp_io_expander driver resets every pin to high-impedance and
 * offers no API to program OUT_H_IM/IN_DEF_STA/INT_MASK, so the second expander
 * is configured here with raw register writes exactly as the M5Stack reference
 * does. Without driving P4/P5 (as opposed to leaving them high-Z) the PMIC
 * charge/QuickCharge path never engages and the pack does not charge.
 *
 * This is intentionally raw + cached: `s_pi4ioe2_out` shadows OUT_SET so later
 * charge/poweroff updates preserve the rail bits.
 */
static bool s_pi4ioe2_ready;
static uint8_t s_pi4ioe2_out;

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

static void bsp_io_expander1_write(uint8_t reg, uint8_t val)
{
    (void)bsp_io_expander1_raw(reg, &val, true);
}

/** Configure the second expander once (M5Stack sequence; no chip reset). */
static void bsp_io_expander1_ensure(void)
{
    if (s_pi4ioe2_ready) {
        return;
    }
    (void)bsp_i2c_init();
    bsp_io_expander1_write(PI4IO_REG_IO_DIR, 0xB9);     /* P1,P2,P6 inputs */
    bsp_io_expander1_write(PI4IO_REG_OUT_H_IM, 0x06);   /* P1,P2 high-Z */
    bsp_io_expander1_write(PI4IO_REG_PULL_SEL, 0xB9);
    bsp_io_expander1_write(PI4IO_REG_PULL_EN, 0xF9);
    bsp_io_expander1_write(PI4IO_REG_IN_DEF_STA, 0x40);
    bsp_io_expander1_write(PI4IO_REG_INT_MASK, 0xBF);
    s_pi4ioe2_out = (1u << 0) | (1u << 3);              /* P0 Wi-Fi, P3 USB 5V */
    bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out);
    s_pi4ioe2_ready = true;
    ESP_LOGI(BSP_IO_EXPANDER_TAG, "second IO expander configured (M5Stack init)");
}

void bsp_set_charge_qc_en(bool enable)
{
    bsp_io_expander1_ensure();
    if (enable) {
        s_pi4ioe2_out &= (uint8_t)~BSP_CHARGE_QC_EN_MASK;  /* active low */
    } else {
        s_pi4ioe2_out |= (uint8_t)BSP_CHARGE_QC_EN_MASK;
    }
    bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out);
}

void bsp_set_charge_en(bool enable)
{
    bsp_io_expander1_ensure();
    if (enable) {
        s_pi4ioe2_out |= (uint8_t)BSP_CHARGE_EN_MASK;
    } else {
        s_pi4ioe2_out &= (uint8_t)~BSP_CHARGE_EN_MASK;
    }
    bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out);
}

void bsp_generate_poweroff_signal(void)
{
    /* Pulse P4 of the second PI4IOE5V6408 (0x44) three times, as the M5Stack
     * reference does, so a missed edge cannot leave the board on. */
    int i;

    bsp_io_expander1_ensure();
    ESP_LOGW(BSP_IO_EXPANDER_TAG, "Generating poweroff signal");

    for (i = 0; i < 3; i++) {
        bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out | (uint8_t)BSP_POWEROFF_MASK);
        vTaskDelay(pdMS_TO_TICKS(100));
        bsp_io_expander1_write(PI4IO_REG_OUT_SET, s_pi4ioe2_out);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
