/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tab5kbd.c
 * @brief M5Stack Tab5Keyboard (STM32F030, I2C 0x6D) input + RGB driver.
 *
 * Adapted from M5Stack's M5Tab5-Keyboard-UserDemo (`m5_tab5_keyboard.*`): the
 * module sits on the expansion port's I2C bus (SDA/SCL from board_config, 400
 * kHz) with an interrupt line, and exposes a byte-register interface:
 *
 *   0x00 INT_CFG   0x01 INT_STA   0x02 EVENT_NUM   0x03 BRIGHTNESS
 *   0x10 KEYBOARD_MODE (0 Normal, 1 HID, 2 String)  0x11 RGB_MODE (0 bind, 1 custom)
 *   0x20 KEY_EVENT (Normal)   0x30 HID_EVENT (2 bytes: modifier, keycode)
 *   0x40 CHAR_EVENT_LEN   0x50 CHAR_EVENT_BASE
 *   0x60 RGB_COLOR_BASE (RGB1 B,G,R, 0, RGB2 B,G,R)   0xFE VERSION
 *
 * HID reports are forwarded through `shell_usb_keyboard_input()` - the single
 * keystroke injection point shared with the USB HID keyboard - so the shell and
 * editor get physical-keyboard input for free. The two RGB LEDs are driven
 * through the `rgb` command / status mapping.
 *
 * The module is optional: when absent the driver probes once, disables itself,
 * and the firmware continues without it.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

#include "tab5kbd.h"
#include "board_config.h"
#include "p4minishell_config.h"
#include "shell.h"

#define TAB5KBD_TAG "tab5kbd"

#ifndef BOARD_CFG_TAB5KBD_PRESENT
#define BOARD_CFG_TAB5KBD_PRESENT 0
#endif

/* Register map (M5Stack Tab5 Keyboard I2C protocol). */
#define TAB5KBD_REG_INT_CFG       0x00
#define TAB5KBD_REG_INT_STA       0x01
#define TAB5KBD_REG_EVENT_NUM     0x02
#define TAB5KBD_REG_BRIGHTNESS    0x03
#define TAB5KBD_REG_KEYBOARD_MODE 0x10
#define TAB5KBD_REG_RGB_MODE      0x11
#define TAB5KBD_REG_KEY_EVENT     0x20
#define TAB5KBD_REG_HID_EVENT     0x30
#define TAB5KBD_REG_CHAR_EVENT_LEN 0x40
#define TAB5KBD_REG_CHAR_EVENT_BASE 0x50
#define TAB5KBD_REG_RGB_COLOR_BASE 0x60
#define TAB5KBD_REG_VERSION       0xFE

#define TAB5KBD_MODE_NORMAL 0
#define TAB5KBD_MODE_HID    1
#define TAB5KBD_RGB_MODE_CUSTOM 1

#define TAB5KBD_INT_STA_ANY 0x07

#define TAB5KBD_HID_MAX_REPORTS 16

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    TaskHandle_t task;
    bool bus_owned;
    bool present;
    bool ready;
    uint8_t mode;
    uint8_t version;
    uint32_t events;
    uint8_t rgb[2][3]; /*!< Last colour written to RGB1/RGB2 (r,g,b) */
} s_tab5kbd;

#if BOARD_CFG_TAB5KBD_PRESENT

/* ---- I2C transport ------------------------------------------------------- */

