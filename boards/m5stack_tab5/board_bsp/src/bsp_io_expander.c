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

/* PI4IOE P5 = charge QuickCharge enable (active low); P7 = charge enable. */
#define BSP_CHARGE_QC_EN_MASK (1u << 5)
#define BSP_CHARGE_EN_MASK    (1u << 7)

void bsp_set_charge_qc_en(bool enable)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();

    if (io == NULL) {
        return;
    }
    esp_io_expander_set_dir(io, BSP_CHARGE_QC_EN_MASK, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io, BSP_CHARGE_QC_EN_MASK, enable ? 0 : 1);
}

void bsp_set_charge_en(bool enable)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();

    if (io == NULL) {
        return;
    }
    esp_io_expander_set_dir(io, BSP_CHARGE_EN_MASK, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io, BSP_CHARGE_EN_MASK, enable ? 1 : 0);
}

void bsp_generate_poweroff_signal(void)
{
    /* P4 of the second PI4IOE5V6408 (addr 0x44) is the PMIC power-off latch.
     * Pulse it three times (the M5Stack reference does the same) so a missed
     * edge cannot leave the board on. */
    const uint32_t mask = IO_EXPANDER_PIN_NUM_4;
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    int i;

    if (io == NULL) {
        ESP_LOGW(BSP_IO_EXPANDER_TAG, "poweroff: second IO expander unavailable");
        return;
    }
    ESP_LOGW(BSP_IO_EXPANDER_TAG, "Generating poweroff signal");

    esp_io_expander_set_dir(io, mask, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io, mask, 0);
    for (i = 0; i < 3; i++) {
        esp_io_expander_set_level(io, mask, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_io_expander_set_level(io, mask, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
