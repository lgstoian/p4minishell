/**
 * @file security_commands.h
 * @brief Owner info, device passcode/lock, and private-record concealment.
 *
 * All persistence lives in CONFIG.SYS (the single scalar-preference store):
 *   OWNER_NAME / OWNER_COMPANY / OWNER_PHONE
 *   SECURITY_PASS_SALT / SECURITY_PASS_HASH   (PBKDF2-SHA256, hex)
 *   SECURITY_CONCEAL   (show|mask|hide)
 *   SECURITY_AUTOLOCK  (<secs|off>)
 *   SECURITY_BOOTLOCK  (on|off)
 *
 * Recovery when a passcode is forgotten: delete the SECURITY_* lines from
 * CONFIG.SYS on the removable SD card (or run `config factory`).
 *
 * This file is command-local (like config_cmd.c); other command files call the
 * state accessors to gate private-record output.
 */

#ifndef P4MINISHELL_SECURITY_COMMANDS_H
#define P4MINISHELL_SECURITY_COMMANDS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Concealment policy for secret DB records. */
enum {
    SECURITY_CONCEAL_SHOW = 0,  /**< Secret payloads readable when unlocked. */
    SECURITY_CONCEAL_MASK = 1,  /**< Metadata visible, payload always hidden. */
    SECURITY_CONCEAL_HIDE = 2,  /**< Secret records skipped entirely. */
};

/** Initialise security state to defaults. Does NOT touch the SD card: the
 *  saved CONFIG.SYS values load later in security_load_saved(), once the boot
 *  script has a mounted card (so init never mounts the SD ahead of the shell's
 *  first-mount hook). Idempotent. */
void security_init(void);

/** Load persisted security/owner state from CONFIG.SYS. Idempotent; safe to
 *  call once the SD card is mounted (boot_script_apply calls it before
 *  security_engage_boot_lock). Retries on a later call if the card was not
 *  readable yet. */
void security_load_saved(void);

/** True when the device is locked (boot lock or auto-lock). */
bool security_is_locked(void);

/** True when secret record payloads may be revealed (unlocked). */
bool security_can_reveal_private(void);

/** Active concealment policy (SECURITY_CONCEAL_*). */
int security_conceal_mode(void);

/**
 * Dispatcher gate: true when @p command may run while locked. The small
 * allowlist is security/unlock/help/cls/clear/version/about; everything else is
 * refused with a message.
 */
bool security_command_allowed(const char *command);

/** Engage the boot lock once, after the boot script has finished. */
void security_engage_boot_lock(void);

/** Auto-lock tick: locks when the configured idle timeout has elapsed. */
void security_autolock_tick(void);

/** `owner [name] [company] [phone]` command. */
void shell_command_owner(int argc, char **argv);

/** `security ...` command. */
void shell_command_security(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_SECURITY_COMMANDS_H */
