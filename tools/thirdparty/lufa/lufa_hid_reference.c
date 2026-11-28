/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file lufa_hid_reference.c
 * @brief Pure byte builders for the canonical boot descriptors (host only).
 * See lufa_hid_reference.h for the idea/provenance and the tag block.
 */
#include "lufa_hid_reference.h"

#include "lufa_hid_constants.h"

/* Item tag bits, in our own naming, from the tag arguments of LUFA's
 * HID_RI_* template macros. Values are HID-spec bytes, names are ours. */
enum {
    REF_TAG_USAGE_PAGE    = 0x00,
    REF_TAG_USAGE         = 0x00,
    REF_TAG_USAGE_MIN     = 0x10,
    REF_TAG_USAGE_MAX     = 0x20,
    REF_TAG_COLLECTION    = 0xA0,
    REF_TAG_LOGICAL_MIN   = 0x10,
    REF_TAG_LOGICAL_MAX   = 0x20,
    REF_TAG_PHYSICAL_MIN  = 0x30,
    REF_TAG_PHYSICAL_MAX  = 0x40,
    REF_TAG_REPORT_SIZE   = 0x70,
    REF_TAG_REPORT_COUNT  = 0x90,
    REF_TAG_INPUT         = 0x80,
    REF_TAG_OUTPUT        = 0x90,
    REF_TAG_END_COLLECTION = 0xC0,
};

/** One short-item prefix byte, assembled from LUFA's masks/types/sizes. */
static uint8_t ref_prefix(uint8_t type, uint8_t tag, uint8_t data_bits)
{
    return (uint8_t)((type & HID_RI_TYPE_MASK) |
                     (tag & HID_RI_TAG_MASK) |
                     (data_bits & HID_RI_DATA_SIZE_MASK));
}

/** Append helpers keep every call site bounded; no dynamic allocation. */
#define REF_PUT8(type, tag, value)                                            \
    do {                                                                      \
        if (n + 2u > cap) {                                                   \
            return n;                                                         \
        }                                                                     \
        out[n++] = ref_prefix((type), (tag), HID_RI_DATA_BITS_8);             \
        out[n++] = (uint8_t)(value);                                          \
    } while (0)

#define REF_PUT16(type, tag, value)                                           \
    do {                                                                      \
        uint16_t ref_value_ = (uint16_t)(value);                              \
        if (n + 3u > cap) {                                                   \
            return n;                                                         \
        }                                                                     \
        out[n++] = ref_prefix((type), (tag), HID_RI_DATA_BITS_16);            \
        out[n++] = (uint8_t)(ref_value_ & 0xFFu);                             \
        out[n++] = (uint8_t)(ref_value_ >> 8);                                \
    } while (0)

size_t lufa_ref_mouse_descriptor(uint8_t *out, size_t cap)
{
    size_t n = 0;

    if (out == NULL) {
        return 0;
    }
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_USAGE_PAGE, 0x01);   /* Generic Desktop */
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE, 0x02);         /* Mouse */
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_COLLECTION, 0x01);     /* Application */
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE, 0x01);         /* Pointer */
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_COLLECTION, 0x00);     /* Physical */
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_USAGE_PAGE, 0x09);   /* Button */
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE_MIN, 0x01);
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE_MAX, 0x05);     /* 5 buttons */
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_LOGICAL_MIN, 0x00);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_LOGICAL_MAX, 0x01);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_COUNT, 0x05);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_SIZE, 0x01);
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_INPUT,
             HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_COUNT, 0x01);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_SIZE, 0x03);  /* padding */
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_INPUT, HID_IOF_CONSTANT);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_USAGE_PAGE, 0x01);   /* Generic Desktop */
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE, 0x30);         /* X */
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE, 0x31);         /* Y */
    REF_PUT16(HID_RI_TYPE_GLOBAL, REF_TAG_LOGICAL_MIN, 0x8000);  /* -32768 */
    REF_PUT16(HID_RI_TYPE_GLOBAL, REF_TAG_LOGICAL_MAX, 0x7FFF);
    REF_PUT16(HID_RI_TYPE_GLOBAL, REF_TAG_PHYSICAL_MIN, 0x0000);
    REF_PUT16(HID_RI_TYPE_GLOBAL, REF_TAG_PHYSICAL_MAX, 0x0000);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_COUNT, 0x02);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_SIZE, 0x10);  /* 16-bit axes */
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_INPUT,
             HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_RELATIVE);
    if (n + 2u > cap) {
        return n;
    }
    out[n++] = ref_prefix(HID_RI_TYPE_MAIN, REF_TAG_END_COLLECTION,
                          HID_RI_DATA_BITS_0);
    out[n++] = ref_prefix(HID_RI_TYPE_MAIN, REF_TAG_END_COLLECTION,
                          HID_RI_DATA_BITS_0);
    return n;
}

size_t lufa_ref_keyboard_descriptor(uint8_t *out, size_t cap)
{
    size_t n = 0;

    if (out == NULL) {
        return 0;
    }
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_USAGE_PAGE, 0x01);
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE, 0x06);         /* Keyboard */
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_COLLECTION, 0x01);

    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_USAGE_PAGE, 0x07);   /* Keyboard/Keypad */
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE_MIN, 0xE0);
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE_MAX, 0xE7);     /* 8 modifiers */
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_LOGICAL_MIN, 0x00);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_LOGICAL_MAX, 0x01);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_SIZE, 0x01);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_COUNT, 0x08);
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_INPUT,
             HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_COUNT, 0x01);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_SIZE, 0x08);
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_INPUT, HID_IOF_CONSTANT); /* reserved */

    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_USAGE_PAGE, 0x08);   /* LEDs */
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE_MIN, 0x01);
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE_MAX, 0x05);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_COUNT, 0x05);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_SIZE, 0x01);
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_OUTPUT,
             HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_COUNT, 0x01);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_SIZE, 0x03);
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_OUTPUT, HID_IOF_CONSTANT);

    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_LOGICAL_MIN, 0x00);
    REF_PUT16(HID_RI_TYPE_GLOBAL, REF_TAG_LOGICAL_MAX, 0x00FF);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_USAGE_PAGE, 0x07);
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE_MIN, 0x00);
    REF_PUT8(HID_RI_TYPE_LOCAL, REF_TAG_USAGE_MAX, 0xFF);
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_COUNT, 0x06); /* key slots */
    REF_PUT8(HID_RI_TYPE_GLOBAL, REF_TAG_REPORT_SIZE, 0x08);
    REF_PUT8(HID_RI_TYPE_MAIN, REF_TAG_INPUT,
             HID_IOF_DATA | HID_IOF_ARRAY | HID_IOF_ABSOLUTE);
    if (n + 1u > cap) {
        return n;
    }
    out[n++] = ref_prefix(HID_RI_TYPE_MAIN, REF_TAG_END_COLLECTION,
                          HID_RI_DATA_BITS_0);
    return n;
}
