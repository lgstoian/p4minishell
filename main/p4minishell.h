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
#include <stdbool.h>
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

/* ========================================================================
 * SD / FILE COMMAND BRIDGE
 * ========================================================================
 * These bridge functions allow command.c to dispatch SD and file commands
 * whose implementations live in main.c (due to tight coupling with shell
 * state like s_shell_cwd, s_shell_env_vars, s_sd_persistent_mounted, etc.).
 * Long-term: move these implementations into a dedicated fs component. */

void shell_bridge_sd_command(char *command);
void shell_bridge_cd_command(int argc, char **argv);
void shell_bridge_dir_command(int argc, char **argv);
void shell_bridge_copy_command(int argc, char **argv);
void shell_bridge_move_command(int argc, char **argv);
void shell_bridge_del_command(int argc, char **argv);
void shell_bridge_ren_command(int argc, char **argv, const char *verb);
void shell_bridge_mkdir_command(int argc, char **argv);
void shell_bridge_rmdir_command(int argc, char **argv);
void shell_bridge_type_command(int argc, char **argv);
void shell_bridge_write_command(int argc, char **argv, bool append_mode);
void shell_bridge_touch_command(int argc, char **argv);
void shell_bridge_set_command(int argc, char **argv);
void shell_bridge_path_command(int argc, char **argv);
void shell_bridge_echo_command(int argc, char **argv);
void shell_bridge_call_command(int argc, char **argv);
void shell_bridge_gpio_command(int argc, char **argv);
void shell_bridge_batch_file(const char *path, int argc, char **argv);

#endif /* P4MINISHELL_H */
