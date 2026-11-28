/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 * @module lufa_host
 * @layer leaf
 * @domain io
 * @provides host-only LUFA-backed validation of the shipped USB HID fixtures
 * @status stable
 * @test tools/thirdparty/lufa/lufa_hid_gate.py --self-test
 * @hard-rule Host-only: never compiled into firmware or the ESP-IDF build
 *
 * lufa_hid_fixture_check.c - host checker that USES the LUFA-derived idea of
 * canonical boot descriptors (lufa_hid_reference.c, our own pure builders)
 * and LUFA's verbatim named constants to validate existing shipped surfaces:
 *   - generates a canonical mouse descriptor and feeds it to the SHIPPED
 *     decoder (components/usb/usb_hid_layout.c);
 *   - generates a canonical boot keyboard descriptor and verifies it really
 *     has the modifier/reserved/6-key shape components/usb/usb.c consumes;
 *   - checks the captured mouse fixture against the generated prefix and
 *     decodes it with the shipped decoder;
 *   - cross-checks every USB_HID_KEY_* / USB_HID_MOD_* define in usb.c.
 * No LUFA driver, parser or macro machinery is compiled.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lufa_hid_constants.h"
#include "lufa_hid_reference.h"
#include "usb_hid_layout.h"

typedef struct {
    const char *name;
    unsigned long value;
} named_value_t;

/* Keyboard scan codes declared in components/usb/usb.c, matched to the
 * LUFA HID_KEYBOARD_SC_* constants copied verbatim into the vendored header. */