static esp_err_t tab5kbd_write_bytes(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buffer[1 + 8];
    esp_err_t error;
    int retry;

    if (s_tab5kbd.dev == NULL || len > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    buffer[0] = reg;
    memcpy(&buffer[1], data, len);
    for (retry = 0; retry < 2; retry++) {
        error = i2c_master_transmit(s_tab5kbd.dev, buffer, len + 1,
                                    P4_CONFIG_TAB5KBD_I2C_TIMEOUT_MS);
        if (error == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return error;
}

static esp_err_t tab5kbd_write(uint8_t reg, uint8_t value)
{
    return tab5kbd_write_bytes(reg, &value, 1);
}

static esp_err_t tab5kbd_read_bytes(uint8_t reg, uint8_t *data, size_t len)
{
    if (s_tab5kbd.dev == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(s_tab5kbd.dev, &reg, 1, data, len,
                                       P4_CONFIG_TAB5KBD_I2C_TIMEOUT_MS);
}

static esp_err_t tab5kbd_read(uint8_t reg, uint8_t *value)
{
    return tab5kbd_read_bytes(reg, value, 1);
}

/* ---- Event drain --------------------------------------------------------- */

/** Forward one HID report. Releases (key code 0) carry no shell action. */
static void tab5kbd_forward_hid(uint8_t modifier, uint8_t key_code)
{
    if (key_code == 0) {
        return;
    }
    shell_usb_keyboard_input(key_code, modifier, true);
    s_tab5kbd.events++;
}

/**
 * Drain pending events while in HID mode.
 *
 * The device stores the actual key reports, so a coalesced interrupt only needs
 * one pass: read the event count, pop that many {modifier, key_code} reports,
 * then clear the count and the interrupt latch.
 */
static esp_err_t tab5kbd_drain_hid(void)
{
    uint8_t status = 0;
    uint8_t count = 0;
    esp_err_t error;

    error = tab5kbd_read(TAB5KBD_REG_INT_STA, &status);
    if (error != ESP_OK) {
        return error;
    }
    if ((status & TAB5KBD_INT_STA_ANY) == 0) {
        return ESP_OK;
    }

    error = tab5kbd_read(TAB5KBD_REG_EVENT_NUM, &count);
    if (error != ESP_OK) {
        return error;
    }
    if (count > TAB5KBD_HID_MAX_REPORTS) {
        count = TAB5KBD_HID_MAX_REPORTS;
    }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t report[2];

        if (tab5kbd_read_bytes(TAB5KBD_REG_HID_EVENT, report, sizeof(report)) != ESP_OK) {
            break;
        }
        if (report[0] == 0xFF && report[1] == 0xFF) {
            break;
        }
        tab5kbd_forward_hid(report[0], report[1]);
    }

    /* Clear the device queue and interrupt latch for the next event. */
    (void)tab5kbd_write(TAB5KBD_REG_EVENT_NUM, 0);
    (void)tab5kbd_write(TAB5KBD_REG_INT_STA, 0);
    return ESP_OK;
}

/* ---- Poll task ----------------------------------------------------------- */

static void tab5kbd_task(void *arg)
{
    (void)arg;

    while (s_tab5kbd.ready) {
        if (s_tab5kbd.mode == TAB5KBD_MODE_HID) {
            (void)tab5kbd_drain_hid();
        }
        vTaskDelay(pdMS_TO_TICKS(P4_CONFIG_TAB5KBD_POLL_PERIOD_MS));
    }
    s_tab5kbd.task = NULL;
    vTaskDelete(NULL);
}

/* ---- Init ---------------------------------------------------------------- */

esp_err_t tab5kbd_init(void)
{
    i2c_master_bus_config_t bus_cfg;
    i2c_device_config_t dev_cfg;
    esp_err_t error;
    uint8_t int_cfg = 0;

    if (s_tab5kbd.ready) {
        return ESP_OK;
    }

    /* Dedicated controller keeps the module off the SYS I2C bus. */
    memset(&bus_cfg, 0, sizeof(bus_cfg));
    bus_cfg.i2c_port = BOARD_CFG_TAB5KBD_I2C_PORT;
    bus_cfg.sda_io_num = BOARD_CFG_TAB5KBD_SDA_GPIO;
    bus_cfg.scl_io_num = BOARD_CFG_TAB5KBD_SCL_GPIO;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    error = i2c_new_master_bus(&bus_cfg, &s_tab5kbd.bus);
    if (error != ESP_OK) {
        ESP_LOGW(TAB5KBD_TAG, "I2C bus init failed: %s", esp_err_to_name(error));
        s_tab5kbd.bus = NULL;
        return error;
    }
    s_tab5kbd.bus_owned = true;

    memset(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = BOARD_CFG_TAB5KBD_I2C_ADDR;
    dev_cfg.scl_speed_hz = 400000;
    error = i2c_master_bus_add_device(s_tab5kbd.bus, &dev_cfg, &s_tab5kbd.dev);
    if (error != ESP_OK) {
        s_tab5kbd.dev = NULL;
        goto fail;
    }

    /* Probe INT_CFG; a NAK means the module is not attached. */
    error = tab5kbd_read(TAB5KBD_REG_INT_CFG, &int_cfg);
    if (error != ESP_OK) {
        ESP_LOGI(TAB5KBD_TAG, "Tab5Keyboard not detected at 0x%02X", BOARD_CFG_TAB5KBD_I2C_ADDR);
        i2c_master_bus_rm_device(s_tab5kbd.dev);
        s_tab5kbd.dev = NULL;
        goto absent;
    }
    s_tab5kbd.present = true;

    (void)tab5kbd_read(TAB5KBD_REG_VERSION, &s_tab5kbd.version);

    /* HID mode, custom RGB so the shell can set colours, full brightness. */
    s_tab5kbd.mode = TAB5KBD_MODE_HID;
    (void)tab5kbd_write(TAB5KBD_REG_KEYBOARD_MODE, TAB5KBD_MODE_HID);
    (void)tab5kbd_write(TAB5KBD_REG_RGB_MODE, TAB5KBD_RGB_MODE_CUSTOM);
    (void)tab5kbd_write(TAB5KBD_REG_EVENT_NUM, 0);
    (void)tab5kbd_write(TAB5KBD_REG_INT_STA, 0);

    s_tab5kbd.events = 0;
    s_tab5kbd.ready = true;
    if (xTaskCreate(tab5kbd_task, "tab5kbd", P4_CONFIG_TAB5KBD_TASK_STACK, NULL,
                    tskIDLE_PRIORITY + 1, &s_tab5kbd.task) != pdPASS) {
        ESP_LOGE(TAB5KBD_TAG, "failed to create poll task");
        s_tab5kbd.ready = false;
        i2c_master_bus_rm_device(s_tab5kbd.dev);
        s_tab5kbd.dev = NULL;
        goto fail;
    }

    ESP_LOGI(TAB5KBD_TAG, "Tab5Keyboard ready at 0x%02X (fw 0x%02X, HID mode)",
             BOARD_CFG_TAB5KBD_I2C_ADDR, s_tab5kbd.version);
    return ESP_OK;

absent:
    if (s_tab5kbd.bus_owned && s_tab5kbd.bus != NULL) {
        i2c_del_master_bus(s_tab5kbd.bus);
        s_tab5kbd.bus = NULL;
        s_tab5kbd.bus_owned = false;
    }
    return ESP_ERR_NOT_FOUND;
fail:
    if (s_tab5kbd.bus_owned && s_tab5kbd.bus != NULL) {
        i2c_del_master_bus(s_tab5kbd.bus);
        s_tab5kbd.bus = NULL;
        s_tab5kbd.bus_owned = false;
    }
    return error;
}

#else  /* !BOARD_CFG_TAB5KBD_PRESENT */

esp_err_t tab5kbd_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* BOARD_CFG_TAB5KBD_PRESENT */

void tab5kbd_deinit(void)
{
    s_tab5kbd.ready = false;
    if (s_tab5kbd.task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(P4_CONFIG_TAB5KBD_POLL_PERIOD_MS + 10));
    }
    if (s_tab5kbd.dev != NULL) {
        i2c_master_bus_rm_device(s_tab5kbd.dev);
        s_tab5kbd.dev = NULL;
    }
    if (s_tab5kbd.bus_owned && s_tab5kbd.bus != NULL) {
        i2c_del_master_bus(s_tab5kbd.bus);
        s_tab5kbd.bus = NULL;
        s_tab5kbd.bus_owned = false;
    }
    s_tab5kbd.present = false;
}

bool tab5kbd_is_ready(void)
{
    return s_tab5kbd.ready;
}

void tab5kbd_get_status(tab5kbd_status_t *out)
{
    if (out == NULL) {
        return;
    }
    out->present = s_tab5kbd.present;
    out->ready = s_tab5kbd.ready;
    out->mode = s_tab5kbd.mode;
    out->events = s_tab5kbd.events;
}

esp_err_t tab5kbd_poll(void)
{
#if !BOARD_CFG_TAB5KBD_PRESENT
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_tab5kbd.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tab5kbd.mode != TAB5KBD_MODE_HID) {
        return ESP_OK;
    }
    return tab5kbd_drain_hid();
#endif
}

/* ---- RGB (the two keyboard LEDs) ----------------------------------------- */

esp_err_t tab5kbd_set_rgb_index(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
#if !BOARD_CFG_TAB5KBD_PRESENT
    (void)index; (void)r; (void)g; (void)b;
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* 7-byte window: RGB1 B,G,R, reserved(0), RGB2 B,G,R. The module expects
     * custom RGB mode (set at init) for these to take effect. */
    uint8_t buf[7];

    if (index > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tab5kbd.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_tab5kbd.rgb[index][0] = r;
    s_tab5kbd.rgb[index][1] = g;
    s_tab5kbd.rgb[index][2] = b;

    buf[0] = s_tab5kbd.rgb[0][2]; /* RGB1 blue */
    buf[1] = s_tab5kbd.rgb[0][1]; /* RGB1 green */
    buf[2] = s_tab5kbd.rgb[0][0]; /* RGB1 red */
    buf[3] = 0x00;                /* reserved */
    buf[4] = s_tab5kbd.rgb[1][2]; /* RGB2 blue */
    buf[5] = s_tab5kbd.rgb[1][1]; /* RGB2 green */
    buf[6] = s_tab5kbd.rgb[1][0]; /* RGB2 red */
    return tab5kbd_write_bytes(TAB5KBD_REG_RGB_COLOR_BASE, buf, sizeof(buf));
#endif
}

esp_err_t tab5kbd_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
#if !BOARD_CFG_TAB5KBD_PRESENT
    (void)r; (void)g; (void)b;
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t error;

    if (!s_tab5kbd.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    error = tab5kbd_set_rgb_index(0, r, g, b);
    if (error != ESP_OK) {
        return error;
    }
    return tab5kbd_set_rgb_index(1, r, g, b);
#endif
}

esp_err_t tab5kbd_get_rgb_index(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (index > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (r != NULL) {
        *r = s_tab5kbd.rgb[index][0];
    }
    if (g != NULL) {
        *g = s_tab5kbd.rgb[index][1];
    }
    if (b != NULL) {
        *b = s_tab5kbd.rgb[index][2];
    }
    return ESP_OK;
}

esp_err_t tab5kbd_set_brightness(uint8_t percent)
{
#if !BOARD_CFG_TAB5KBD_PRESENT
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_tab5kbd.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        percent = 100;
    }
    return tab5kbd_write(TAB5KBD_REG_BRIGHTNESS, percent);
#endif
}

esp_err_t tab5kbd_get_version(uint8_t *version_out)
{
#if !BOARD_CFG_TAB5KBD_PRESENT
    (void)version_out;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (version_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tab5kbd.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return tab5kbd_read(TAB5KBD_REG_VERSION, version_out);
#endif
}
