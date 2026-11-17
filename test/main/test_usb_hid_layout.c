/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_usb_hid_layout.c
 * @brief The HID report-descriptor decoder behind the USB mouse path.
 *
 * These cases pin the exact descriptor a real boot-capable mouse reports over
 * USB: five button bits, three padding bits, two 16-bit relative axes, an
 * 8-bit wheel and AC Pan. Reading the boot layout instead collapsed the wheel
 * onto a Y byte (the wheel "worked" but scrolled on every motion report) and
 * mapped both axes to Y, so the descriptor map is tested directly.
 */

#include "unity.h"
#include "usb_hid_layout.h"
#include <string.h>

/** BITMAPINFOHEADER-style reality check: this is the descriptor captured from
 *  the ADATA/Logitech-class mouse on COM3 (71 bytes). */
static const uint8_t s_mouse_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01,
    0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x05,
    0x81, 0x02, 0x95, 0x03, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x16, 0x00, 0x80, 0x26,
    0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x80, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06, 0x05, 0x0C, 0x0A, 0x38,
    0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};

/** The captured descriptor maps to buttons byte 0, X byte 1 (16-bit), Y byte
 *  3 (16-bit), wheel byte 5 (8-bit). */
void test_usb_hid_layout_real_mouse(void)
{
    usb_mouse_layout_t layout;

    usb_mouse_parse_layout(s_mouse_desc, sizeof(s_mouse_desc), &layout);
    TEST_ASSERT_TRUE(layout.valid);
    TEST_ASSERT_FALSE(layout.has_report_id);
    TEST_ASSERT_TRUE(layout.buttons.valid);
    TEST_ASSERT_EQUAL_INT(0, layout.buttons.byte);
    TEST_ASSERT_EQUAL_INT(5, layout.buttons.bits);
    TEST_ASSERT_EQUAL_INT(1, layout.x.byte);
    TEST_ASSERT_EQUAL_INT(16, layout.x.bits);
    TEST_ASSERT_EQUAL_INT(3, layout.y.byte);
    TEST_ASSERT_EQUAL_INT(16, layout.y.bits);
    TEST_ASSERT_EQUAL_INT(5, layout.wheel.byte);
    TEST_ASSERT_EQUAL_INT(8, layout.wheel.bits);
    TEST_ASSERT_TRUE(layout.wheel.valid);
}

/** A full report decodes with the right sign on every field. */
void test_usb_hid_layout_extract_report(void)
{
    usb_mouse_layout_t layout;
    const uint8_t report[] = {
        0x02,             /* right button down */
        0x0A, 0x00,       /* X = +10 */
        0xF6, 0xFF,       /* Y = -10 */
        0xFF,             /* wheel = -1 (scroll down) */
        0x00,             /* AC Pan */
    };

    usb_mouse_parse_layout(s_mouse_desc, sizeof(s_mouse_desc), &layout);
    TEST_ASSERT_EQUAL_INT(2, usb_hid_field_value(report, sizeof(report),
                                                 &layout.buttons));
    TEST_ASSERT_EQUAL_INT(10, usb_hid_field_value(report, sizeof(report),
                                                  &layout.x));
    TEST_ASSERT_EQUAL_INT(-10, usb_hid_field_value(report, sizeof(report),
                                                   &layout.y));
    TEST_ASSERT_EQUAL_INT(-1, usb_hid_field_value(report, sizeof(report),
                                                  &layout.wheel));
}

/** The full 16-bit range survives (no 8-bit truncation of a fast flick). */
void test_usb_hid_layout_extract_axis_extremes(void)
{
    usb_mouse_layout_t layout;
    const uint8_t report[] = { 0x00, 0x00, 0x80, 0xFF, 0x7F, 0x7F };

    usb_mouse_parse_layout(s_mouse_desc, sizeof(s_mouse_desc), &layout);
    TEST_ASSERT_EQUAL_INT(-32768, usb_hid_field_value(report, sizeof(report),
                                                      &layout.x));
    TEST_ASSERT_EQUAL_INT(32767, usb_hid_field_value(report, sizeof(report),
                                                     &layout.y));
    TEST_ASSERT_EQUAL_INT(127, usb_hid_field_value(report, sizeof(report),
                                                   &layout.wheel));
    /* A truncated report yields 0 rather than reading past its end. */
    TEST_ASSERT_EQUAL_INT(0, usb_hid_field_value(report, 2, &layout.y));
}

/** A boot-style descriptor without a wheel stays valid but reports no wheel,
 *  so the caller falls back to the documented "wheel unavailable" path. */
void test_usb_hid_layout_without_wheel(void)
{
    static const uint8_t desc[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01,
        0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03,
        0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0x05, 0x01,
        0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
        0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
    };
    usb_mouse_layout_t layout;

    usb_mouse_parse_layout(desc, sizeof(desc), &layout);
    TEST_ASSERT_TRUE(layout.valid);
    TEST_ASSERT_FALSE(layout.wheel.valid);
    TEST_ASSERT_EQUAL_INT(1, layout.x.byte);
    TEST_ASSERT_EQUAL_INT(2, layout.y.byte);
}

/** With a Report ID the first byte of every report is the ID, and the field
 *  offsets shift by one. A decoder that ignores it reads the ID as buttons. */
