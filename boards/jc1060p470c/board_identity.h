/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file board_identity.h
 * @brief Board identity strings and the chip serial accessor - no pin or
 *        driver dependency.
 *
 * board_config.h carries every hardware constant and therefore includes
 * driver/gpio.h. Code that only needs to name the board (the `%BOARD%` /
 * `%SERIAL%` virtual variables, the path helpers in
 * components/storage/storage_path.c) must compile on the hardware-free host
 * lane too, so the identity strings live here and board_config.h includes
 * this header. One definition, two audiences.
 *
 * @module boards/jc1060p470c
 * @layer leaf
 * @domain platform
 * @provides board identity strings, SD mount point, board_identity_serial()
 * @status stable
 * @test test/main/test_calc.c
 * @hard-rule no driver includes on the host build path; the MAC read is target-only
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sdkconfig.h"

// ---- Board identity ----
// BOARD_CFG_ID is the machine-readable profile slug used by the host tools to
// map a COM port to a board (see tools/board_ports.py + PORTING.md). It must
// match the boards/<name>/ directory name and the -DP4_BOARD= value.
#define BOARD_CFG_ID "jc1060p470c"
#define BOARD_CFG_NAME "JC1060P470C"
#define BOARD_CFG_DETECTED_NAME "ESP32-P4-Function-EV-Board"

// SD card VFS mount point. It lives with the identity strings (not with the
// pin map) so the pure path helpers resolve `sd:/...` without pulling in
// driver/gpio.h.
#define BOARD_CFG_SD_MOUNT_POINT "/sdcard"

#if !defined(CONFIG_IDF_TARGET_LINUX)
#include "esp_mac.h"
#endif

/**
 * The board's chip serial as uppercase hex, or "" when unknown.
 *
 * On the target this reads the factory Wi-Fi STA MAC once and caches it for
 * the boot (the same value `%SERIAL%` always exposed). The ESP-IDF `linux`
 * target has no eFuse/MAC emulation, so it reports the documented "unknown"
 * value (an empty string) instead of inventing a serial number.
 *
 * @return Static, NUL-terminated string owned by this header's caller TU.
 */
static inline const char *board_identity_serial(void)
{
#if defined(CONFIG_IDF_TARGET_LINUX)
    return "";
#else
    static char serial[13];
    static bool ready;

    if (!ready) {
        uint8_t mac[6] = {0};

        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            serial[0] = '\0';
        }
        ready = true;
    }
    return serial;
#endif
}