static const named_value_t kKeys[] = {
    {"USB_HID_KEY_A", HID_KEYBOARD_SC_A},
    {"USB_HID_KEY_Z", HID_KEYBOARD_SC_Z},
    {"USB_HID_KEY_1", HID_KEYBOARD_SC_1_AND_EXCLAMATION},
    {"USB_HID_KEY_9", HID_KEYBOARD_SC_9_AND_OPENING_PARENTHESIS},
    {"USB_HID_KEY_0", HID_KEYBOARD_SC_0_AND_CLOSING_PARENTHESIS},
    {"USB_HID_KEY_ENTER", HID_KEYBOARD_SC_ENTER},
    {"USB_HID_KEY_ESC", HID_KEYBOARD_SC_ESCAPE},
    {"USB_HID_KEY_BACKSPACE", HID_KEYBOARD_SC_BACKSPACE},
    {"USB_HID_KEY_TAB", HID_KEYBOARD_SC_TAB},
    {"USB_HID_KEY_SPACE", HID_KEYBOARD_SC_SPACE},
    {"USB_HID_KEY_MINUS", HID_KEYBOARD_SC_MINUS_AND_UNDERSCORE},
    {"USB_HID_KEY_EQUAL", HID_KEYBOARD_SC_EQUAL_AND_PLUS},
    {"USB_HID_KEY_LEFT_BRACE", HID_KEYBOARD_SC_OPENING_BRACKET_AND_OPENING_BRACE},
    {"USB_HID_KEY_RIGHT_BRACE", HID_KEYBOARD_SC_CLOSING_BRACKET_AND_CLOSING_BRACE},
    {"USB_HID_KEY_BACKSLASH", HID_KEYBOARD_SC_BACKSLASH_AND_PIPE},
    {"USB_HID_KEY_SEMICOLON", HID_KEYBOARD_SC_SEMICOLON_AND_COLON},
    {"USB_HID_KEY_APOSTROPHE", HID_KEYBOARD_SC_APOSTROPHE_AND_QUOTE},
    {"USB_HID_KEY_GRAVE", HID_KEYBOARD_SC_GRAVE_ACCENT_AND_TILDE},
    {"USB_HID_KEY_COMMA", HID_KEYBOARD_SC_COMMA_AND_LESS_THAN_SIGN},
    {"USB_HID_KEY_PERIOD", HID_KEYBOARD_SC_DOT_AND_GREATER_THAN_SIGN},
    {"USB_HID_KEY_SLASH", HID_KEYBOARD_SC_SLASH_AND_QUESTION_MARK},
    {"USB_HID_KEY_CAPS_LOCK", HID_KEYBOARD_SC_CAPS_LOCK},
    {"USB_HID_KEY_F1", HID_KEYBOARD_SC_F1},
    {"USB_HID_KEY_F12", HID_KEYBOARD_SC_F12},
    {"USB_HID_KEY_PRINT_SCREEN", HID_KEYBOARD_SC_PRINT_SCREEN},
    {"USB_HID_KEY_SCROLL_LOCK", HID_KEYBOARD_SC_SCROLL_LOCK},
    {"USB_HID_KEY_PAUSE", HID_KEYBOARD_SC_PAUSE},
    {"USB_HID_KEY_INSERT", HID_KEYBOARD_SC_INSERT},
    {"USB_HID_KEY_HOME", HID_KEYBOARD_SC_HOME},
    {"USB_HID_KEY_PAGE_UP", HID_KEYBOARD_SC_PAGE_UP},
    {"USB_HID_KEY_DELETE", HID_KEYBOARD_SC_DELETE},
    {"USB_HID_KEY_END", HID_KEYBOARD_SC_END},
    {"USB_HID_KEY_PAGE_DOWN", HID_KEYBOARD_SC_PAGE_DOWN},
    {"USB_HID_KEY_RIGHT", HID_KEYBOARD_SC_RIGHT_ARROW},
    {"USB_HID_KEY_LEFT", HID_KEYBOARD_SC_LEFT_ARROW},
    {"USB_HID_KEY_DOWN", HID_KEYBOARD_SC_DOWN_ARROW},
    {"USB_HID_KEY_UP", HID_KEYBOARD_SC_UP_ARROW},
    {"USB_HID_KEY_NUM_LOCK", HID_KEYBOARD_SC_NUM_LOCK},
    {"USB_HID_KEY_KP_SLASH", HID_KEYBOARD_SC_KEYPAD_SLASH},
    {"USB_HID_KEY_KP_ASTERISK", HID_KEYBOARD_SC_KEYPAD_ASTERISK},
    {"USB_HID_KEY_KP_MINUS", HID_KEYBOARD_SC_KEYPAD_MINUS},
    {"USB_HID_KEY_KP_PLUS", HID_KEYBOARD_SC_KEYPAD_PLUS},
    {"USB_HID_KEY_KP_ENTER", HID_KEYBOARD_SC_KEYPAD_ENTER},
    {"USB_HID_KEY_KP_1", HID_KEYBOARD_SC_KEYPAD_1_AND_END},
    {"USB_HID_KEY_KP_9", HID_KEYBOARD_SC_KEYPAD_9_AND_PAGE_UP},
    {"USB_HID_KEY_KP_0", HID_KEYBOARD_SC_KEYPAD_0_AND_INSERT},
    {"USB_HID_KEY_KP_PERIOD", HID_KEYBOARD_SC_KEYPAD_DOT_AND_DELETE},
};

static const named_value_t kMods[] = {
    {"USB_HID_MOD_LEFT_CTRL", HID_KEYBOARD_MODIFIER_LEFTCTRL},
    {"USB_HID_MOD_LEFT_SHIFT", HID_KEYBOARD_MODIFIER_LEFTSHIFT},
    {"USB_HID_MOD_LEFT_ALT", HID_KEYBOARD_MODIFIER_LEFTALT},
    {"USB_HID_MOD_LEFT_GUI", HID_KEYBOARD_MODIFIER_LEFTGUI},
    {"USB_HID_MOD_RIGHT_CTRL", HID_KEYBOARD_MODIFIER_RIGHTCTRL},
    {"USB_HID_MOD_RIGHT_SHIFT", HID_KEYBOARD_MODIFIER_RIGHTSHIFT},
    {"USB_HID_MOD_RIGHT_ALT", HID_KEYBOARD_MODIFIER_RIGHTALT},
    {"USB_HID_MOD_RIGHT_GUI", HID_KEYBOARD_MODIFIER_RIGHTGUI},
};

static char *read_all(const char *path)
{
    FILE *fp = fopen(path, "rb");
    char *buf;
    long length;
    size_t got;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)length + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    got = fread(buf, 1u, (size_t)length, fp);
    fclose(fp);
    if (got != (size_t)length) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    return buf;
}

