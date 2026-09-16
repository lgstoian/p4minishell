/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#ifndef P4MINISHELL_USB_H
#define P4MINISHELL_USB_H

/**
 * @file usb.h
 * @brief USB Host module for P4MiniShell (MSC storage + HID input + CDC serial).
 *
 * Owns ESP-IDF USB Host Library bring-up with three class drivers:
 *   - MSC (Mass Storage Class): mounts at /usb0 via VFS/FATFS
 *   - HID (Human Interface Device): keyboard and mouse with opt-in transcript echo
 *   - CDC-ACM (serial): `usb userial` raw serial to external gear
 *     (implemented in userial.c; one open device at a time)
 *
 * USB MSC commands mirror the SD command family style with bounded,
 * transcript-friendly output. HID echo is intentionally opt-in for debug use.
 *
 * USB keyboard input can be routed to the shell CLI via a registered
 * input callback. When a USB keyboard is attached, the on-screen keyboard
 * is automatically hidden and keystrokes are injected into the input line.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** USB keyboard input event passed to the registered input callback. */
typedef enum {
    USB_KEY_EVENT_PRESS = 0,    /**< Key pressed (make) */
    USB_KEY_EVENT_RELEASE,      /**< Key released (break) */
} usb_key_event_t;

/** USB keyboard modifier flags (same as USB HID boot protocol). */
#define USB_KEY_MOD_LEFT_CTRL   0x01
#define USB_KEY_MOD_LEFT_SHIFT  0x02
#define USB_KEY_MOD_LEFT_ALT    0x04
#define USB_KEY_MOD_LEFT_GUI    0x08
#define USB_KEY_MOD_RIGHT_CTRL  0x10
#define USB_KEY_MOD_RIGHT_SHIFT 0x20
#define USB_KEY_MOD_RIGHT_ALT   0x40
#define USB_KEY_MOD_RIGHT_GUI   0x80

/**
 * USB keyboard input callback type.
 * Called from the USB module task context when a key event occurs.
 * @param key_code  USB HID key code (standard HID usage table).
 * @param modifiers Bitmask of USB_KEY_MOD_* flags.
 * @param event     PRESS or RELEASE.
 */
typedef void (*usb_keyboard_input_cb_t)(uint8_t key_code, uint8_t modifiers, usb_key_event_t event);

/** Initialize USB Host Library plus MSC and HID class drivers. Call once after networking_init(). */
void usb_init(void);

/** Entry point for shell-level `usb ...` command dispatch. */
void usb_handle_command(char *command);

/* ========================================================================
 * USB CDC-ACM SERIAL (userial.c): leaf driver + byte API
 * ========================================================================
 * The `usb userial` verbs live in components/command/userial_commands.c,
 * which uses only this byte API (keeping the usb component a leaf). One open
 * device at a time; RX lands in an internal ring drained by userial_read().
 */

/** Serial line settings (driver-type free so this header stays leaf-pure). */
typedef struct {
    uint32_t baud;       /**< Line speed (300..3000000). */
    uint8_t data_bits;   /**< 5..8. */
    uint8_t parity;      /**< 0 none, 1 odd, 2 even. */
    uint8_t stop_bits;   /**< 1 or 2. */
} userial_coding_t;

/** Snapshot of the serial state for `usb userial status`. */
typedef struct {
    bool open;                 /**< A device handle is held. */
    bool link_lost;            /**< The device disconnected. */
    uint16_t vid;              /**< Open device vendor id. */
    uint16_t pid;              /**< Open device product id. */
    userial_coding_t coding;   /**< Active line settings. */
    size_t waiting;            /**< Bytes buffered in the RX ring. */
    uint32_t dropped;          /**< Bytes lost to a full ring. */
} userial_status_t;

/**
 * Install the CDC-ACM host driver (idempotent stage of the USB bring-up;
 * called from usb_install_host_stack, never directly).
 */
esp_err_t userial_install_driver(void);

/**
 * Open a CDC-ACM device by vendor/product id. Blocks up to
 * P4_CONFIG_USERIAL_OPEN_TIMEOUT_MS for a matching device, then applies the
 * line coding. Fails when a device is already open.
 * @return 0 on success, 1 on no-match/IO failure or already-open.
 */
int userial_open(uint16_t vid, uint16_t pid, const userial_coding_t *coding);

/** Close the open device (no-op when none). Clears the RX ring. */
void userial_close(void);

/** True when a device is open and still connected. */
bool userial_is_open(void);

/** True when the open device reported a disconnect. */
bool userial_link_lost(void);

/** Fill @p out with the current state. @return true when filled. */
bool userial_get_status(userial_status_t *out);

/** Blocking write of @p len bytes with the op timeout. @return 0 ok, 1 fail. */
int userial_write(const uint8_t *data, size_t len);

/** Drain up to @p max bytes from the RX ring. @return bytes read. */
size_t userial_read(uint8_t *out, size_t max);

/**
 * Parse a `<vid:pid>` device id (hex, case-insensitive). Pure, unit-tested.
 * @return true with both outputs filled.
 */
bool userial_parse_id(const char *text, uint16_t *vid_out, uint16_t *pid_out);

/**
 * Validate serial line options into a @ref userial_coding_t. NULL selects
 * the default (115200 8N1). Pure, unit-tested.
 * @return true with @p out filled.
 */
bool userial_parse_coding(const char *baud_str, const char *data_str,
                          const char *parity_str, const char *stop_str,
                          userial_coding_t *out);

/** Print transcript-visible USB host, MSC, and HID state. */
void usb_status(void);

/** Mount a connected MSC device at /usb0 via msc_host_vfs_register(). */
void usb_msc_mount(void);

/** List files from a USB MSC path with bounded transcript output. */
void usb_msc_ls(const char *path);

/** Enable transcript echo for attached HID boot keyboard. */
void usb_hid_keyboard_enable(void);

/** Disable transcript echo for attached HID boot keyboard. */
void usb_hid_keyboard_disable(void);

/** Enable transcript echo for attached HID boot mouse. */
void usb_hid_mouse_enable(void);

/** Disable transcript echo for attached HID boot mouse. */
void usb_hid_mouse_disable(void);

/** Returns true when any USB device (MSC or HID) is attached. */
bool usb_is_connected(void);

/** Returns true when a USB MSC device is mounted at /usb0. */
bool usb_is_mounted(void);

/** Returns true when a USB HID keyboard is attached. */
bool usb_is_keyboard_attached(void);

/** Returns true when a USB HID mouse is attached. */
bool usb_is_mouse_attached(void);

/**
 * Register a callback to receive USB keyboard input events.
 * The callback is invoked from the USB module task context.
 * Pass NULL to unregister.
 * @param cb  Callback function, or NULL to unregister.
 */
void usb_register_keyboard_input_callback(usb_keyboard_input_cb_t cb);

/**
 * Convert a USB HID key code and modifiers to an ASCII character.
 * Returns the character in *out and true if the key produces a printable ASCII char.
 * Returns false for non-printable keys (arrows, function keys, etc.).
 * @param key_code   USB HID key code.
 * @param modifiers  Modifier bitmask.
 * @param out        Output character (only valid if returning true).
 * @return true if the key maps to a printable ASCII character.
 */
bool usb_key_to_ascii_full(uint8_t key_code, uint8_t modifiers, char *out);

/**
 * Get a human-readable name for a USB HID key code.
 * Returns NULL for unknown/unmapped keys.
 * @param key_code  USB HID key code.
 * @return Static string name, or NULL.
 */
const char *usb_key_name_full(uint8_t key_code);

#endif