/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file security_commands.c
 * @brief `owner` / `security` verbs: owner identity, device passcode/lock,
 * and the private-record concealment policy. See security_commands.h.
 *
 * The passcode is never stored in the clear: a random 16-byte salt and the
 * PBKDF2-HMAC-SHA256 hash (reusing crypt_derive_key, 32-byte key) are kept as
 * hex in CONFIG.SYS. Recovery is deleting the SECURITY_* lines on the SD card.
 */

#include "security_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "batch.h"
#include "command.h"
#include "config_cmd.h"
#include "storage.h"
#include "modal_surf.h"
#include "shell.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_err.h"

/* Internal buffer sizes (mirrors the crypt primitive's salt/key lengths). */
#ifndef P4_CONFIG_SECURITY_OWNER_BYTES
#define P4_CONFIG_SECURITY_OWNER_BYTES 64
#endif
#ifndef P4_CONFIG_SECURITY_PHONE_BYTES
#define P4_CONFIG_SECURITY_PHONE_BYTES 32
#endif
#ifndef P4_CONFIG_SECURITY_SALT_BYTES
#define P4_CONFIG_SECURITY_SALT_BYTES 16
#endif
#ifndef P4_CONFIG_SECURITY_HASH_BYTES
#define P4_CONFIG_SECURITY_HASH_BYTES 32
#endif

#define SEC_PASS_MAX 64

static char s_owner_name[P4_CONFIG_SECURITY_OWNER_BYTES];
static char s_owner_company[P4_CONFIG_SECURITY_OWNER_BYTES];
static char s_owner_phone[P4_CONFIG_SECURITY_PHONE_BYTES];
static bool s_has_pass;
static uint8_t s_salt[P4_CONFIG_SECURITY_SALT_BYTES];
static uint8_t s_hash[P4_CONFIG_SECURITY_HASH_BYTES];
static int s_conceal = SECURITY_CONCEAL_SHOW;
static int s_autolock_secs;                 /* 0 = off */
static bool s_bootlock;
static volatile bool s_locked;
static int64_t s_last_activity_us;

/* -------------------------------------------------------------------------
 * small helpers
 * ---------------------------------------------------------------------- */

static void sec_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = hex[(in[i] >> 4) & 0xF];
        out[2 * i + 1] = hex[in[i] & 0xF];
    }
    out[2 * n] = '\0';
}