static int expect(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "LUFA CHECK FAIL %s\n", what);
        return -1;
    }
    return 0;
}

/* Compare every #define NAME 0xNN in the shipped source against its LUFA
 * counterpart. A missing name is a failure (no false green on an empty file). */
static int check_defines(const char *text, const named_value_t *map, size_t count,
                         const char *label, size_t *found)
{
    const char *cursor = text;
    size_t hits = 0;

    while (cursor != NULL && *cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        size_t len = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        char line[160];
        const char *word;
        const char *value;
        size_t name_len;

        if (len >= sizeof(line)) {
            len = sizeof(line) - 1u;
        }
        memcpy(line, cursor, len);
        line[len] = '\0';
        if (len > 0 && line[len - 1u] == '\r') {
            line[--len] = '\0';
        }
        cursor = end != NULL ? end + 1 : NULL;

        word = line;
        while (*word == ' ' || *word == '\t') {
            word++;
        }
        if (strncmp(word, "#define ", 8u) != 0) {
            continue;
        }
        word += 8u;
        while (*word == ' ' || *word == '\t') {
            word++;
        }
        value = word;
        while (isalnum((unsigned char)*value) || *value == '_') {
            value++;
        }
        name_len = (size_t)(value - word);
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        if (name_len == 0 || value[0] != '0' || (value[1] != 'x' && value[1] != 'X')) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            char *stop = NULL;
            unsigned long actual;

            if (strlen(map[i].name) != name_len ||
                memcmp(map[i].name, word, name_len) != 0) {
                continue;
            }
            hits++;
            actual = strtoul(value + 2, &stop, 16);
            if (stop == value + 2 || *stop != '\0') {
                fprintf(stderr, "LUFA CHECK FAIL %s: %s has a malformed hex value\n",
                        label, map[i].name);
                *found = hits;
                return -1;
            }
            if (actual != map[i].value) {
                fprintf(stderr, "LUFA CHECK FAIL %s: %s = 0x%02lX, LUFA = 0x%02lX\n",
                        label, map[i].name, actual, map[i].value);
                *found = hits;
                return -1;
            }
            break;
        }
    }
    if (hits != count) {
        fprintf(stderr, "LUFA CHECK FAIL %s: found %lu/%lu expected defines\n",
                label, (unsigned long)hits, (unsigned long)count);
        *found = hits;
        return -1;
    }
    printf("  %-22s %2lu names match LUFA constants\n", label, (unsigned long)hits);
    *found = hits;
    return 0;
}

/* Extract the first static uint8_t descriptor array from the shipped test. */
static int first_descriptor(const char *text, uint8_t *out, size_t cap, size_t *length)
{
    const char *open = strstr(text, "static const uint8_t");
    const char *close;
    const char *scan;
    size_t count = 0;

    if (open == NULL || (open = strchr(open, '{')) == NULL) {
        return -1;
    }
    close = strchr(open, '}');
    if (close == NULL) {
        return -1;
    }
    scan = ++open;
    while (scan < close && count < cap) {
        const char *hex = strchr(scan, '0');

        if (hex == NULL || hex + 1 >= close) {
            break;
        }
        if (hex[1] != 'x' && hex[1] != 'X') {
            scan = hex + 1;
            continue;
        }
        {
            char *stop = NULL;
            unsigned long value = strtoul(hex + 2, &stop, 16);

            if (stop == hex + 2) {
                scan = hex + 1;
                continue;
            }
            out[count++] = (uint8_t)(value & 0xFFu);
            scan = stop;
        }
    }
    *length = count;
    return count > 0 ? 0 : -1;
}

/* Walk every descriptor array with LUFA's item masks and ensure each short
 * item has a recognised type/size and a payload that fits. */