void test_usb_hid_layout_report_id(void)
{
    static const uint8_t desc[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,
        0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
        0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
        0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
        0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
        0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
        0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
    };
    usb_mouse_layout_t layout;
    const uint8_t report[] = { 0x01, 0x01, 0x05, 0xFB, 0xFF };

    usb_mouse_parse_layout(desc, sizeof(desc), &layout);
    TEST_ASSERT_TRUE(layout.valid);
    TEST_ASSERT_TRUE(layout.has_report_id);
    TEST_ASSERT_EQUAL_INT(1, layout.x.report_id);
    TEST_ASSERT_EQUAL_INT(1, layout.buttons.byte);
    TEST_ASSERT_EQUAL_INT(2, layout.x.byte);
    TEST_ASSERT_EQUAL_INT(3, layout.y.byte);
    TEST_ASSERT_EQUAL_INT(4, layout.wheel.byte);
    TEST_ASSERT_EQUAL_INT(1, usb_hid_field_value(report, sizeof(report),
                                                 &layout.buttons));
    TEST_ASSERT_EQUAL_INT(5, usb_hid_field_value(report, sizeof(report),
                                                 &layout.x));
    TEST_ASSERT_EQUAL_INT(-5, usb_hid_field_value(report, sizeof(report),
                                                  &layout.y));
    TEST_ASSERT_EQUAL_INT(-1, usb_hid_field_value(report, sizeof(report),
                                                  &layout.wheel));
}

/** Truncated or absent descriptors are rejected without reading past the end. */
void test_usb_hid_layout_rejects_bad_descriptor(void)
{
    usb_mouse_layout_t layout;
    static const uint8_t truncated[] = { 0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
                                         0x75, 0x10, 0x95, 0x02 };

    usb_mouse_parse_layout(NULL, 0, &layout);
    TEST_ASSERT_FALSE(layout.valid);
    usb_mouse_parse_layout(truncated, sizeof(truncated), &layout);
    TEST_ASSERT_FALSE(layout.valid);
    TEST_ASSERT_FALSE(layout.x.valid);
    TEST_ASSERT_FALSE(layout.y.valid);
}

/** Media keys: the common 16-bit Consumer Page array (report ID 2). */
void test_usb_consumer_array_layout(void)
{
    static const uint8_t desc[] = {
        0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
        0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A,
        0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
        0xC0,
    };
    usb_consumer_layout_t layout;
    const uint8_t press[] = { 0x02, 0xCD, 0x00 };
    const uint8_t release[] = { 0x02, 0x00, 0x00 };

    usb_consumer_parse_layout(desc, sizeof(desc), &layout);
    TEST_ASSERT_TRUE(layout.valid);
    TEST_ASSERT_TRUE(layout.is_array);
    TEST_ASSERT_TRUE(layout.has_report_id);
    TEST_ASSERT_EQUAL_INT(2, layout.fields[0].report_id);
    TEST_ASSERT_EQUAL_INT(1, layout.fields[0].byte);
    TEST_ASSERT_EQUAL_INT(16, layout.fields[0].bits);
    TEST_ASSERT_EQUAL_HEX16(0x00CD, usb_consumer_report_usage(press, sizeof(press), &layout));
    TEST_ASSERT_EQUAL_HEX16(0x0000, usb_consumer_report_usage(release, sizeof(release), &layout));
}

/** Variable media keys: one bit per usage (volume up/down, mute). */
void test_usb_consumer_variable_layout(void)
{
    static const uint8_t desc[] = {
        0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x05, 0x0C,
        0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2, 0x15, 0x00,
        0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
        0x95, 0x05, 0x81, 0x01, 0xC0,
    };
    usb_consumer_layout_t layout;
    const uint8_t up[] = { 0x01 };
    const uint8_t down[] = { 0x02 };
    const uint8_t mute[] = { 0x04 };
    const uint8_t idle[] = { 0x00 };

    usb_consumer_parse_layout(desc, sizeof(desc), &layout);
    TEST_ASSERT_TRUE(layout.valid);
    TEST_ASSERT_FALSE(layout.is_array);
    TEST_ASSERT_EQUAL_INT(3, layout.count);
    TEST_ASSERT_EQUAL_HEX16(0x00E9, usb_consumer_report_usage(up, sizeof(up), &layout));
    TEST_ASSERT_EQUAL_HEX16(0x00EA, usb_consumer_report_usage(down, sizeof(down), &layout));
    TEST_ASSERT_EQUAL_HEX16(0x00E2, usb_consumer_report_usage(mute, sizeof(mute), &layout));
    TEST_ASSERT_EQUAL_HEX16(0x0000, usb_consumer_report_usage(idle, sizeof(idle), &layout));
}

/** Truncated/absent descriptors are rejected without out-of-bounds reads. */
void test_usb_consumer_rejects_bad_descriptor(void)
{
    usb_consumer_layout_t layout;
    static const uint8_t truncated[] = { 0x05, 0x0C, 0x75, 0x10 };

    usb_consumer_parse_layout(NULL, 0, &layout);
    TEST_ASSERT_FALSE(layout.valid);
    usb_consumer_parse_layout(truncated, sizeof(truncated), &layout);
    TEST_ASSERT_FALSE(layout.valid);
    TEST_ASSERT_EQUAL_HEX16(0, usb_consumer_report_usage(truncated, 1, &layout));
}
