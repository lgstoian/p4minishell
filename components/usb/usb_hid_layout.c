/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file usb_hid_layout.c
 * @brief Pure HID report-descriptor decode (see usb_hid_layout.h).
 *
 * Boot protocol carries buttons + X + Y and no wheel, so the pointing-device
 * path asks for report protocol and maps reports through this decoder. It is
 * the MODEL half: no USB host calls, no LVGL, no allocation, which is why the
 * host unit lane runs the exact TU the firmware compiles.
 *
 * @module usb
 * @layer leaf
 * @domain io
 * @provides HID report-descriptor field map and signed field extraction
 * @status stable
 * @test test/main/test_usb_hid_layout.c
 * @hard-rule ai-context.md#module-layering-rules
 */
#include "usb_hid_layout.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* HID report-descriptor decode (mouse report protocol)                    */
/* ---------------------------------------------------------------------- */

static void usb_hid_field_set(usb_hid_field_t *field, uint32_t bit_cursor,
                              bool has_report_id, uint8_t report_id,
                              uint8_t width, uint8_t bit_offset)
{
    field->byte = (uint8_t)((has_report_id ? 1u : 0u) + bit_cursor / 8u);
    field->bit = bit_offset;
    field->bits = width;
    field->report_id = report_id;
    field->valid = true;
}

/** Locate buttons/X/Y/wheel in a mouse descriptor.
 *
 *  Implements the subset of the HID item grammar a pointing device uses:
 *  usage page/usage/usage min/max, report size/count/id, and Input items.
 *  The bit cursor advances field by field, so a report with an ID byte,
 *  padding, or 16-bit axes is decoded at the right offsets instead of being
 *  assumed to be the 3-byte boot layout. */
void usb_mouse_parse_layout(const uint8_t *desc, size_t len,
                                   usb_mouse_layout_t *out)
{
    uint32_t usage_page = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    uint32_t usages[16];
    uint8_t usage_count = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint32_t report_id = 0;
    uint32_t bit_cursor = 0;
    bool any_report_id = false;
    size_t pos = 0;

    memset(out, 0, sizeof(*out));
    memset(usages, 0, sizeof(usages));
    if (desc == NULL || len == 0) {
        return;
    }

    while (pos < len) {
        uint8_t prefix = desc[pos++];
        uint8_t size_code;
        uint8_t size;
        uint8_t type;
        uint8_t tag;
        uint32_t value = 0;

        if (prefix == 0xFE) {              /* long item: skip payload */
            uint8_t data_size;
            if (pos + 2 > len) {
                break;
            }
            data_size = desc[pos++];
            pos++;                         /* long item tag */
            if (pos + data_size > len) {
                break;
            }
            pos += data_size;
            continue;
        }
        size_code = (uint8_t)(prefix & 0x03u);
        size = (size_code == 3u) ? 4u : size_code;
        type = (uint8_t)((prefix >> 2) & 0x03u);
        tag = (uint8_t)((prefix >> 4) & 0x0Fu);
        if (pos + size > len) {
            break;
        }
        for (uint8_t b = 0; b < size; b++) {
            value |= (uint32_t)desc[pos + b] << (8u * b);
        }
        pos += size;

        if (type == 1u) {                  /* Global */
            switch (tag) {
            case 0x0:
                usage_page = value;
                break;
            case 0x7:
                report_size = value;
                break;
            case 0x8:
                report_id = value;
                any_report_id = true;
                bit_cursor = 0;            /* each report ID restarts */
                break;
            case 0x9:
                report_count = value;
                break;
            default:
                break;
            }
        } else if (type == 2u) {           /* Local */
            if (tag == 0x0) {
                if (usage_count < sizeof(usages) / sizeof(usages[0])) {
                    usages[usage_count++] = value & 0xFFFFu;
                }
            } else if (tag == 0x1) {
                usage_min = value & 0xFFFFu;
            } else if (tag == 0x2) {
                usage_max = value & 0xFFFFu;
            }
        } else if (type == 0u && tag == 0x8u) {  /* Input */
            uint32_t first_bit = bit_cursor;
            uint8_t first_bit_off = (uint8_t)(first_bit % 8u);

            for (uint32_t j = 0; j < report_count; j++) {
                uint32_t u;

                if (usage_min != 0 && usage_max != 0) {
                    u = usage_min + j;
                } else if (j < usage_count) {
                    /* One Usage per field, in order (09 30 09 31 with a count
                     * of 2 is X then Y - not Y twice). */
                    u = usages[j];
                } else if (usage_count > 0) {
                    u = usages[usage_count - 1];
                } else {
                    u = 0;
                }

                if (usage_page == 0x09u) {             /* Button page */
                    if (!out->buttons.valid) {
                        usb_hid_field_set(&out->buttons, first_bit,
                                          any_report_id,
                                          (uint8_t)report_id,
                                          (uint8_t)(report_size * report_count > 8u
                                                        ? 8u
                                                        : report_size * report_count),
                                          first_bit_off);
                    }
                } else if (usage_page == 0x01u) {      /* Generic Desktop */
                    if (u == 0x30u && !out->x.valid) {
                        usb_hid_field_set(&out->x, bit_cursor, any_report_id,
                                          (uint8_t)report_id,
                                          (uint8_t)report_size,
                                          (uint8_t)(bit_cursor % 8u));
                    } else if (u == 0x31u && !out->y.valid) {
                        usb_hid_field_set(&out->y, bit_cursor, any_report_id,
                                          (uint8_t)report_id,
                                          (uint8_t)report_size,
                                          (uint8_t)(bit_cursor % 8u));
                    } else if (u == 0x38u && !out->wheel.valid) {
                        usb_hid_field_set(&out->wheel, bit_cursor, any_report_id,
                                          (uint8_t)report_id,
                                          (uint8_t)report_size,
                                          (uint8_t)(bit_cursor % 8u));
                    }
                }
                bit_cursor += report_size;
            }
            usage_count = 0;
            usage_min = 0;
            usage_max = 0;
        } else if (type == 0u && tag == 0x9u) {  /* Output occupies bits too */
            bit_cursor += report_size * report_count;
        } else if (type == 0u) {                 /* Collection / End */
            usage_count = 0;
            usage_min = 0;
            usage_max = 0;
        }
    }

    out->has_report_id = any_report_id;
    out->valid = out->x.valid && out->y.valid;
}

/** Extract a signed field; values are two's complement at the field width. */
int32_t usb_hid_field_value(const uint8_t *data, size_t len,
                                   const usb_hid_field_t *field)
{
    uint64_t raw = 0;
    uint8_t bits;
    size_t bytes;

    if (!field->valid) {
        return 0;
    }
    bits = field->bits;
    if (bits == 0 || bits > 32) {
        return 0;
    }
    bytes = ((size_t)field->bit + (size_t)bits + 7u) / 8u;
    if (bytes > 8u || (size_t)field->byte + bytes > len) {
        return 0;
    }
    for (size_t i = 0; i < bytes; i++) {
        raw |= (uint64_t)data[field->byte + i] << (8u * i);
    }
    if (field->bit != 0) {
        raw >>= field->bit;
    }
    if (bits < 32u) {
        uint32_t mask = (1u << bits) - 1u;
        uint32_t value = (uint32_t)raw & mask;

        if ((value & (1u << (bits - 1u))) != 0) {
            return (int32_t)(value | ~mask);
        }
        return (int32_t)value;
    }
    return (int32_t)(uint32_t)raw;
}