static int check_descriptors(const char *text, size_t *descriptors, size_t *items)
{
    const char *cursor = text;
    const uint8_t collection8 = (uint8_t)(HID_RI_TYPE_MAIN | 0xA0u |
                                          HID_RI_DATA_BITS_8);
    const uint8_t input8 = (uint8_t)(HID_RI_TYPE_MAIN | 0x80u |
                                     HID_RI_DATA_BITS_8);
    size_t arrays = 0;
    size_t total_items = 0;
    int saw_collection = 0;
    int saw_input = 0;

    while ((cursor = strstr(cursor, "static const uint8_t")) != NULL) {
        const char *equals = strchr(cursor, '=');
        const char *open;
        const char *close;
        const char *scan;
        uint8_t bytes[512];
        size_t length = 0;
        size_t pos = 0;

        if (equals == NULL) {
            break;
        }
        open = strchr(equals, '{');
        close = open != NULL ? strchr(open, '}') : NULL;
        if (close == NULL) {
            break;
        }
        scan = open + 1;
        while (scan < close) {
            const char *hex = strchr(scan, '0');

            if (hex == NULL || hex + 1 >= close) {
                break;
            }
            if (hex[1] != 'x' && hex[1] != 'X') {
                scan = hex + 1;
                continue;
            }
            {
                char *stop = NULL;
                unsigned long value = strtoul(hex + 2, &stop, 16);

                if (stop == hex + 2) {
                    scan = hex + 1;
                    continue;
                }
                if (length < sizeof(bytes)) {
                    bytes[length++] = (uint8_t)(value & 0xFFu);
                }
                scan = stop;
            }
        }
        cursor = close + 1;
        if (length == 0) {
            continue;
        }
        arrays++;
        while (pos < length) {
            uint8_t prefix = bytes[pos++];
            unsigned type = (unsigned)(prefix & HID_RI_TYPE_MASK);
            unsigned size_code = (unsigned)(prefix & HID_RI_DATA_SIZE_MASK);
            size_t data_bytes = size_code == 3u ? 4u : size_code;

            if (!(type == HID_RI_TYPE_MAIN || type == HID_RI_TYPE_GLOBAL ||
                  type == HID_RI_TYPE_LOCAL)) {
                fprintf(stderr, "LUFA CHECK FAIL descriptor %lu: prefix 0x%02X "
                                "has an invalid item type\n",
                        (unsigned long)arrays, (unsigned)prefix);
                return -1;
            }
            if (!(size_code == HID_RI_DATA_BITS_0 || size_code == HID_RI_DATA_BITS_8 ||
                  size_code == HID_RI_DATA_BITS_16 ||
                  size_code == HID_RI_DATA_BITS_32)) {
                fprintf(stderr, "LUFA CHECK FAIL descriptor %lu: prefix 0x%02X "
                                "has an invalid data size\n",
                        (unsigned long)arrays, (unsigned)prefix);
                return -1;
            }
            if (pos + data_bytes > length) {
                fprintf(stderr, "LUFA CHECK FAIL descriptor %lu: item at offset "
                                "%lu is truncated\n",
                        (unsigned long)arrays, (unsigned long)(pos - 1u));
                return -1;
            }
            pos += data_bytes;
            total_items++;
            if (prefix == collection8) {
                saw_collection = 1;
            }
            if (prefix == input8) {
                saw_input = 1;
            }
        }
    }
    if (arrays < 5 || !saw_collection || !saw_input) {
        fprintf(stderr, "LUFA CHECK FAIL descriptors: %lu arrays, collection=%d, "
                        "input=%d\n", (unsigned long)arrays, saw_collection, saw_input);
        return -1;
    }
    printf("  %-22s %2lu descriptors, %lu valid short items\n", "descriptors",
           (unsigned long)arrays, (unsigned long)total_items);
    *descriptors = arrays;
    *items = total_items;
    return 0;
}

/* Feed the canonical mouse descriptor built from the LUFA-derived idea to
 * the SHIPPED decoder: real cross-validation, no second decoder. */
