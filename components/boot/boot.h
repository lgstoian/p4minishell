/**
 * @file boot.h
 * @brief DOS-style boot scripting: CONFIG.SYS parser and AUTOEXEC.BAT runner.
 *
 * On every boot the firmware looks for CONFIG.SYS and AUTOEXEC.BAT on the SD
 * card root. When either is missing and P4_CONFIG_BOOT_GENERATE_DEFAULTS is
 * set, default files are written once. CONFIG.SYS directives are parsed and
 * applied (environment, PATH, prompt, echo, display, audio, and the policy
 * directives that the owning modules expose). AUTOEXEC.BAT is then run through
 * the normal batch pipeline.
 *
 * Layering: this is a thin orchestrator. It never reaches into private state;
 * every effect goes through the documented public API of the owning module
 * (storage, batch, command, display, networking, usb). It depends on all of
 * them, so it sits alongside main in the dependency graph and is called once
 * from the post-init path.
 */

#ifndef P4MINISHELL_BOOT_H
#define P4MINISHELL_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run the boot scripting sequence: ensure CONFIG.SYS / AUTOEXEC.BAT exist
 * (generating defaults when configured), parse and apply CONFIG.SYS, then run
 * AUTOEXEC.BAT.
 *
 * Safe to call with no SD card present — it silently skips, identical to the
 * current boot behaviour. Must be called after storage, batch, command,
 * display, networking and usb are initialized, and after an SD session can be
 * opened, but before the interactive prompt is shown.
 */
void boot_run_startup(void);

/**
 * Ensure CONFIG.SYS and AUTOEXEC.BAT exist on the SD card root, generating the
 * documented defaults when either is missing. Idempotent: an existing file is
 * never overwritten.
 *
 * @return true when at least one default file was generated.
 */
bool boot_ensure_default_files(void);

/**
 * First-mount callback registered by main: runs when the SD card mounts for
 * the first time this boot (at startup or when a freshly-inserted card is
 * mounted by the first SD command). Generates the default boot files when
 * missing and prints a short "SD card ready" welcome.
 */
void boot_on_sd_first_mount(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_BOOT_H */
