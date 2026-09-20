/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tab5kbd.h
 * @brief M5Stack Tab5Keyboard (STM32F030, I2C 0x6D) input source.
 *
 * The Tab5Keyboard is a 70-key module on the Tab5 expansion port. It exposes an
 * I2C register interface plus an interrupt line and can report key events in
 * Normal / HID / Character modes. This component is the board's third input
 * source beside USB HID and the on-screen keyboard: it drains HID-mode reports
 * and feeds them into the shell through the same `shell_usb_keyboard_input()`
 * path the USB keyboard uses, so there is exactly one keystroke injection
 * point.
 *
 * On boards without the keyboard (`BOARD_CFG_TAB5KBD_PRESENT == 0`) every entry
 * point is a cheap no-op and the module never touches I2C. The driver is
 * compile-verified only; it has not been exercised against Tab5 hardware yet.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Tab5Keyboard data-packet modes (register 0x10 KEYBOARD_MODE). */
typedef enum {
    TAB5KBD_MODE_NORMAL = 0,    /*!< Row/column coordinate events */
    TAB5KBD_MODE_HID = 1,       /*!< HID reports (modifier + key code) */
    TAB5KBD_MODE_CHARACTER = 2, /*!< Modifier byte + ASCII string */
} tab5kbd_mode_t;

/** Snapshot of the module state for diagnostics (`gpio`/`sysinfo` style). */
typedef struct {
    bool present;   /*!< Address responded to the last probe */
    bool ready;     /*!< Device handle installed and poll task running */
    uint8_t mode;   /*!< Active mode (tab5kbd_mode_t) */
    uint32_t events;/*!< Events forwarded since init */
} tab5kbd_status_t;

/**
 * @brief Probe the module and start the event poll task when present.
 *
 * Safe to call on any board. On a board without the keyboard, or when the
 * module is not attached, this returns a benign error and leaves the module
 * disabled; the firmware continues without physical-keyboard input.
 *
 * @return ESP_OK when the keyboard was found and the poll task started;
 *         ESP_ERR_NOT_SUPPORTED when the board has no Tab5Keyboard;
 *         ESP_ERR_NOT_FOUND when the board supports it but the module is absent;
 *         another esp_err_t on an I2C failure.
 */
esp_err_t tab5kbd_init(void);

/** @brief Stop the poll task and release the I2C device (idempotent). */
void tab5kbd_deinit(void);

/** @brief Last probe/ready state. */
bool tab5kbd_is_ready(void);

/**
 * @brief Copy the current status snapshot.
 * @param out Destination (must not be NULL).
 */
void tab5kbd_get_status(tab5kbd_status_t *out);

/**
 * @brief Drain and forward any pending key events once.
 *
 * Exposed for the unit tests and for a caller that prefers to poll from its own
 * cadence; the poll task normally calls this itself.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_STATE when the module is not ready.
 */
esp_err_t tab5kbd_poll(void);

/**
 * @brief Set both keyboard RGB LEDs to one colour.
 *
 * No-op error (ESP_ERR_INVALID_STATE) until the module is ready; the module is
 * put in custom-RGB mode at init so these take effect. The `rgb` command and
 * the auto status colour route here on the Tab5.
 */
esp_err_t tab5kbd_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set one keyboard RGB LED independently.
 *
 * The module has two LEDs (index 0 and 1). The other LED keeps its current
 * colour. Custom-RGB mode is selected at init, so these take effect
 * immediately.
 *
 * @param index 0 for RGB1, 1 for RGB2.
 * @return ESP_OK, ESP_ERR_INVALID_ARG on a bad index, or
 *         ESP_ERR_INVALID_STATE until the module is ready.
 */
esp_err_t tab5kbd_set_rgb_index(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Read the last colour written to one keyboard RGB LED.
 * @param index 0 for RGB1, 1 for RGB2.
 * @param r/g/b Destinations (all may be NULL).
 * @return ESP_OK, or ESP_ERR_INVALID_ARG on a bad index.
 */
esp_err_t tab5kbd_get_rgb_index(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b);

/** @brief Set the keyboard LED brightness (0..100). */
esp_err_t tab5kbd_set_brightness(uint8_t percent);

/** @brief Read the module firmware version (register 0xFE). */
esp_err_t tab5kbd_get_version(uint8_t *version_out);
