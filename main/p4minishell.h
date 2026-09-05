/**
 * @file p4minishell.h
 * @brief Public API for P4MiniShell host bridge callbacks.
 *
 * Declares the host bridge callbacks that components/c6ota, components/usb,
 * and components/networking link against. ESP-IDF's component model requires
 * these symbols to live in the app (main) component, so the implementations
 * are split between main.c (c6ota and usb) and p4minishell.c (networking).
 *
 * Command implementations do NOT belong here. All built-in commands live in
 * components/command/command.c and are reached through command.h.
 */

#ifndef P4MINISHELL_H
#define P4MINISHELL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ========================================================================
 * HOST BRIDGE CALLBACKS
 * ======================================================================== */

/* c6ota module bridge — implemented in main.c */
void c6ota_host_transcript_append_text(const char *text);
void c6ota_host_schedule_transcript_append_text(const char *text);
void c6ota_host_record_error(esp_err_t error, const char *message);
void c6ota_host_record_warning(const char *message);
void c6ota_host_record_info(const char *message);
void c6ota_host_notify_header(const char *text, uint32_t timeout_ms);

/* usb module bridge — implemented in main.c */
void usb_host_transcript_append_text(const char *text);
void usb_host_schedule_transcript_append_text(const char *text);
void usb_host_record_error(esp_err_t error, const char *message);
void usb_host_record_warning(const char *message);
void usb_host_record_info(const char *message);
void usb_host_notify_header(const char *text, uint32_t timeout_ms);

/* networking module bridge — implemented in p4minishell.c */
void shell_networking_record_error(const char *tag, esp_err_t error, const char *message);
void shell_networking_schedule_text(const char *text);
void shell_networking_record_warning(const char *tag, const char *message);
void shell_networking_record_info(const char *tag, const char *message);

/* ========================================================================
 * NATIVE APP REGISTRATION
 * ======================================================================== */

/* Registered native apps (the applib ABI sample) — implemented in
 * main/native_apps.c, called once after command_init(). */
void native_apps_register(void);

#endif /* P4MINISHELL_H */