static int check_mouse_decoder(void)
{
    uint8_t descriptor[128];
    size_t length = lufa_ref_mouse_descriptor(descriptor, sizeof(descriptor));
    usb_mouse_layout_t layout;
    const uint8_t report[] = {
        0x02,             /* right button */
        0xF6, 0xFF,       /* X = -10 */
        0x0A, 0x00,       /* Y = +10 */
    };

    if (expect(length == 58, "canonical mouse descriptor length differs")) {
        return -1;
    }
    usb_mouse_parse_layout(descriptor, length, &layout);
    if (expect(layout.valid, "canonical mouse descriptor rejected by decoder") ||
        expect(!layout.has_report_id, "canonical mouse unexpectedly has a report ID") ||
        expect(layout.buttons.valid, "canonical mouse buttons not found") ||
        expect(layout.buttons.byte == 0 && layout.buttons.bits == 5,
               "canonical mouse button offset/width differs") ||
        expect(layout.x.valid && layout.x.byte == 1 && layout.x.bits == 16,
               "canonical mouse X offset/width differs") ||
        expect(layout.y.valid && layout.y.byte == 3 && layout.y.bits == 16,
               "canonical mouse Y offset/width differs") ||
        expect(!layout.wheel.valid, "canonical mouse must not have a wheel") ||
        expect(usb_hid_field_value(report, sizeof(report), &layout.buttons) == 2,
               "canonical mouse buttons decode differs") ||
        expect(usb_hid_field_value(report, sizeof(report), &layout.x) == -10,
               "canonical mouse signed X decode differs") ||
        expect(usb_hid_field_value(report, sizeof(report), &layout.y) == 10,
               "canonical mouse signed Y decode differs")) {
        return -1;
    }
    printf("  %-22s %2lu bytes decoded by shipped usb_hid_layout.c\n",
           "mouse -> decoder", (unsigned long)length);
    return 0;
}

/* Walk the canonical boot keyboard descriptor and prove the modifier byte +
 * reserved byte + 6-key array that components/usb/usb.c reads really exist. */
static int check_keyboard_report(void)
{
    uint8_t descriptor[128];
    size_t length = lufa_ref_keyboard_descriptor(descriptor, sizeof(descriptor));
    size_t pos = 0;
    uint32_t usage_page = 0;
    uint16_t usage_min = 0;
    uint16_t usage_max = 0;
    uint32_t size = 0;
    uint32_t count = 0;
    bool saw_modifiers = false;
    bool saw_reserved = false;
    bool saw_keys = false;

    if (expect(length == 64, "canonical keyboard descriptor length differs")) {
        return -1;
    }
    while (pos < length) {
        uint8_t prefix = descriptor[pos++];
        unsigned size_code = (unsigned)(prefix & HID_RI_DATA_SIZE_MASK);
        unsigned type = (unsigned)(prefix & HID_RI_TYPE_MASK);
        unsigned tag = (unsigned)((prefix & HID_RI_TAG_MASK) >> 4);
        size_t bytes = size_code == 3u ? 4u : size_code;
        uint32_t value = 0;

        if (pos + bytes > length) {
            return expect(false, "canonical keyboard descriptor is truncated");
        }
        for (size_t i = 0; i < bytes; i++) {
            value |= (uint32_t)descriptor[pos + i] << (8u * i);
        }
        pos += bytes;

        if (type == HID_RI_TYPE_GLOBAL) {
            if (tag == 0u) {
                usage_page = value;
            } else if (tag == 7u) {
                size = value;
            } else if (tag == 9u) {
                count = value;
            }
        } else if (type == HID_RI_TYPE_LOCAL) {
            if (tag == 1u) {
                usage_min = (uint16_t)value;
            } else if (tag == 2u) {
                usage_max = (uint16_t)value;
            }
        } else if (type == HID_RI_TYPE_MAIN && tag == 8u) { /* Input */
            bool constant = (value & HID_IOF_CONSTANT) != 0;

            if (usage_page == 0x07u && usage_min == 0xE0u && usage_max == 0xE7u &&
                size == 1u && count == 8u && !constant) {
                saw_modifiers = true;
            }
            if (constant && size == 8u && count == 1u) {
                saw_reserved = true;
            }
            if (usage_page == 0x07u && usage_min == 0x00u && usage_max == 0xFFu &&
                size == 8u && count == 6u && !constant) {
                saw_keys = true;
            }
            usage_min = 0;
            usage_max = 0;
        }
    }
    if (expect(saw_modifiers, "canonical keyboard has no 8-bit modifier field") ||
        expect(saw_reserved, "canonical keyboard has no reserved byte") ||
        expect(saw_keys, "canonical keyboard has no 6-key array") ||
        expect(sizeof(kMods) / sizeof(kMods[0]) == 8u,
               "usb.c modifier mask count differs from boot layout")) {
        return -1;
    }
    printf("  %-22s %2lu bytes: modifier(8)+reserved(1)+keys(6)\n",
           "keyboard report", (unsigned long)length);
    return 0;
}

