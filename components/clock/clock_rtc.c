/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file clock_rtc.c
 * @brief RTC backup for the system clock: NVS anchor + optional ext. chip.
 *
 * The ESP32-P4 keeps its RTC counter (`esp_rtc_get_time_us()`) across resets
 * and, with a VBAT coin cell, across power loss. This module bridges wall
 * time over reboots without SNTP:
 *
 *   - Every anchor stores {unix_sec, rtc_us} in the "p4rtc" NVS namespace.
 *     At boot, when the system clock is still unset, the anchor is replayed:
 *     now = anchor_unix + (rtc_now - anchor_rtc) / 1e6.
 *   - Anchors land on SNTP sync, manual `date`/`time` set, `reboot`, and a
 *     periodic esp_timer (P4_CONFIG_RTC_ANCHOR_PERIOD_S, 0 disables).
 *   - Counter continuity decides the outcome (pure clock_rtc_restore_math,
 *     unit-tested): the RTC kept counting -> RESTORED; the counter reset
 *     below the anchor (power loss without VBAT) -> STALE (last-known time
 *     is still applied, but flagged); implausible data -> INVALID (unset).
 *   - An optional external DS3231-class I2C chip (P4_CONFIG_RTC_EXT_*,
 *     default off) is read first at boot and rewritten on SNTP/manual set.
 *
 * No new tasks: the anchors ride esp_timers (whose task has an internal-RAM
 * stack). The post-SNTP anchor is deferred through a one-shot timer because
 * that callback runs on the lwIP tcpip_thread, whose stack may be in PSRAM
 * (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP), where flash/NVS writes are illegal.
 * No VBAT API exists in IDF 5.5, so backup health is inferred from counter
 * continuity and reported by `rtc status`.
 */

#include "clock.h"
#include "board_config.h"
#include "board_bsp.h"
#include "p4minishell_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rtc_time.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

#define RTC_TAG "rtc"
#define RTC_NVS_NAMESPACE "p4rtc"
#define RTC_NVS_KEY_UNIX "unix"
#define RTC_NVS_KEY_RTC "rtc"

/** Deltas above this (10 years) are implausible: refuse, don't set garbage. */
#define RTC_MAX_DELTA_US (10LL * 365 * 24 * 3600 * 1000000LL)

#ifndef P4_CONFIG_RTC_ANCHOR_PERIOD_S
#define P4_CONFIG_RTC_ANCHOR_PERIOD_S 3600
#endif

#ifndef P4_CONFIG_RTC_EXT_ENABLE
#define P4_CONFIG_RTC_EXT_ENABLE 0
#endif

/* Board profile pins win over the central defaults: board_config.h is the
 * hardware source of truth. When the board wires its RTC on the shared BSP
 * I2C bus (BOARD_CFG_RTC_USE_BSP_I2C), the driver reuses bsp_i2c_get_handle()
 * instead of creating a second master bus on the same pins. */
#ifdef BOARD_CFG_RTC_EXT_ENABLE
#define RTC_EXT_ENABLE BOARD_CFG_RTC_EXT_ENABLE
#else
#define RTC_EXT_ENABLE P4_CONFIG_RTC_EXT_ENABLE
#endif

#ifdef BOARD_CFG_RTC_EXT_ADDR
#define RTC_EXT_ADDR BOARD_CFG_RTC_EXT_ADDR
#else
#define RTC_EXT_ADDR P4_CONFIG_RTC_EXT_ADDR
#endif

#ifdef BOARD_CFG_RTC_EXT_SDA
#define RTC_EXT_SDA BOARD_CFG_RTC_EXT_SDA
#else
#define RTC_EXT_SDA P4_CONFIG_RTC_EXT_SDA
#endif

#ifdef BOARD_CFG_RTC_EXT_SCL
#define RTC_EXT_SCL BOARD_CFG_RTC_EXT_SCL
#else
#define RTC_EXT_SCL P4_CONFIG_RTC_EXT_SCL
#endif

#ifdef BOARD_CFG_RTC_EXT_PORT
#define RTC_EXT_PORT BOARD_CFG_RTC_EXT_PORT
#else
#define RTC_EXT_PORT P4_CONFIG_RTC_EXT_PORT
#endif