static int sec_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool sec_hex_decode(const char *hex, uint8_t *out, size_t n)
{
    size_t i;

    if (hex == NULL || strlen(hex) != 2 * n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        int hi = sec_hex_nibble(hex[2 * i]);
        int lo = sec_hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/** Constant-time compare of two equal-length byte arrays. */
static bool sec_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static void sec_touch_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/* -------------------------------------------------------------------------
 * lifecycle / state
 * ---------------------------------------------------------------------- */

void security_init(void)
{
    /* Defaults only. The saved CONFIG.SYS values are loaded by
     * security_load_saved() once the boot script has a mounted card, so this
     * init path never mounts the SD ahead of the shell's first-mount hook
     * (which would skip the welcome/default-file/history/CJK boot work). */
    sec_touch_activity();
}

void security_load_saved(void)
{
    static bool s_settings_loaded;
    char buf[P4_CONFIG_SECURITY_HASH_BYTES * 2 + 1];
    char salt_hex[P4_CONFIG_SECURITY_SALT_BYTES * 2 + 1];
    char hash_hex[P4_CONFIG_SECURITY_HASH_BYTES * 2 + 1];
    bool have_salt;
    bool have_hash;

    if (s_settings_loaded) {
        return;
    }

    config_get_saved("OWNER_NAME", s_owner_name, sizeof(s_owner_name));
    config_get_saved("OWNER_COMPANY", s_owner_company, sizeof(s_owner_company));
    config_get_saved("OWNER_PHONE", s_owner_phone, sizeof(s_owner_phone));

    if (config_get_saved("SECURITY_CONCEAL", buf, sizeof(buf)) >= 0) {
        if (strcasecmp(buf, "hide") == 0) {
            s_conceal = SECURITY_CONCEAL_HIDE;
        } else if (strcasecmp(buf, "mask") == 0) {
            s_conceal = SECURITY_CONCEAL_MASK;
        } else {
            s_conceal = SECURITY_CONCEAL_SHOW;
        }
    }
    if (config_get_saved("SECURITY_AUTOLOCK", buf, sizeof(buf)) >= 0) {
        s_autolock_secs = (strcasecmp(buf, "off") == 0) ? 0 : atoi(buf);
    }
    if (config_get_saved("SECURITY_BOOTLOCK", buf, sizeof(buf)) >= 0) {
        s_bootlock = (strcasecmp(buf, "on") == 0);
    }

    have_salt = config_get_saved("SECURITY_PASS_SALT", salt_hex, sizeof(salt_hex)) >= 0 &&
                sec_hex_decode(salt_hex, s_salt, sizeof(s_salt));
    have_hash = config_get_saved("SECURITY_PASS_HASH", hash_hex, sizeof(hash_hex)) >= 0 &&
                sec_hex_decode(hash_hex, s_hash, sizeof(s_hash));
    s_has_pass = have_salt && have_hash;

    /* Only latch as loaded once the card was readable, so a mount that was not
     * ready retries on a later call (mirrors font_restore_saved). */
    s_settings_loaded = storage_sd_is_mounted();
    sec_touch_activity();
}

bool security_is_locked(void)
{
    return s_locked;
}

bool security_can_reveal_private(void)
{
    return !s_locked;
}

int security_conceal_mode(void)
{
    return s_conceal;
}

bool security_command_allowed(const char *command)
{
    static const char *const allow[] = {
        "security", "unlock", "help", "cls", "clear", "version", "ver", "about",
    };
    size_t i;

    security_autolock_tick();
    if (!s_locked) {
        sec_touch_activity();
        return true;
    }
    if (command == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(allow) / sizeof(allow[0]); i++) {
        if (shell_text_equals_ignore_case(command, allow[i])) {
            return true;
        }
    }
    shell_print_error("Device locked - run 'security unlock' first");
    return false;
}

void security_engage_boot_lock(void)
{
    s_locked = s_has_pass && s_bootlock;
    sec_touch_activity();
}

void security_autolock_tick(void)
{
    if (s_locked || !s_has_pass || s_autolock_secs <= 0) {
        return;
    }
    if (esp_timer_get_time() - s_last_activity_us >= (int64_t)s_autolock_secs * 1000000LL) {
        s_locked = true;
    }
}

/* -------------------------------------------------------------------------
 * commands
 * ---------------------------------------------------------------------- */

static void owner_show(void)
{
    shell_print_field("owner.name", "%s", s_owner_name[0] ? s_owner_name : "(not set)");
    shell_print_field("owner.company", "%s", s_owner_company[0] ? s_owner_company : "(not set)");
    shell_print_field("owner.phone", "%s", s_owner_phone[0] ? s_owner_phone : "(not set)");
}

void shell_command_owner(int argc, char **argv)
{
    if (argc == 1) {
        owner_show();
        batch_set_errorlevel(0);
        return;
    }
    if (strcasecmp(argv[1], "name") == 0 && argc >= 3) {
        snprintf(s_owner_name, sizeof(s_owner_name), "%s", argv[2]);
        config_persist_set("OWNER_NAME", s_owner_name);
    } else if (strcasecmp(argv[1], "company") == 0 && argc >= 3) {
        snprintf(s_owner_company, sizeof(s_owner_company), "%s", argv[2]);
        config_persist_set("OWNER_COMPANY", s_owner_company);
    } else if (strcasecmp(argv[1], "phone") == 0 && argc >= 3) {
        snprintf(s_owner_phone, sizeof(s_owner_phone), "%s", argv[2]);
        config_persist_set("OWNER_PHONE", s_owner_phone);
    } else if (argc >= 2 && (strcasecmp(argv[1], "show") == 0 || strcasecmp(argv[1], "status") == 0)) {
        owner_show();
    } else {
        shell_print_usage("Usage: owner [show] | owner name|company|phone <value>");
        batch_set_errorlevel(2);
        return;
    }
    shell_print_ok("owner: saved");
    batch_set_errorlevel(0);
}

static bool sec_verify(const char *pass)
{
    uint8_t candidate[P4_CONFIG_SECURITY_HASH_BYTES];

    if (!s_has_pass) {
        return false;
    }
    if (crypt_derive_key(pass, s_salt, candidate) != 0) {
        return false;
    }
    return sec_ct_equal(candidate, s_hash, sizeof(s_hash));
}

static void sec_show_status(void)
{
    char salt_hex[P4_CONFIG_SECURITY_SALT_BYTES * 2 + 1];
    char hash_hex[P4_CONFIG_SECURITY_HASH_BYTES * 2 + 1];

    sec_hex_encode(s_salt, sizeof(s_salt), salt_hex);
    sec_hex_encode(s_hash, sizeof(s_hash), hash_hex);
    shell_print_field("security.locked", "%s", s_locked ? "YES" : "NO");
    shell_print_field("security.passcode", "%s", s_has_pass ? "set" : "not set");
    shell_print_field("security.conceal", "%s",
                      s_conceal == SECURITY_CONCEAL_HIDE ? "hide" :
                      (s_conceal == SECURITY_CONCEAL_MASK ? "mask" : "show"));
    if (s_autolock_secs > 0) {
        shell_print_field_num("security.autolock", s_autolock_secs);
    } else {
        shell_print_field("security.autolock", "off");
    }
    shell_print_field("security.bootlock", "%s", s_bootlock ? "on" : "off");
    (void)salt_hex;
    (void)hash_hex;
}

static void sec_setpass(void)
{
    char p1[SEC_PASS_MAX];
    char p2[SEC_PASS_MAX];

    /* Password entry uses the shared password modal (the same surface `ask /p`
     * builds) so touch, USB, and serial all work. */
    if (modal_ask_run("New passcode", NULL, true, 0, p1, sizeof(p1)) != 0 || p1[0] == '\0') {
        shell_print_error("security: no passcode entered");
        batch_set_errorlevel(1);
        return;
    }
    if (modal_ask_run("Confirm passcode", NULL, true, 0, p2, sizeof(p2)) != 0) {
        shell_print_error("security: confirmation cancelled");
        batch_set_errorlevel(1);
        return;
    }
    if (strcmp(p1, p2) != 0) {
        shell_print_error("security: passcodes did not match");
        batch_set_errorlevel(1);
        return;
    }

    esp_fill_random(s_salt, sizeof(s_salt));
    if (crypt_derive_key(p1, s_salt, s_hash) != 0) {
        shell_print_error("security: could not derive key");
        batch_set_errorlevel(1);
        return;
    }
    s_has_pass = true;
    {
        char salt_hex[P4_CONFIG_SECURITY_SALT_BYTES * 2 + 1];
        char hash_hex[P4_CONFIG_SECURITY_HASH_BYTES * 2 + 1];
        sec_hex_encode(s_salt, sizeof(s_salt), salt_hex);
        sec_hex_encode(s_hash, sizeof(s_hash), hash_hex);
        config_persist_set("SECURITY_PASS_SALT", salt_hex);
        config_persist_set("SECURITY_PASS_HASH", hash_hex);
    }
    shell_print_ok("security: passcode set");
    batch_set_errorlevel(0);
}

static void sec_clearpass(void)
{
    char p[SEC_PASS_MAX];

    if (s_has_pass) {
        if (modal_ask_run("Current passcode", NULL, true, 0, p, sizeof(p)) != 0) {
            shell_print_error("security: cancelled");
            batch_set_errorlevel(1);
            return;
        }
        if (!sec_verify(p)) {
            shell_print_error("security: wrong passcode");
            batch_set_errorlevel(1);
            return;
        }
    }
    s_has_pass = false;
    memset(s_salt, 0, sizeof(s_salt));
    memset(s_hash, 0, sizeof(s_hash));
    s_locked = false;
    config_persist_set("SECURITY_PASS_SALT", "");
    config_persist_set("SECURITY_PASS_HASH", "");
    shell_print_ok("security: passcode cleared");
    batch_set_errorlevel(0);
}

static void sec_unlock(void)
{
    char p[SEC_PASS_MAX];

    if (!s_has_pass) {
        s_locked = false;
        shell_print_ok("security: no passcode set (unlocked)");
        batch_set_errorlevel(0);
        return;
    }
    if (modal_ask_run("Passcode", NULL, true, 0, p, sizeof(p)) != 0) {
        shell_print_error("security: cancelled");
        batch_set_errorlevel(1);
        return;
    }
    if (!sec_verify(p)) {
        shell_print_error("security: wrong passcode");
        batch_set_errorlevel(1);
        return;
    }
    s_locked = false;
    sec_touch_activity();
    shell_print_ok("security: unlocked");
    batch_set_errorlevel(0);
}

void shell_command_security(int argc, char **argv)
{
    const char *sub;

    if (argc < 2 || strcasecmp(argv[1], "status") == 0) {
        sec_show_status();
        batch_set_errorlevel(0);
        return;
    }
    sub = argv[1];

    if (strcasecmp(sub, "conceal") == 0) {
        if (argc < 3) {
            shell_print_usage("Usage: security conceal show|mask|hide");
            batch_set_errorlevel(2);
            return;
        }
        if (strcasecmp(argv[2], "show") == 0) {
            s_conceal = SECURITY_CONCEAL_SHOW;
        } else if (strcasecmp(argv[2], "mask") == 0) {
            s_conceal = SECURITY_CONCEAL_MASK;
        } else if (strcasecmp(argv[2], "hide") == 0) {
            s_conceal = SECURITY_CONCEAL_HIDE;
        } else {
            shell_print_usage("Usage: security conceal show|mask|hide");
            batch_set_errorlevel(2);
            return;
        }
        config_persist_set("SECURITY_CONCEAL",
                           s_conceal == SECURITY_CONCEAL_HIDE ? "hide" :
                           (s_conceal == SECURITY_CONCEAL_MASK ? "mask" : "show"));
        shell_print_ok("security: conceal=%s", argv[2]);
        batch_set_errorlevel(0);
        return;
    }
    if (strcasecmp(sub, "setpass") == 0) {
        sec_setpass();
        return;
    }
    if (strcasecmp(sub, "clearpass") == 0) {
        sec_clearpass();
        return;
    }
    if (strcasecmp(sub, "lock") == 0) {
        if (!s_has_pass) {
            shell_print_error("security: set a passcode first");
            batch_set_errorlevel(1);
            return;
        }
        s_locked = true;
        shell_print_ok("security: locked");
        batch_set_errorlevel(0);
        return;
    }
    if (strcasecmp(sub, "unlock") == 0) {
        sec_unlock();
        return;
    }
    if (strcasecmp(sub, "autolock") == 0) {
        if (argc < 3) {
            shell_print_usage("Usage: security autolock <secs|off>");
            batch_set_errorlevel(2);
            return;
        }
        if (strcasecmp(argv[2], "off") == 0) {
            s_autolock_secs = 0;
            config_persist_set("SECURITY_AUTOLOCK", "off");
        } else {
            s_autolock_secs = atoi(argv[2]);
            if (s_autolock_secs < 0) {
                s_autolock_secs = 0;
            }
            config_persist_set("SECURITY_AUTOLOCK", argv[2]);
        }
        sec_touch_activity();
        shell_print_ok("security: autolock=%s", s_autolock_secs > 0 ? argv[2] : "off");
        batch_set_errorlevel(0);
        return;
    }
    if (strcasecmp(sub, "bootlock") == 0) {
        if (argc < 3) {
            shell_print_usage("Usage: security bootlock on|off");
            batch_set_errorlevel(2);
            return;
        }
        s_bootlock = (strcasecmp(argv[2], "on") == 0);
        config_persist_set("SECURITY_BOOTLOCK", s_bootlock ? "on" : "off");
        shell_print_ok("security: bootlock=%s", s_bootlock ? "on" : "off");
        batch_set_errorlevel(0);
        return;
    }

    shell_print_usage("Usage: security [status] | conceal show|mask|hide | setpass | clearpass | lock | unlock | autolock <secs|off> | bootlock on|off");
    batch_set_errorlevel(2);
}
