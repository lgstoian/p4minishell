/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file lufa_hid_reference.h
 * @brief Pure host-only reference for canonical HID boot descriptor shapes.
 *
 * Idea taken from LUFA's HID_DESCRIPTOR_MOUSE / HID_DESCRIPTOR_KEYBOARD
 * templates at pinned commit 90d65ba059d91078a34b9c26c8772ee14b556a13: a
 * mouse is button bits then X/Y axes; a keyboard boot report is 8 modifier
 * bits, a reserved byte and six key codes. The byte-building code here is
 * P4MiniShell's own (our names, our style); only the HID constants it uses
 * are copied verbatim from LUFA in lufa_hid_constants.h. This is a pure
 * helper: no USB host calls, no allocation, no LUFA driver/parser code.
 *
 * @module lufa_host
 * @layer leaf
 * @domain io
 * @provides canonical mouse/keyboard descriptor bytes for host HID checks
 * @status stable
 * @test tools/thirdparty/lufa/lufa_hid_fixture_check.c
 * @hard-rule Host-only: never compiled into firmware or the ESP-IDF build
 */
#ifndef P4_LUFA_HID_REFERENCE_H
#define P4_LUFA_HID_REFERENCE_H

#include <stddef.h>
#include <stdint.h>

/** Bytes 0..15 of the canonical mouse descriptor: through Usage Maximum
 *  (button count), so a fixture edit to that byte is also detected. */
#define LUFA_REF_MOUSE_PREFIX_BYTES 16u

/** Build the canonical 58-byte mouse descriptor (5 buttons, 16-bit axes,
 *  relative, no wheel) into @p out. Returns the number of bytes written, or
 *  fewer if @p cap is too small. */
size_t lufa_ref_mouse_descriptor(uint8_t *out, size_t cap);

/** Build the canonical 64-byte boot keyboard descriptor (6 key slots) into
 *  @p out. Returns the number of bytes written, or fewer if @p cap is small. */
size_t lufa_ref_keyboard_descriptor(uint8_t *out, size_t cap);

#endif /* P4_LUFA_HID_REFERENCE_H */
