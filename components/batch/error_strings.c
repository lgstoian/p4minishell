/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file error_strings.c
 * @brief Centralized error code → message table.
 *
 * Compact array indexed by error code. Gaps are filled with NULL;
 * error_code_string() scans linearly (fewer than 200 entries). Do not
 * reorder — the codes are a stable ABI for batch scripts that read
 * `%ERRMSG%` and for `help /all` listings.
 */

#include "error_strings.h"

typedef struct {
    int code;
    const char *msg;
} error_entry_t;

static const error_entry_t s_errors[] = {
    /* ---- Generic / command ---- */
    { ERR_BASE_OK,              "Success" },
    { ERR_BASE_ERROR,           "General error" },
    { ERR_BASE_USAGE,           "Incorrect usage — see help for this command" },
    { ERR_BASE_NOENT,           "File or directory not found" },
    { ERR_BASE_IO,              "I/O error (disk read/write failure)" },
    { ERR_BASE_NOMEM,           "Not enough memory" },
    { ERR_BASE_INVAL,           "Invalid parameter" },
    { ERR_BASE_NOTSUP,          "Operation not supported" },

    /* ---- Storage / filesystem ---- */
    { ERR_STORAGE_FULL,         "Storage full — free space and retry" },
    { ERR_STORAGE_WRITE_PROT,   "Write protected — cannot modify" },
    { ERR_STORAGE_CORRUPT,      "Filesystem corruption detected" },
    { ERR_STORAGE_NO_SD,        "No SD card detected" },
    { ERR_STORAGE_NO_USB,        "No USB mass storage device" },
    { ERR_STORAGE_LOOP,         "Too many symbolic link levels" },

    /* ---- Network ---- */
    { ERR_NET_DISCONNECTED,     "Network disconnected" },
    { ERR_NET_TIMEOUT,          "Connection timed out" },
    { ERR_NET_DNS,              "DNS lookup failed" },
    { ERR_NET_TLS,              "TLS handshake failed" },
    { ERR_NET_HTTP,             "HTTP error (server returned failure)" },
    { ERR_NET_WIFI_START,       "Failed to start Wi-Fi" },
    { ERR_NET_BT,               "Bluetooth error" },

    /* ---- App / package ---- */
    { ERR_APP_NOT_FOUND,        "Application not found" },
    { ERR_APP_CRC,              "Package CRC mismatch (corrupt download)" },
    { ERR_APP_SIGN,             "Package signature verification failed" },
    { ERR_APP_COMPAT,           "Package incompatible with this firmware" },
    { ERR_APP_LAUNCH,           "Failed to launch application" },

    /* ---- Hardware ---- */
    { ERR_HW_AUDIO,             "Audio subsystem error" },
    { ERR_HW_DISPLAY,           "Display initialization failed" },
    { ERR_HW_TOUCH,             "Touch controller not responding" },
    { ERR_HW_BATTERY,           "Battery sensor error" },
    { ERR_HW_CAMERA,            "Camera subsystem error" },
    { ERR_HW_IMU,               "IMU sensor error" },
};

const char *error_code_string(int code) {
    for (size_t i = 0; i < sizeof(s_errors) / sizeof(s_errors[0]); i++) {
        if (s_errors[i].code == code) {
            return s_errors[i].msg;
        }
    }
    return "Unknown error";
}

size_t error_code_count(void) {
    return sizeof(s_errors) / sizeof(s_errors[0]);
}
