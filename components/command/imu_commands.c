/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imu_commands.c
 * @brief `imu` hardware verb: read the BMI270, classify the held orientation,
 *        toggle tilt-based auto-rotation, and publish the sample as environment
 *        variables so batch files can consume it.
 *
 * Usage:
 *   imu [read]          Read accel/gyro + orientation (also sets IMU_* env vars)
 *   imu status          IMU presence, orientation, auto-rotate state
 *   imu rotate on|off   Enable/disable rotating the display as the board turns
 *
 * ERRORLEVEL: 0 success, 1 failure, 2 usage.
 */

#include <stdio.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "display.h"
#include "imu.h"
#include "applib_env.h"
#include "command.h"
#include "board_config.h"
#include "esp_err.h"
#include "esp_log.h"

/* Board profile defaults for boards without an IMU. */
#ifndef BOARD_CFG_IMU_PRESENT
#define BOARD_CFG_IMU_PRESENT 0
#endif
#ifndef BOARD_CFG_IMU_I2C_ADDR
#define BOARD_CFG_IMU_I2C_ADDR 0x68
#endif

/** Rotation sink for auto-rotate (display_set_rotation). */
static void imu_apply_rotation(int rotation_deg, void *user)
{
    (void)user;
    (void)display_set_rotation((display_rotation_t)rotation_deg);
}

/** Publish a sample into the shared environment for batch consumers. */
static void imu_publish_env(const imu_sample_t *s)
{
    char buf[24];
    int rotation = imu_orientation_rotation_deg(s->orientation);
    static const char *acc_names[3] = { "IMU_AX", "IMU_AY", "IMU_AZ" };
    static const char *gyr_names[3] = { "IMU_GX", "IMU_GY", "IMU_GZ" };
    int i;

    for (i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "%.3f", s->accel_mps2[i]);
        app_env_set(acc_names[i], buf);
        snprintf(buf, sizeof(buf), "%.3f", s->gyro_dps[i]);
        app_env_set(gyr_names[i], buf);
    }
    snprintf(buf, sizeof(buf), "%.1f", s->pitch_deg);
    app_env_set("IMU_PITCH", buf);
    snprintf(buf, sizeof(buf), "%.1f", s->roll_deg);
    app_env_set("IMU_ROLL", buf);
    snprintf(buf, sizeof(buf), "%d", (rotation >= 0) ? rotation : -1);
    app_env_set("IMU_ORIENT", buf);
}

static bool imu_ensure_ready(void)
{
    if (imu_available()) {
        return true;
    }
    if (imu_init() == ESP_OK) {
        return true;
    }
    return false;
}

static void imu_print_unavailable(void)
{
    shell_print_error("imu: no IMU responding (board %s)",
                      (BOARD_CFG_IMU_PRESENT != 0) ? "has one but it did not answer" : "has no IMU");
    shell_record_warningf("imu", "IMU unavailable");
    batch_set_errorlevel(1);
}

static void imu_do_read(void)
{
    imu_sample_t s;

    if (!imu_ensure_ready()) {
        imu_print_unavailable();
        return;
    }
    if (imu_read(&s) != ESP_OK) {
        shell_print_error("imu: sample read failed");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_HEAD "IMU" SH_RST " " SH_MUTE "(BMI270)" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_LBL "accel:" SH_RST " " SH_NUM "%7.2f %7.2f %7.2f" SH_RST
                                  " " SH_MUTE "m/s^2" SH_RST "\n",
                                  s.accel_mps2[0], s.accel_mps2[1], s.accel_mps2[2]);
    shell_transcript_appendf_ansi("  " SH_LBL "gyro:" SH_RST " " SH_NUM "%7.2f %7.2f %7.2f" SH_RST
                                  " " SH_MUTE "deg/s" SH_RST "\n",
                                  s.gyro_dps[0], s.gyro_dps[1], s.gyro_dps[2]);
    shell_transcript_appendf_ansi("  " SH_LBL "pitch/roll:" SH_RST " " SH_NUM "%.1f / %.1f" SH_RST
                                  " " SH_MUTE "deg" SH_RST "\n", s.pitch_deg, s.roll_deg);
    shell_transcript_appendf_ansi("  " SH_LBL "orientation:" SH_RST " " SH_VAL "%s" SH_RST
                                  " " SH_MUTE "(rotation %d)" SH_RST "\n",
                                  imu_orientation_name(s.orientation),
                                  imu_orientation_rotation_deg(s.orientation));
    imu_publish_env(&s);
    shell_transcript_appendf_ansi("  " SH_MUTE "IMU_AX..IMU_ORIENT set for batch use" SH_RST "\n");
    batch_set_errorlevel(0);
}

static void imu_do_status(void)
{
    bool ready = imu_ensure_ready();

    shell_transcript_appendf_ansi(SH_HEAD "IMU" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_LBL "present:" SH_RST " %s\n",
                                  ready ? SH_OK "yes" SH_RST : SH_MUTE "no" SH_RST);
    shell_transcript_appendf_ansi("  " SH_LBL "address:" SH_RST " " SH_NUM "0x%02X" SH_RST
                                  " " SH_MUTE "(SYS I2C)" SH_RST "\n",
                                  (int)BOARD_CFG_IMU_I2C_ADDR);
    if (ready) {
        imu_sample_t s;

        if (imu_read(&s) == ESP_OK) {
            shell_transcript_appendf_ansi("  " SH_LBL "orientation:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                          imu_orientation_name(s.orientation));
        }
    }
    shell_transcript_appendf_ansi("  " SH_LBL "auto-rotate:" SH_RST " %s\n",
                                  imu_auto_rotate_enabled() ? SH_OK "on" SH_RST : SH_MUTE "off" SH_RST);
    batch_set_errorlevel(ready ? 0 : 1);
}

static void imu_do_rotate(int argc, char **argv)
{
    bool enable;

    if (argc != 3 ||
        !(shell_text_equals_ignore_case(argv[2], "on") || shell_text_equals_ignore_case(argv[2], "off"))) {
        shell_print_usage("Usage: imu rotate <on|off>");
        shell_record_warningf("imu", "Usage error for imu rotate");
        batch_set_errorlevel(2);
        return;
    }
    if (!imu_ensure_ready()) {
        imu_print_unavailable();
        return;
    }
    enable = shell_text_equals_ignore_case(argv[2], "on");
    imu_set_rotation_callback(imu_apply_rotation, NULL);
    if (imu_set_auto_rotate(enable) != ESP_OK) {
        shell_print_error("imu: failed to set auto-rotate");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "imu:" SH_RST " auto-rotate %s\n",
                                  enable ? SH_OK "enabled" SH_RST : SH_MUTE "disabled" SH_RST);
    batch_set_errorlevel(0);
}

void shell_execute_imu_command(int argc, char **argv)
{
    if (argc == 1 || shell_text_equals_ignore_case(argv[1], "read")) {
        imu_do_read();
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "status")) {
        imu_do_status();
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "rotate")) {
        imu_do_rotate(argc, argv);
        return;
    }
    shell_print_usage("Usage: imu [read] | imu status | imu rotate <on|off>");
    shell_record_warningf("imu", "Usage error for imu command");
    batch_set_errorlevel(2);
}
