/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file usb_hid_layout.h
 * @brief HID report-descriptor decode for a pointing device.
 *
 * Boot protocol is fixed: buttons, X, Y and nothing else, so a wheel simply
 * does not exist there. Report protocol carries whatever the descriptor
 * declares (16-bit axes, a wheel, AC Pan, report IDs, padding), which is why
 * the mouse path asks for report protocol and maps the report through this
 * decoder instead of assuming the boot byte order.
 *
 * The decoder is pure: no USB, no LVGL, no allocation. It is compiled into the
 * firmware and into the host unit lane, so the offsets are tested without a
 * mouse plugged in.
 */
#ifndef P4MINISHELL_USB_HID_LAYOUT_H
#define P4MINISHELL_USB_HID_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One field located in a decoded HID input report. `byte` already includes
 *  the report-ID byte when the descriptor declares one. */
typedef struct {
    uint8_t byte;
    uint8_t bit;
    uint8_t bits;
    uint8_t report_id;
    bool valid;
} usb_hid_field_t;

/** Mouse report layout found in the HID report descriptor. */
typedef struct {
    usb_hid_field_t buttons;
    usb_hid_field_t x;
    usb_hid_field_t y;
    usb_hid_field_t wheel;
    bool has_report_id;
    bool valid;
} usb_mouse_layout_t;

/**
 * Locate buttons/X/Y/wheel in a mouse report descriptor.
 *
 * Implements the subset of the HID item grammar a pointing device uses:
 * usage page/usage/usage min/max, report size/count/id, and Input items. The
 * bit cursor advances field by field, so a report with an ID byte, padding or
 * 16-bit axes decodes at the right offsets. On a malformed or unsupported
 * descriptor @p out is cleared and `valid` is false; the caller then falls back
 * to the boot layout (and the wheel is unavailable rather than misread).
 *
 * @param desc  Report descriptor bytes (NULL is allowed).
 * @param len   Descriptor length in bytes.
 * @param out   Layout to fill; always initialized.
 */
void usb_mouse_parse_layout(const uint8_t *desc, size_t len,
                            usb_mouse_layout_t *out);

/**
 * Extract a signed field from an input report (two's complement at the field
 * width). Returns 0 when the field is absent or the report is too short.
 */
int32_t usb_hid_field_value(const uint8_t *data, size_t len,
                            const usb_hid_field_t *field);

/** One Consumer Page (0x0C) usage field: a media key. Arrays carry the usage
 *  code as the field value; variable descriptors carry one bit per usage. */
typedef struct {
    uint8_t byte;
    uint8_t bit;
    uint8_t bits;
    uint8_t report_id;
    uint16_t usage;
    bool valid;
} usb_consumer_field_t;

typedef struct {
    usb_consumer_field_t fields[16];
    int count;
    bool is_array;       /**< one 16-bit usage code (common media-key layout) */
    bool has_report_id;
    bool valid;
} usb_consumer_layout_t;

/** Locate Consumer Page usages in a report descriptor. Supports the two
 *  common media-key shapes: a 16-bit array of usage codes, and variable
 *  one-bit usages (volume up/down/mute/play). */
void usb_consumer_parse_layout(const uint8_t *desc, size_t len,
                               usb_consumer_layout_t *out);

/** Decode the media usage in one report. Returns 0 when the report contains no
 *  (new) usage; the caller decides what to do with it. */
uint16_t usb_consumer_report_usage(const uint8_t *data, size_t len,
                                   const usb_consumer_layout_t *layout);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_USB_HID_LAYOUT_H */
