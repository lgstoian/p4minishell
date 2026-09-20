/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_hardware.c
 * @brief Unit tests for the pure helpers behind the Tab5 hardware support:
 *        IMU orientation classification, battery charge classification, and the
 *        shared BMP/RGB565 image-format helpers.
 */

#include "unity.h"
#include "imu.h"
#include "power_monitor.h"
#include "imagefmt.h"
#include "header_status.h"
#include "board_config.h"
#include "p4minishell_config.h"
#include <stdint.h>
#include <string.h>

/* ---- IMU orientation ---------------------------------------------------- */

static imu_sample_t sample_with_accel(float ax, float ay, float az)
{
    imu_sample_t s;

    memset(&s, 0, sizeof(s));
    s.accel_mps2[0] = ax;
    s.accel_mps2[1] = ay;
    s.accel_mps2[2] = az;
    s.orientation = imu_orientation_from_sample(&s);
    return s;
}

void test_hardware_imu_orientation(void)
{
    /* ~1 g of gravity on one in-plane axis picks that orientation. */
    TEST_ASSERT_EQUAL_INT(IMU_ORIENT_LANDSCAPE, sample_with_accel(9.8f, 0.2f, 0.0f).orientation);
    TEST_ASSERT_EQUAL_INT(IMU_ORIENT_LANDSCAPE_INV, sample_with_accel(-9.8f, 0.2f, 0.0f).orientation);
    TEST_ASSERT_EQUAL_INT(IMU_ORIENT_PORTRAIT, sample_with_accel(0.2f, 9.8f, 0.0f).orientation);
    TEST_ASSERT_EQUAL_INT(IMU_ORIENT_PORTRAIT_INV, sample_with_accel(0.2f, -9.8f, 0.0f).orientation);

    /* Flat on the table (gravity out of plane) is ambiguous. */
    TEST_ASSERT_EQUAL_INT(IMU_ORIENT_UNKNOWN, sample_with_accel(0.0f, 0.0f, 9.8f).orientation);

    TEST_ASSERT_EQUAL_INT(0, imu_orientation_rotation_deg(IMU_ORIENT_PORTRAIT));
    TEST_ASSERT_EQUAL_INT(90, imu_orientation_rotation_deg(IMU_ORIENT_LANDSCAPE));
    TEST_ASSERT_EQUAL_INT(180, imu_orientation_rotation_deg(IMU_ORIENT_PORTRAIT_INV));
    TEST_ASSERT_EQUAL_INT(270, imu_orientation_rotation_deg(IMU_ORIENT_LANDSCAPE_INV));
    TEST_ASSERT_EQUAL_INT(-1, imu_orientation_rotation_deg(IMU_ORIENT_UNKNOWN));

    TEST_ASSERT_EQUAL_STRING("landscape", imu_orientation_name(IMU_ORIENT_LANDSCAPE));
    TEST_ASSERT_EQUAL_STRING("unknown", imu_orientation_name(IMU_ORIENT_UNKNOWN));
}

/* ---- Battery charge classification -------------------------------------- */

void test_hardware_charge_state(void)
{
    /* Positive current flows into the pack (M5Stack convention). */
    TEST_ASSERT_EQUAL_INT(POWER_MONITOR_CHARGE_CHARGING,
                          power_monitor_classify_charge(7600, 500));
    TEST_ASSERT_EQUAL_INT(POWER_MONITOR_CHARGE_DISCHARGING,
                          power_monitor_classify_charge(7600, -500));
    /* Not discharging (external power holding the pack at a standstill) is
     * reported as charging, matching the vendor UI. */
    TEST_ASSERT_EQUAL_INT(POWER_MONITOR_CHARGE_CHARGING,
                          power_monitor_classify_charge(7600, 0));

#if BOARD_CFG_BATTERY_FULL_MV > 0
    TEST_ASSERT_EQUAL_INT(POWER_MONITOR_CHARGE_FULL,
                          power_monitor_classify_charge(BOARD_CFG_BATTERY_FULL_MV, 0));
#endif

    TEST_ASSERT_EQUAL_STRING("charging", power_monitor_charge_name(POWER_MONITOR_CHARGE_CHARGING));
    TEST_ASSERT_EQUAL_STRING("discharging", power_monitor_charge_name(POWER_MONITOR_CHARGE_DISCHARGING));
}

/* ---- Header WiFi tone --------------------------------------------------- */

void test_hardware_wifi_tone(void)
{
    /* Disconnected is always an error tone. */
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR,
                          header_status_wifi_tone(false, P4_CONFIG_HEADER_RSSI_UNKNOWN));

    /* Connected with no usable RSSI (hosted get_ap_info returns 0) must NOT be
     * reported as an error. */
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK,
                          header_status_wifi_tone(true, P4_CONFIG_HEADER_RSSI_UNKNOWN));

    TEST_ASSERT_EQUAL_INT(HEADER_TONE_OK,
                          header_status_wifi_tone(true, P4_CONFIG_HEADER_RSSI_GOOD));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_WARN, header_status_wifi_tone(true, -75));
    TEST_ASSERT_EQUAL_INT(HEADER_TONE_ERR, header_status_wifi_tone(true, -95));
}

/* ---- BMP / RGB565 image format ------------------------------------------ */

void test_hardware_bmp_header(void)
{
    uint8_t buf[IMAGEFMT_BMP_HEADER_SIZE];

    TEST_ASSERT_EQUAL_INT(IMAGEFMT_BMP_HEADER_SIZE, imagefmt_write_bmp_header(buf, 2, 2));
    TEST_ASSERT_EQUAL_UINT8('B', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('M', buf[1]);
    /* file size = 54 + 2*3*2 = 66 */
    TEST_ASSERT_EQUAL_UINT8(66, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(54, buf[10]);      /* pixel data offset */
    TEST_ASSERT_EQUAL_UINT8(40, buf[14]);      /* info header size */
    TEST_ASSERT_EQUAL_UINT8(2, buf[18]);       /* width */
    TEST_ASSERT_EQUAL_UINT8(2, buf[22]);       /* height */
    TEST_ASSERT_EQUAL_UINT8(1, buf[26]);       /* planes */
    TEST_ASSERT_EQUAL_UINT8(24, buf[28]);      /* bpp */
    TEST_ASSERT_EQUAL_UINT8(0, buf[30]);       /* BI_RGB */
    TEST_ASSERT_EQUAL_UINT8(12, buf[34]);      /* image size 2*3*2 */
}

void test_hardware_rgb565_to_bgr24(void)
{
    const uint16_t src[3] = { 0xFFFF, 0x0000, 0xF800 };
    uint8_t out[9];

    imagefmt_rgb565_to_bgr24(src, out, 3);
    /* white */
    TEST_ASSERT_EQUAL_UINT8(0xFF, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, out[2]);
    /* black */
    TEST_ASSERT_EQUAL_UINT8(0x00, out[3]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[4]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[5]);
    /* red (BGR order: blue=0, green=0, red=255) */
    TEST_ASSERT_EQUAL_UINT8(0x00, out[6]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[7]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, out[8]);
}