/* The first shipped descriptor is a captured mouse. Its leading 16 bytes are
 * byte-for-byte the canonical prefix (through the button count); a fixture
 * edit here must go red. */
static int check_fixture(const char *text)
{
    uint8_t fixture[512];
    uint8_t reference[128];
    size_t length = 0;
    size_t ref_length = lufa_ref_mouse_descriptor(reference, sizeof(reference));
    usb_mouse_layout_t layout;

    if (expect(ref_length == 58, "canonical mouse descriptor length differs") ||
        first_descriptor(text, fixture, sizeof(fixture), &length) != 0 ||
        expect(length >= LUFA_REF_MOUSE_PREFIX_BYTES,
               "shipped mouse fixture is shorter than canonical prefix")) {
        return -1;
    }
    for (size_t i = 0; i < LUFA_REF_MOUSE_PREFIX_BYTES; i++) {
        if (fixture[i] != reference[i]) {
            fprintf(stderr, "LUFA CHECK FAIL fixture prefix byte %lu: 0x%02X, "
                            "reference 0x%02X\n", (unsigned long)i,
                    (unsigned)fixture[i], (unsigned)reference[i]);
            return -1;
        }
    }
    usb_mouse_parse_layout(fixture, length, &layout);
    if (expect(layout.valid, "shipped mouse fixture rejected by decoder") ||
        expect(layout.buttons.valid && layout.buttons.byte == 0 &&
               layout.buttons.bits == 5, "shipped buttons layout differs") ||
        expect(layout.x.valid && layout.x.byte == 1 && layout.x.bits == 16,
               "shipped X layout differs (16-bit axes expected)") ||
        expect(layout.y.valid && layout.y.byte == 3 && layout.y.bits == 16,
               "shipped Y layout differs (16-bit axes expected)") ||
        expect(layout.wheel.valid && layout.wheel.byte == 5 &&
               layout.wheel.bits == 8, "shipped wheel layout differs")) {
        return -1;
    }
    printf("  %-22s %2lu bytes match prefix + decoder map\n",
           "fixture vs reference", (unsigned long)length);
    return 0;
}

int main(int argc, char **argv)
{
    char *usb_text;
    char *test_text;
    size_t keys = 0;
    size_t mods = 0;
    size_t descriptors = 0;
    size_t items = 0;
    int rc = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <components/usb/usb.c> "
                        "<test/main/test_usb_hid_layout.c>\n", argv[0]);
        return 2;
    }
    usb_text = read_all(argv[1]);
    test_text = read_all(argv[2]);
    if (usb_text == NULL || test_text == NULL) {
        fprintf(stderr, "LUFA CHECK FAIL: cannot read input file(s)\n");
        free(usb_text);
        free(test_text);
        return 2;
    }

    printf("LUFA HID cross-reference (host-only, no firmware use):\n");
    if (check_mouse_decoder() == 0 &&
        check_keyboard_report() == 0 &&
        check_fixture(test_text) == 0 &&
        check_defines(usb_text, kKeys, sizeof(kKeys) / sizeof(kKeys[0]),
                      "key codes", &keys) == 0 &&
        check_defines(usb_text, kMods, sizeof(kMods) / sizeof(kMods[0]),
                      "modifiers", &mods) == 0 &&
        check_descriptors(test_text, &descriptors, &items) == 0) {
        printf("LUFA HID CHECK OK: canonical templates decoded by shipped code; "
               "%lu keys, %lu modifiers, %lu fixtures, %lu items\n",
               (unsigned long)keys, (unsigned long)mods,
               (unsigned long)descriptors, (unsigned long)items);
        rc = 0;
    } else {
        printf("LUFA HID CHECK FAIL\n");
    }
    free(usb_text);
    free(test_text);
    return rc;
}
