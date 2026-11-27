/*
 * LUFA Library - verbatim HID constant subset
 * Copyright (C) Dean Camera, 2021.
 *
 * dean [at] fourwalledcubicle [dot] com
 * www.lufa-lib.org
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaims all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 *
 * ------------------------------------------------------------------
 * P4MiniShell local note (MIT, (c) 2026 Stoian Alexandru):
 *
 * HOST-ONLY. The constants below are copied verbatim from LUFA at the
 * pinned commit 90d65ba059d91078a34b9c26c8772ee14b556a13 (LUFA
 * version string 210130), from:
 *   LUFA/Drivers/USB/Class/Common/HIDReportData.h
 *   LUFA/Drivers/USB/Class/Common/HIDClassCommon.h
 * Only named constants are reproduced. No LUFA driver code, parser,
 * AVR header, demo or bootloader is vendored. LUFA is an AVR8 USB
 * stack and is NOT compiled into firmware or the ESP-IDF build here.
 *
 * Every identifier below is a verbatim LUFA name and value. Our own
 * item-tag helpers live in lufa_hid_reference.[ch], not here.
 * ------------------------------------------------------------------
 */
#ifndef P4_LUFA_HID_CONSTANTS_H
#define P4_LUFA_HID_CONSTANTS_H

/* --- HIDReportData.h: short-item encoding masks and data sizes --- */
#define HID_RI_DATA_SIZE_MASK   0x03
#define HID_RI_TYPE_MASK        0x0C
#define HID_RI_TAG_MASK         0xF0

#define HID_RI_TYPE_MAIN        0x00
#define HID_RI_TYPE_GLOBAL      0x04
#define HID_RI_TYPE_LOCAL       0x08

#define HID_RI_DATA_BITS_0      0x00
#define HID_RI_DATA_BITS_8      0x01
#define HID_RI_DATA_BITS_16     0x02
#define HID_RI_DATA_BITS_32     0x03

/* --- HIDReportData.h: Input/Output item flag bits --- */
#define HID_IOF_CONSTANT        (1 << 0)
#define HID_IOF_DATA            (0 << 0)
#define HID_IOF_VARIABLE        (1 << 1)
#define HID_IOF_ARRAY           (0 << 1)
#define HID_IOF_RELATIVE        (1 << 2)
#define HID_IOF_ABSOLUTE        (0 << 2)

/* --- HIDClassCommon.h: keyboard report modifier masks --- */
#define HID_KEYBOARD_MODIFIER_LEFTCTRL   (1 << 0)
#define HID_KEYBOARD_MODIFIER_LEFTSHIFT  (1 << 1)
#define HID_KEYBOARD_MODIFIER_LEFTALT    (1 << 2)
#define HID_KEYBOARD_MODIFIER_LEFTGUI    (1 << 3)
#define HID_KEYBOARD_MODIFIER_RIGHTCTRL  (1 << 4)
#define HID_KEYBOARD_MODIFIER_RIGHTSHIFT (1 << 5)
#define HID_KEYBOARD_MODIFIER_RIGHTALT   (1 << 6)
#define HID_KEYBOARD_MODIFIER_RIGHTGUI   (1 << 7)

/* --- HIDClassCommon.h: keyboard/keypad usage scan codes --- */
#define HID_KEYBOARD_SC_A                                 0x04
#define HID_KEYBOARD_SC_Z                                 0x1D
#define HID_KEYBOARD_SC_1_AND_EXCLAMATION                 0x1E
#define HID_KEYBOARD_SC_9_AND_OPENING_PARENTHESIS         0x26
#define HID_KEYBOARD_SC_0_AND_CLOSING_PARENTHESIS         0x27
#define HID_KEYBOARD_SC_ENTER                             0x28
#define HID_KEYBOARD_SC_ESCAPE                            0x29
#define HID_KEYBOARD_SC_BACKSPACE                         0x2A
#define HID_KEYBOARD_SC_TAB                               0x2B
#define HID_KEYBOARD_SC_SPACE                             0x2C
#define HID_KEYBOARD_SC_MINUS_AND_UNDERSCORE              0x2D
#define HID_KEYBOARD_SC_EQUAL_AND_PLUS                    0x2E
#define HID_KEYBOARD_SC_OPENING_BRACKET_AND_OPENING_BRACE 0x2F
#define HID_KEYBOARD_SC_CLOSING_BRACKET_AND_CLOSING_BRACE 0x30
#define HID_KEYBOARD_SC_BACKSLASH_AND_PIPE                0x31
#define HID_KEYBOARD_SC_SEMICOLON_AND_COLON               0x33
#define HID_KEYBOARD_SC_APOSTROPHE_AND_QUOTE              0x34
#define HID_KEYBOARD_SC_GRAVE_ACCENT_AND_TILDE            0x35
#define HID_KEYBOARD_SC_COMMA_AND_LESS_THAN_SIGN          0x36
#define HID_KEYBOARD_SC_DOT_AND_GREATER_THAN_SIGN         0x37
#define HID_KEYBOARD_SC_SLASH_AND_QUESTION_MARK           0x38
#define HID_KEYBOARD_SC_CAPS_LOCK                         0x39
#define HID_KEYBOARD_SC_F1                                0x3A
#define HID_KEYBOARD_SC_F12                               0x45
#define HID_KEYBOARD_SC_PRINT_SCREEN                      0x46
#define HID_KEYBOARD_SC_SCROLL_LOCK                       0x47
#define HID_KEYBOARD_SC_PAUSE                             0x48
#define HID_KEYBOARD_SC_INSERT                            0x49
#define HID_KEYBOARD_SC_HOME                              0x4A
#define HID_KEYBOARD_SC_PAGE_UP                           0x4B
#define HID_KEYBOARD_SC_DELETE                            0x4C
#define HID_KEYBOARD_SC_END                               0x4D
#define HID_KEYBOARD_SC_PAGE_DOWN                         0x4E
#define HID_KEYBOARD_SC_RIGHT_ARROW                       0x4F
#define HID_KEYBOARD_SC_LEFT_ARROW                        0x50
#define HID_KEYBOARD_SC_DOWN_ARROW                        0x51
#define HID_KEYBOARD_SC_UP_ARROW                          0x52
#define HID_KEYBOARD_SC_NUM_LOCK                          0x53
#define HID_KEYBOARD_SC_KEYPAD_SLASH                      0x54
#define HID_KEYBOARD_SC_KEYPAD_ASTERISK                   0x55
#define HID_KEYBOARD_SC_KEYPAD_MINUS                      0x56
#define HID_KEYBOARD_SC_KEYPAD_PLUS                       0x57
#define HID_KEYBOARD_SC_KEYPAD_ENTER                      0x58
#define HID_KEYBOARD_SC_KEYPAD_1_AND_END                  0x59
#define HID_KEYBOARD_SC_KEYPAD_9_AND_PAGE_UP              0x61
#define HID_KEYBOARD_SC_KEYPAD_0_AND_INSERT               0x62
#define HID_KEYBOARD_SC_KEYPAD_DOT_AND_DELETE             0x63

#endif /* P4_LUFA_HID_CONSTANTS_H */
