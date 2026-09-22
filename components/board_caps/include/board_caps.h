/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file board_caps.h
 * @brief Zero-cost board capability queries over BOARD_CFG_* macros.
 *
 * Thin header-only helpers so command/display code asks one question
 * ("does this profile have a camera?") instead of scattering
 * `#if BOARD_CFG_*_PRESENT` branches and fallback `#ifndef` guards.
 * Everything folds to a compile-time constant; there is no runtime cost,
 * no RAM, and no new dependency direction (leaf: board_config.h only).
 */

#pragma once

#include <stdbool.h>

#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True when the profile wires an ADC battery divider. */
static inline bool board_caps_has_battery_adc(void)
{
#if defined(BOARD_CFG_BATTERY_ADC_PRESENT) && BOARD_CFG_BATTERY_ADC_PRESENT
    return true;
#else
    return false;
#endif
}

/** True when the profile carries an INA226 pack gauge. */
static inline bool board_caps_has_ina226(void)
{
#if defined(BOARD_CFG_BATTERY_INA226_PRESENT) && BOARD_CFG_BATTERY_INA226_PRESENT
    return true;
#else
    return false;
#endif
}

/** True when a MIPI-CSI camera is fitted. */
static inline bool board_caps_has_camera(void)
{
#if defined(BOARD_CFG_CAMERA_SUPPORTED) && BOARD_CFG_CAMERA_SUPPORTED
    return true;
#else
    return false;
#endif
}

/** True when an IMU is fitted. */
static inline bool board_caps_has_imu(void)
{
#if defined(BOARD_CFG_IMU_SUPPORTED) && BOARD_CFG_IMU_SUPPORTED
    return true;
#else
    return false;
#endif
}

/** True when the Tab5Keyboard module is part of the profile. */
static inline bool board_caps_has_tab5kbd(void)
{
#if defined(BOARD_CFG_TAB5KBD_PRESENT) && BOARD_CFG_TAB5KBD_PRESENT
    return true;
#else
    return false;
#endif
}

/** True when status LEDs route through the Tab5Keyboard module. */
static inline bool board_caps_rgb_via_tab5kbd(void)
{
#if defined(BOARD_CFG_RGB_VIA_TAB5KBD) && BOARD_CFG_RGB_VIA_TAB5KBD
    return true;
#else
    return false;
#endif
}

/** True when a WS2812 status LED is wired to a GPIO. */
static inline bool board_caps_has_ws2812(void)
{
#if defined(BOARD_CFG_RGB_LED_IS_WS2812) && BOARD_CFG_RGB_LED_IS_WS2812
    return true;
#else
    return false;
#endif
}

#ifdef __cplusplus
}
#endif
