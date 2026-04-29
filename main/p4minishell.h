/**
 * @file p4minishell.h
 * @brief Public API for P4MiniShell host bridge callbacks.
 *
 * Declares the host bridge callback functions used by components/c6ota,
 * components/usb, and components/networking to communicate with the shell.
 * Implementation lives in p4minishell.c.
 */

#ifndef P4MINISHELL_H
#define P4MINISHELL_H

#include <stdint.h>
#include "esp_err.h"

/* ========================================================================
 * HOST BRIDGE CALLBACKS
 * ======================================================================== */

/* c6ota module bridge */
void c6ota_host_transcript_append_text(const char *text);
void c6ota_host_schedule_transcript_append_text(const char *text);
void c6ota_host_record_error(esp_err_t error, const char *message);
void c6ota_host_record_warning(const char *message);
void c6ota_host_record_info(const char *message);
void c6ota_host_notify_header(const char *text, uint32_t timeout_ms);

/* usb module bridge */
void usb_host_transcript_append_text(const char *text);
void usb_host_schedule_transcript_append_text(const char *text);
void usb_host_record_error(esp_err_t error, const char *message);
void usb_host_record_warning(const char *message);
void usb_host_record_info(const char *message);
void usb_host_notify_header(const char *text, uint32_t timeout_ms);

/* networking module bridge */
void shell_networking_record_error(const char *tag, esp_err_t error, const char *message);
void shell_networking_schedule_text(const char *text);
void shell_networking_record_warning(const char *tag, const char *message);
void shell_networking_record_info(const char *tag, const char *message);

#endif /* P4MINISHELL_H */
