#ifndef P4MINISHELL_USB_H
#define P4MINISHELL_USB_H

/**
 * @file usb.h
 * @brief USB Host module for P4MiniShell (MSC storage + HID input).
 *
 * Owns ESP-IDF USB Host Library bring-up with two class drivers:
 *   - MSC (Mass Storage Class): mounts at /usb0 via VFS/FATFS
 *   - HID (Human Interface Device): keyboard and mouse with opt-in transcript echo
 *
 * USB MSC commands mirror the SD command family style with bounded,
 * transcript-friendly output. HID echo is intentionally opt-in for debug use.
 *
 * USB keyboard input can be routed to the shell CLI via a registered
 * input callback. When a USB keyboard is attached, the on-screen keyboard
 * is automatically hidden and keystrokes are injected into the input line.
 */

#include <stdbool.h>
#include <stdint.h>

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