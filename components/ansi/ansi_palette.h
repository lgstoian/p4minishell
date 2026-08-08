/**
 * @file ansi_palette.h
 * @brief Semantic colour palette for P4MiniShell shell output.
 *
 * This header is the single source of truth for "what colour is a warning",
 * "what colour is a field label", and so on. Every command in the firmware
 * composes its output from the `SH_*` macros below rather than choosing raw
 * `@`-specifiers, so the whole shell stays visually consistent and a future
 * palette change is a one-file edit.
 *
 * Architecture:
 *   The ansi module owns this header because it owns the `@`-specifier
 *   vocabulary these macros expand to. It introduces no new dependencies:
 *   the macros are plain string literals, so any layer that can already call
 *   `shell_transcript_appendf_ansi()` can use them.
 *
 * Usage:
 *   shell_transcript_appendf_ansi(SH_HEAD "System Information" SH_RST "\n");
 *   shell_transcript_appendf_ansi(SH_LBL "SSID:" SH_RST " " SH_VAL "%s" SH_RST "\n", ssid);
 *
 * The macros are string literals, so they concatenate at compile time and
 * cost nothing at runtime.
 *
 * IMPORTANT — specifier gotchas this header exists to hide:
 *   - `@k` is pure BLACK (0x0C0C0C), which is nearly invisible against the
 *     default 0x012456 background. Muted text must use `@K` (bright black /
 *     grey). Always use SH_MUTE rather than picking a specifier by hand.
 *   - `@E` is bright red, not `@Rr`; `@R` on its own is RESET.
 *   - `@L` is bright blue; `@b` is standard blue.
 *   - `@B` is BOLD, not blue.
 */

#ifndef P4MINISHELL_ANSI_PALETTE_H
#define P4MINISHELL_ANSI_PALETTE_H

/* ========================================================================
 * CORE TOKENS
 * ======================================================================== */

/** Reset every attribute. Close each coloured run with this. */
#define SH_RST      "@R"

/** Bold attribute, for emphasis inside an already-coloured run. */
#define SH_BOLD     "@B"

/* ========================================================================
 * STRUCTURE: headings, labels, and body text
 * ======================================================================== */

/** Section heading or title: sysinfo, about, version, wifi status, ... */
#define SH_HEAD     "@G"

/** Sub-heading inside a section, one level below SH_HEAD. */
#define SH_SUBHEAD  "@Y"

/** Field label or key: "SSID:", "IP:", "Status:", "Voltage:". */
#define SH_LBL      "@c"

/** Ordinary body text. Matches the shell's default green. */
#define SH_TEXT     "@G"

/** Muted, secondary, or timestamp text. Bright black (grey), never `@k`. */
#define SH_MUTE     "@K"

/* ========================================================================
 * OUTCOMES: success, failure, and caution
 * ======================================================================== */

/** Success, connected, OK, done. */
#define SH_OK       "@g"

/** Emphasised success, for a headline result rather than a field value. */
#define SH_OK_HI    "@G"

/** Error, failure, non-zero errorlevel. */
#define SH_ERR      "@r"

/** Emphasised error, for a headline failure or a destructive prompt. */
#define SH_ERR_HI   "@E"

/** Warning, non-fatal problem, deprecation. */
#define SH_WARN     "@y"

/** Emphasised warning, for a caution the user must not miss. */
#define SH_WARN_HI  "@Y"

/* ========================================================================
 * VALUES: the data a command is actually reporting
 * ======================================================================== */

/** Important value: SSID, IP, MAC, a filename the user asked about. */
#define SH_VAL      "@W"

/** Number, count, size, percentage, heap figure, battery level, RSSI. */
#define SH_NUM      "@M"

/** Filesystem path or filename in running text. */
#define SH_PATH     "@L"

/** Quoted string value. */
#define SH_STR      "@y"

/* ========================================================================
 * PROMPT AND COMMAND ECHO
 * ======================================================================== */

/** Prompt and input-line accent. */
#define SH_PROMPT   "@C"

/** A command name, as printed in help, history, or an echoed line. */
#define SH_CMD      "@C"

/** Usage syntax text, shown when a command is invoked incorrectly. */
#define SH_USAGE    "@y"

/** Description text beside a command name in help output. */
#define SH_DESC     "@w"

/* ========================================================================
 * DIRECTORY LISTINGS
 * ======================================================================== */

/** A directory entry in `dir`, `sd ls`, or `tree`. Bold for weight. */
#define SH_DIR      "@B@L"

/** An ordinary file entry. */
#define SH_FILE     "@w"

/** An executable or `.bat` entry, which the shell can run. */
#define SH_EXE      "@G"

/** A file size in a listing. */
#define SH_SIZE     "@M"

/** A timestamp in a listing. */
#define SH_TIME     "@K"

/* ========================================================================
 * SUBSYSTEM ACCENTS
 * ======================================================================== */

/** Wi-Fi connected state. */
#define SH_NET_UP   "@g"

/** Wi-Fi disconnected or errored state. */
#define SH_NET_DOWN "@K"

/** Bluetooth status and advertising accent. */
#define SH_BT       "@m"

/** USB attached or active accent. */
#define SH_USB_UP   "@g"

/** USB detached accent. */
#define SH_USB_DOWN "@K"

/** OTA progress and completion accent. */
#define SH_OTA      "@G"

/* ========================================================================
 * COMPOSED FRAGMENTS
 * ========================================================================
 * Ready-made pieces for the shapes that recur throughout the shell. Using
 * these keeps spacing and reset placement identical everywhere.
 */

/** A label followed by a space, already reset. Pair with a value macro. */
#define SH_KEY(text)        SH_LBL text SH_RST " "

/** A complete "label: value" pair using the important-value colour. */
#define SH_KV_FMT           SH_LBL "%s:" SH_RST " " SH_VAL "%s" SH_RST "\n"

/** A complete "label: number" pair. */
#define SH_KN_FMT           SH_LBL "%s:" SH_RST " " SH_NUM "%d" SH_RST "\n"

#endif /* P4MINISHELL_ANSI_PALETTE_H */
