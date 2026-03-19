#ifndef P4MINISHELL_USB_H
#define P4MINISHELL_USB_H

#include <stdbool.h>

// AI: USB module added with official ESP-IDF USB Host (MSC external storage + HID mouse/keyboard) using same API/SDK style as c6ota
// AI: External storage mirrors SD card implementation exactly (VFS + fatfs + MSDOS commands)
// AI: No behavior change to any existing feature
void usb_init(void);
void usb_handle_command(char *command);
void usb_status(void);
void usb_msc_mount(void);
void usb_msc_ls(const char *path);
void usb_hid_keyboard_enable(void);
void usb_hid_keyboard_disable(void);
void usb_hid_mouse_enable(void);
void usb_hid_mouse_disable(void);
bool usb_is_connected(void);
bool usb_is_mounted(void);

#endif