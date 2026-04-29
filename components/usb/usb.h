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
 */

#include <stdbool.h>

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

#endif