#ifndef BOARD_CFG_RTC_USE_BSP_I2C
#define BOARD_CFG_RTC_USE_BSP_I2C 0
#endif

#ifndef P4_CONFIG_RTC_EXT_TIMEOUT_MS
#define P4_CONFIG_RTC_EXT_TIMEOUT_MS 50
#endif

/* Time register base: the DS3231 family maps seconds..year at 0x00..0x06; the
 * RX8130CE maps the same fields at 0x10..0x16 (WEEK sits at 0x13, which the
 * decoder skips). The board profile selects the base. */
#ifdef BOARD_CFG_RTC_EXT_TIME_REG
#define RTC_EXT_REG_TIME BOARD_CFG_RTC_EXT_TIME_REG
#else
#define RTC_EXT_REG_TIME 0x00
#endif
#define RTC_EXT_TIME_LEN 7

/* The RX8130CE has no century bit in the month register (its year is a plain
 * 00..99 BCD field, always 2000-based here) and gates the oscillator with a
 * STOP bit in control register 0x1E bit 6. */
#ifdef BOARD_CFG_RTC_EXT_KIND_RX8130
#define RTC_EXT_KIND_RX8130 BOARD_CFG_RTC_EXT_KIND_RX8130
#else
#define RTC_EXT_KIND_RX8130 0
#endif
#define RTC_EXT_RX8130_REG_CTRL 0x1E
#define RTC_EXT_RX8130_CTRL_STOP 0x40
#define RTC_EXT_RX8130_REG_FLAG 0x1D
#define RTC_EXT_RX8130_FLAG_VBLF 0x80
#define RTC_EXT_RX8130_FLAG_AF   0x08
#define RTC_EXT_RX8130_FLAG_TF   0x10

static bool s_nvs_ready = false;
static bool s_nvs_tried = false;
static const char *s_source = "none";
static bool s_stale = false;
static int64_t s_anchor_at_us = 0;
static int s_ext_present = -1; /* -1 unknown, 0 absent, 1 present */
static esp_timer_handle_t s_anchor_timer = NULL;
static esp_timer_handle_t s_anchor_defer = NULL;

/* ========================================================================
 * Pure core (unit-tested)
 * ======================================================================== */

clock_rtc_restore_t clock_rtc_restore_math(int64_t anchor_unix, uint64_t anchor_rtc,
                                           uint64_t rtc_now, int64_t valid_epoch,
                                           int64_t *unix_out)
{
    if (unix_out == NULL) {
        return CLOCK_RTC_INVALID;
    }
    *unix_out = 0;
    if (anchor_unix < valid_epoch) {
        return CLOCK_RTC_INVALID;
    }
    if (rtc_now < anchor_rtc) {
        /* The counter reset: power loss without VBAT backup. */
        *unix_out = anchor_unix;
        return CLOCK_RTC_STALE;
    }
    {
        uint64_t delta_us = rtc_now - anchor_rtc;
        if (delta_us > (uint64_t)RTC_MAX_DELTA_US) {
            return CLOCK_RTC_INVALID;
        }
        *unix_out = anchor_unix + (int64_t)(delta_us / 1000000u);
    }
    if (*unix_out < valid_epoch) {
        return CLOCK_RTC_INVALID;
    }
    return CLOCK_RTC_RESTORED;
}

uint8_t clock_rtc_bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10) + (bcd & 0x0F));
}

uint8_t clock_rtc_bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

/* ========================================================================
 * NVS anchor store
 * ======================================================================== */

/** Best-effort NVS init (idempotent; networking also inits it for Wi-Fi). */
static bool clock_rtc_nvs_ready(void)
{
    if (!s_nvs_tried) {
        s_nvs_tried = true;
        s_nvs_ready = (nvs_flash_init() == ESP_OK);
        if (!s_nvs_ready) {
            ESP_LOGW(RTC_TAG, "NVS unavailable; time anchor disabled");
        }
    }
    return s_nvs_ready;
}

static bool clock_rtc_anchor_read(int64_t *unix_out, uint64_t *rtc_out)
{
    nvs_handle_t h;
    int64_t unix = 0;
    uint64_t rtc = 0;

    if (!clock_rtc_nvs_ready()) {
        return false;
    }
    if (nvs_open(RTC_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    if (nvs_get_i64(h, RTC_NVS_KEY_UNIX, &unix) != ESP_OK ||
        nvs_get_u64(h, RTC_NVS_KEY_RTC, &rtc) != ESP_OK) {
        nvs_close(h);
        return false;
    }
    nvs_close(h);
    *unix_out = unix;
    *rtc_out = rtc;
    return true;
}

static void clock_rtc_anchor_store(int64_t unix, uint64_t rtc)
{
    nvs_handle_t h;

    if (!clock_rtc_nvs_ready()) {
        return;
    }
    if (nvs_open(RTC_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_i64(h, RTC_NVS_KEY_UNIX, unix) == ESP_OK &&
        nvs_set_u64(h, RTC_NVS_KEY_RTC, rtc) == ESP_OK) {
        if (nvs_commit(h) == ESP_OK) {
            s_anchor_at_us = esp_timer_get_time();
        }
    }
    nvs_close(h);
}

/**
 * Write an anchor for the current wall time (no-op while the clock is
 * unset: anchoring the boot epoch would poison every later restore).
 */
void clock_rtc_anchor_now(void)
{
    time_t now = time(NULL);

    if (now < (time_t)P4_CONFIG_CLOCK_VALID_EPOCH) {
        return;
    }
    clock_rtc_anchor_store((int64_t)now, esp_rtc_get_time_us());
}

static void clock_rtc_anchor_defer_cb(void *arg)
{
    (void)arg;
    clock_rtc_anchor_now();
}

/**
 * Persist the current wall time without writing flash on the caller's stack.
 *
 * `clock_rtc_note_synced()` runs from the SNTP notification callback, which
 * executes on the lwIP `tcpip_thread`. With SPIRAM Wi-Fi/LWIP allocation that
 * thread's stack lives in PSRAM, and an NVS write disables the flash cache,
 * which the cache-disabled code path asserts must not run on an external-RAM
 * stack (`esp_task_stack_is_sane_cache_disabled`). Hand the write to the
 * esp_timer task, which always uses an internal-RAM stack. Falls back to a
 * synchronous write when the timer is unavailable.
 */
static void clock_rtc_anchor_async(void)
{
    if (s_anchor_defer != NULL) {
        (void)esp_timer_start_once(s_anchor_defer, 1000); /* 1 ms */
    } else {
        clock_rtc_anchor_now();
    }
}

/** Note a trustworthy time source (SNTP sync or manual set). */
void clock_rtc_note_synced(const char *source)
{
    s_stale = false;
    s_source = (source != NULL) ? source : "manual";
    clock_rtc_anchor_async();
}

/* ========================================================================
 * External DS3231-class RTC (transient bus, default off)
 * ======================================================================== */

static bool clock_rtc_ext_configured(void)
{
    if (RTC_EXT_ENABLE == 0) {
        return false;
    }
#if BOARD_CFG_RTC_USE_BSP_I2C
    /* The RTC shares the board's BSP I2C bus (pins owned by the BSP). */
    return true;
#else
    return RTC_EXT_SDA >= 0 && RTC_EXT_SCL >= 0;
#endif
}

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool owns_bus;  /* true only when this session created the master bus */
    bool open;
} clock_rtc_ext_session_t;

static esp_err_t clock_rtc_ext_open(clock_rtc_ext_session_t *s)
{
    i2c_device_config_t dev_cfg;

    memset(s, 0, sizeof(*s));
    if (!clock_rtc_ext_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

#if BOARD_CFG_RTC_USE_BSP_I2C
    /* Reuse the board's shared I2C master bus; never create a second master on
     * the same pins, and never delete a bus this session did not create. */
    s->bus = bsp_i2c_get_handle();
    s->owns_bus = false;
    if (s->bus == NULL) {
        return ESP_FAIL;
    }
#else
    {
        i2c_master_bus_config_t bus_cfg;
        memset(&bus_cfg, 0, sizeof(bus_cfg));
        bus_cfg.i2c_port = RTC_EXT_PORT;
        bus_cfg.sda_io_num = (gpio_num_t)RTC_EXT_SDA;
        bus_cfg.scl_io_num = (gpio_num_t)RTC_EXT_SCL;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.trans_queue_depth = 1;
        bus_cfg.flags.enable_internal_pullup = true;
        if (i2c_new_master_bus(&bus_cfg, &s->bus) != ESP_OK) {
            return ESP_FAIL;
        }
        s->owns_bus = true;
    }
#endif

    memset(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = RTC_EXT_ADDR;
    dev_cfg.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(s->bus, &dev_cfg, &s->dev) != ESP_OK) {
        if (s->owns_bus) {
            i2c_del_master_bus(s->bus);
        }
        s->bus = NULL;
        return ESP_FAIL;
    }
    s->open = true;
    return ESP_OK;
}

static void clock_rtc_ext_close(clock_rtc_ext_session_t *s)
{
    if (s->dev != NULL) {
        i2c_master_bus_rm_device(s->dev);
        s->dev = NULL;
    }
    if (s->bus != NULL && s->owns_bus) {
        i2c_del_master_bus(s->bus);
    }
    s->bus = NULL;
    s->open = false;
}

static bool clock_rtc_ext_decode(const uint8_t regs[RTC_EXT_TIME_LEN], struct tm *out)
{
    unsigned sec = clock_rtc_bcd_to_bin(regs[0] & 0x7F);
    unsigned min = clock_rtc_bcd_to_bin(regs[1] & 0x7F);
    unsigned hour;
    unsigned date;
    unsigned mon;
    unsigned year;
    int century = 0;

    if ((regs[2] & 0x40) != 0) {
        /* 12-hour mode: bit5 is PM, low nibble+bit4 the hour. */
        hour = clock_rtc_bcd_to_bin(regs[2] & 0x1F);
        if ((regs[2] & 0x20) != 0) {
            hour = (hour % 12) + 12;
        } else if (hour == 12) {
            hour = 0;
        }
    } else {
        hour = clock_rtc_bcd_to_bin(regs[2] & 0x3F);
    }
    date = clock_rtc_bcd_to_bin(regs[4] & 0x3F);
    mon = clock_rtc_bcd_to_bin(regs[5] & 0x1F);
    if ((regs[5] & 0x80) != 0) {
        century = 100;
    }
#if RTC_EXT_KIND_RX8130
    century = 100;   /* RX8130CE year is 00..99 BCD, always 2000-based */
#endif
    year = clock_rtc_bcd_to_bin(regs[6]);
    if (sec > 59 || min > 59 || hour > 23 || date < 1 || date > 31 ||
        mon < 1 || mon > 12) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec = (int)sec;
    out->tm_min = (int)min;
    out->tm_hour = (int)hour;
    out->tm_mday = (int)date;
    out->tm_mon = (int)mon - 1;
    /* Chip years are 00-99; the century bit selects 2000s over 1900s. */
    out->tm_year = (century != 0) ? (100 + (int)year) : (int)year;
    out->tm_isdst = -1;
    return true;
}

/**
 * Read wall time from the external chip (device-local, via mktime).
 * Caches presence: -1 unknown, 0 absent, 1 present.
 */
esp_err_t clock_rtc_ext_read(time_t *unix_out)
{
    clock_rtc_ext_session_t s;
    uint8_t reg = RTC_EXT_REG_TIME;
    uint8_t regs[RTC_EXT_TIME_LEN];
    struct tm tmv;
    time_t stamp;

    if (unix_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (clock_rtc_ext_open(&s) != ESP_OK) {
        if (s_ext_present != 0) {
            s_ext_present = 0;
        }
        return ESP_FAIL;
    }
    if (i2c_master_transmit_receive(s.dev, &reg, 1, regs, sizeof(regs),
                                    P4_CONFIG_RTC_EXT_TIMEOUT_MS) != ESP_OK ||
        !clock_rtc_ext_decode(regs, &tmv)) {
        clock_rtc_ext_close(&s);
        s_ext_present = 0;
        return ESP_FAIL;
    }
    clock_rtc_ext_close(&s);
    stamp = mktime(&tmv);
    if (stamp == (time_t)-1) {
        s_ext_present = 0;
        return ESP_FAIL;
    }
    s_ext_present = 1;
    *unix_out = stamp;
    return ESP_OK;
}

/** Push wall time to the external chip (best-effort, errors ignored). */
void clock_rtc_ext_write(time_t unix)
{
    clock_rtc_ext_session_t s;
    struct tm tmv;
    uint8_t buf[1 + RTC_EXT_TIME_LEN];
    int year;

    if (!clock_rtc_ext_configured()) {
        return;
    }
    if (localtime_r(&unix, &tmv) == NULL) {
        return;
    }
    year = tmv.tm_year + 1900;
    buf[0] = RTC_EXT_REG_TIME;
    buf[1] = clock_rtc_bin_to_bcd((uint8_t)tmv.tm_sec);
    buf[2] = clock_rtc_bin_to_bcd((uint8_t)tmv.tm_min);
    buf[3] = clock_rtc_bin_to_bcd((uint8_t)tmv.tm_hour); /* 24-hour mode */
#if RTC_EXT_KIND_RX8130
    /* RX8130CE weekday is one-hot (bit0=Sunday .. bit6=Saturday). */
    buf[4] = (uint8_t)(1u << ((unsigned)tmv.tm_wday & 7u));
#else
    buf[4] = clock_rtc_bin_to_bcd((uint8_t)((tmv.tm_wday + 6) % 7 + 1));
#endif
    buf[5] = clock_rtc_bin_to_bcd((uint8_t)tmv.tm_mday);
    buf[6] = clock_rtc_bin_to_bcd((uint8_t)tmv.tm_mon + 1);
#if RTC_EXT_KIND_RX8130
    /* No century bit on the RX8130CE; years are 2000-based. */
    year = (year >= 2000) ? (year - 2000) : (year - 1900);
#else
    if (year >= 2000) {
        buf[6] |= 0x80; /* century bit */
        year -= 2000;
    } else {
        year -= 1900;
    }
#endif
    buf[7] = clock_rtc_bin_to_bcd((uint8_t)year);
    if (clock_rtc_ext_open(&s) != ESP_OK) {
        return;
    }
#if RTC_EXT_KIND_RX8130
    /* Make sure the oscillator is running (clear STOP) before loading time. */
    {
        uint8_t ctrl_reg = RTC_EXT_RX8130_REG_CTRL;
        uint8_t ctrl = 0;

        if (i2c_master_transmit_receive(s.dev, &ctrl_reg, 1, &ctrl, 1,
                                        P4_CONFIG_RTC_EXT_TIMEOUT_MS) == ESP_OK) {
            ctrl &= (uint8_t)~RTC_EXT_RX8130_CTRL_STOP;
            uint8_t ctrl_buf[2] = { RTC_EXT_RX8130_REG_CTRL, ctrl };
            (void)i2c_master_transmit(s.dev, ctrl_buf, sizeof(ctrl_buf),
                                      P4_CONFIG_RTC_EXT_TIMEOUT_MS);
        }
    }
#endif
    if (i2c_master_transmit(s.dev, buf, sizeof(buf),
                            P4_CONFIG_RTC_EXT_TIMEOUT_MS) == ESP_OK) {
        s_ext_present = 1;
    }
#if RTC_EXT_KIND_RX8130
    /* Clear stale alarm/timer IRQ flags; VBLF is diagnostic and left intact. */
    {
        uint8_t flag_reg = RTC_EXT_RX8130_REG_FLAG;
        uint8_t raw = 0;

        if (i2c_master_transmit_receive(s.dev, &flag_reg, 1, &raw, 1,
                                        P4_CONFIG_RTC_EXT_TIMEOUT_MS) == ESP_OK) {
            uint8_t cleared = (uint8_t)(raw & (uint8_t)~(RTC_EXT_RX8130_FLAG_AF | RTC_EXT_RX8130_FLAG_TF));
            uint8_t flag_buf[2] = { RTC_EXT_RX8130_REG_FLAG, cleared };
            (void)i2c_master_transmit(s.dev, flag_buf, sizeof(flag_buf),
                                      P4_CONFIG_RTC_EXT_TIMEOUT_MS);
        }
    }
#endif
    clock_rtc_ext_close(&s);
}

#if RTC_EXT_KIND_RX8130
static unsigned clock_rtc_rx8130_flags_to_api(uint8_t raw)
{
    unsigned flags = 0;

    if ((raw & RTC_EXT_RX8130_FLAG_VBLF) != 0) {
        flags |= CLOCK_RTC_FLAG_VBLF;
    }
    if ((raw & RTC_EXT_RX8130_FLAG_AF) != 0) {
        flags |= CLOCK_RTC_FLAG_AF;
    }
    if ((raw & RTC_EXT_RX8130_FLAG_TF) != 0) {
        flags |= CLOCK_RTC_FLAG_TF;
    }
    return flags;
}

static uint8_t clock_rtc_rx8130_flags_to_reg(unsigned flags)
{
    uint8_t raw = 0;

    if ((flags & CLOCK_RTC_FLAG_VBLF) != 0) {
        raw |= RTC_EXT_RX8130_FLAG_VBLF;
    }
    if ((flags & CLOCK_RTC_FLAG_AF) != 0) {
        raw |= RTC_EXT_RX8130_FLAG_AF;
    }
    if ((flags & CLOCK_RTC_FLAG_TF) != 0) {
        raw |= RTC_EXT_RX8130_FLAG_TF;
    }
    return raw;
}
#endif

esp_err_t clock_rtc_ext_flags(unsigned *flags_out)
{
    if (flags_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *flags_out = 0;
#if !RTC_EXT_KIND_RX8130
    /* Only the RX8130CE exposes a status-flag register in this driver. */
    return ESP_ERR_NOT_FOUND;
#else
    {
        clock_rtc_ext_session_t s;
        uint8_t reg = RTC_EXT_RX8130_REG_FLAG;
        uint8_t raw = 0;
        esp_err_t error;

        if (clock_rtc_ext_open(&s) != ESP_OK) {
            s_ext_present = 0;
            return ESP_ERR_NOT_FOUND;
        }
        error = i2c_master_transmit_receive(s.dev, &reg, 1, &raw, 1,
                                            P4_CONFIG_RTC_EXT_TIMEOUT_MS);
        clock_rtc_ext_close(&s);
        if (error != ESP_OK) {
            s_ext_present = 0;
            return error;
        }
        s_ext_present = 1;
        *flags_out = clock_rtc_rx8130_flags_to_api(raw);
        return ESP_OK;
    }
#endif
}

esp_err_t clock_rtc_ext_clear_flags(unsigned flags)
{
#if !RTC_EXT_KIND_RX8130
    (void)flags;
    return ESP_ERR_NOT_FOUND;
#else
    clock_rtc_ext_session_t s;
    uint8_t reg = RTC_EXT_RX8130_REG_FLAG;
    uint8_t raw = 0;
    uint8_t mask = clock_rtc_rx8130_flags_to_reg(flags);
    esp_err_t error;

    if (mask == 0) {
        return ESP_OK;
    }
    if (clock_rtc_ext_open(&s) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    error = i2c_master_transmit_receive(s.dev, &reg, 1, &raw, 1,
                                        P4_CONFIG_RTC_EXT_TIMEOUT_MS);
    if (error == ESP_OK) {
        uint8_t updated = (uint8_t)(raw & (uint8_t)~mask);
        uint8_t buf[2] = { RTC_EXT_RX8130_REG_FLAG, updated };
        error = i2c_master_transmit(s.dev, buf, sizeof(buf), P4_CONFIG_RTC_EXT_TIMEOUT_MS);
    }
    clock_rtc_ext_close(&s);
    return error;
#endif
}

/* ========================================================================
 * Boot restore + status
 * ======================================================================== */

/** Apply one unix timestamp as the system clock (best-effort). */
static void clock_rtc_apply(time_t unix)
{
    struct timeval tv;

    tv.tv_sec = unix;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) == 0) {
        clock_rtc_anchor_store((int64_t)unix, esp_rtc_get_time_us());
    } else {
        ESP_LOGW(RTC_TAG, "settimeofday failed during restore");
    }
}

/**
 * Boot-time restore, called from time_init() while the clock is still
 * unset. External chip first (when configured), then the NVS anchor.
 */
void clock_rtc_restore(void)
{
    int64_t anchor_unix = 0;
    uint64_t anchor_rtc = 0;
    uint64_t rtc_now;
    int64_t unix = 0;

    if (time(NULL) >= (time_t)P4_CONFIG_CLOCK_VALID_EPOCH) {
        s_source = "preset"; /* already set (ULP, bootloader, prior init) */
        return;
    }
    if (clock_rtc_ext_configured()) {
        time_t ext = 0;
        if (clock_rtc_ext_read(&ext) == ESP_OK &&
            ext >= (time_t)P4_CONFIG_CLOCK_VALID_EPOCH) {
            s_source = "ext";
            s_stale = false;
            clock_rtc_apply(ext);
            return;
        }
        ESP_LOGW(RTC_TAG, "external RTC not responding; trying NVS anchor");
    }
    if (!clock_rtc_anchor_read(&anchor_unix, &anchor_rtc)) {
        s_source = "none";
        return;
    }
    rtc_now = esp_rtc_get_time_us();
    switch (clock_rtc_restore_math(anchor_unix, anchor_rtc, rtc_now,
                                   (int64_t)P4_CONFIG_CLOCK_VALID_EPOCH, &unix)) {
    case CLOCK_RTC_RESTORED:
        s_source = "anchor";
        s_stale = false;
        clock_rtc_apply((time_t)unix);
        break;
    case CLOCK_RTC_STALE:
        s_source = "stale";
        s_stale = true;
        clock_rtc_apply((time_t)unix);
        ESP_LOGW(RTC_TAG, "RTC counter reset (no VBAT?); last-known time applied as stale");
        break;
    case CLOCK_RTC_INVALID:
    default:
        s_source = "none";
        break;
    }
}

static void clock_rtc_timer_cb(void *arg)
{
    (void)arg;
    clock_rtc_anchor_now(); /* no-op unless the clock currently holds time */
}

/** Start the periodic anchor timer (called once from time_init). */
void clock_rtc_start_timer(void)
{
    esp_timer_create_args_t args;

    /* The deferred anchor is always created: a post-SNTP persist must not run
     * on the lwIP thread (its stack can be in PSRAM, where flash writes are
     * illegal). */
    if (s_anchor_defer == NULL) {
        memset(&args, 0, sizeof(args));
        args.callback = clock_rtc_anchor_defer_cb;
        args.name = "rtc_anchor_defer";
        if (esp_timer_create(&args, &s_anchor_defer) != ESP_OK) {
            s_anchor_defer = NULL;
        }
    }

    /* The periodic anchor is optional (P4_CONFIG_RTC_ANCHOR_PERIOD_S <= 0
     * disables it). */
    if (s_anchor_timer != NULL || P4_CONFIG_RTC_ANCHOR_PERIOD_S <= 0) {
        return;
    }
    memset(&args, 0, sizeof(args));
    args.callback = clock_rtc_timer_cb;
    args.name = "rtc_anchor";
    if (esp_timer_create(&args, &s_anchor_timer) != ESP_OK) {
        s_anchor_timer = NULL;
        return;
    }
    if (esp_timer_start_periodic(s_anchor_timer,
                                 (uint64_t)P4_CONFIG_RTC_ANCHOR_PERIOD_S * 1000000u) != ESP_OK) {
        esp_timer_delete(s_anchor_timer);
        s_anchor_timer = NULL;
    }
}

const char *clock_rtc_source(void)
{
    return s_source;
}

bool clock_rtc_is_stale(void)
{
    return s_stale;
}

uint64_t clock_rtc_counter_us(void)
{
    return esp_rtc_get_time_us();
}

int64_t clock_rtc_anchor_age_sec(void)
{
    int64_t now;

    if (s_anchor_at_us == 0) {
        return -1;
    }
    now = esp_timer_get_time();
    if (now < s_anchor_at_us) {
        return 0;
    }
    return (now - s_anchor_at_us) / 1000000;
}

int clock_rtc_ext_state(void)
{
    return s_ext_present;
}
