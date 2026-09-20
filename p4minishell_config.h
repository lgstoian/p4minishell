/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file p4minishell_config.h
 * @brief Centralized configuration for P4MiniShell.
 *
 * ALL tunable values live here, organized by subsystem. This is the single
 * C-level source of truth. The companion YAML file (p4minishell_config.yaml)
 * documents every value with descriptions, units, and valid ranges.
 *
 * Categories:
 *   - Shell identity, UI, and limits
 *   - Transcript and command buffer sizing
 *   - Wi-Fi and Bluetooth parameters
 *   - SD card and filesystem limits
 *   - Batch engine and environment variables
 *   - GPIO and hardware control
 *   - Header bar visual styling
 *   - USB host parameters
 *   - C6 OTA parameters
 *   - Task stack sizes
 *
 * Usage: #include "p4minishell_config.h" in every source file that needs
 * configurable values. The header has no dependencies beyond <stdint.h>
 * and <stdbool.h>.
 */

#ifndef P4MINISHELL_CONFIG_H
#define P4MINISHELL_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ESP-IDF type references are resolved by source files that include
 * the relevant headers (driver/gpio.h, esp_adc/adc_oneshot.h).
 * This header only stores the raw integer values. */

/* ========================================================================
 * SHELL IDENTITY
 * ======================================================================== */

/** Log tag used by ESP_LOGx macros in the shell layer. */
#define P4_CONFIG_SHELL_TAG                  "p4minishell"

/* Board identity (slug, requested name, detected name) is a per-board fact and
 * lives in boards/<name>/board_config.h as BOARD_CFG_ID / BOARD_CFG_NAME /
 * BOARD_CFG_DETECTED_NAME. It is NOT duplicated here so the two board profiles
 * cannot drift. See PORTING.md. */

/**
 * P4MiniShell semantic version.
 * Update these when the project version changes in changelog.md.
 * The boot message and all version commands read from these macros.
 */
#define P4_CONFIG_VERSION_MAJOR             1
#define P4_CONFIG_VERSION_MINOR             2
#define P4_CONFIG_VERSION_PATCH             1

/** Full version string assembled from the components above. */
#define P4_CONFIG_VERSION_STRING             "v" STR(P4_CONFIG_VERSION_MAJOR) "." STR(P4_CONFIG_VERSION_MINOR) "." STR(P4_CONFIG_VERSION_PATCH)

/** Helper for stringification. */
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/** Product display name. */
#define P4_CONFIG_PRODUCT_NAME               "P4MiniShell"

/** Copyright/notice surfaced by `about` and the license notice (MIT). */
#define P4_CONFIG_COPYRIGHT_NOTICE           "Copyright (c) 2026 Stoian Alexandru. MIT License."

/** Boot banner displayed in the transcript on startup. The board name comes
 *  from the active board profile's BOARD_CFG_NAME. */
#define P4_CONFIG_BOOT_MESSAGE               P4_CONFIG_PRODUCT_NAME " " P4_CONFIG_VERSION_STRING " ready | " BOARD_CFG_NAME " | type help"

/* ========================================================================
 * BOOT SPLASH (generated icon overlay)
 * ======================================================================== */

/** Show the generated boot-splash icon over the shell on the first UI build. */
#define P4_CONFIG_SPLASH_ENABLE              1

/** How long the boot splash stays up before auto-dismissing (ms). A tap on it
 *  dismisses it earlier. */
#define P4_CONFIG_SPLASH_MS                  1500

/** Shell prompt string shown on the input line and serial console.
 *  The LVGL input line uses a plain-text version; the UART console
 *  renders with ANSI color codes for PowerShell-style coloring.
 *  Format: "PS " (bright white) + path (bright yellow) + "> " (bright white) */
#define P4_CONFIG_SHELL_PROMPT               "PS " P4_CONFIG_PS_PATH_SEPARATOR "> "

/* ========================================================================
 * TRANSCRIPT AND COMMAND BUFFER SIZING
 * ======================================================================== */

/** Maximum bytes stored in the on-screen transcript buffer.
 *  Sized so the scrollable transcript holds a long history (~150 lines of
 *  coloured output) instead of only the last screenful; the LVGL span group
 *  shows ~15 lines and scrolls through the rest. The buffer is allocated from
 *  PSRAM (with an internal-RAM fallback) so it does not compete with the
 *  DMA-capable heap used by the WiFi/SDIO transport mempool and the
 *  USB-Serial/JTAG ring buffers. Raised to 24576 in v0.35.2: PSRAM is
 *  plentiful, and the render staging copies moved to PSRAM as well, so long
 *  scrollback no longer costs internal RAM. */
#define P4_CONFIG_TRANSCRIPT_BYTES           65536

/**
 * Maximum bytes staged for the on-screen transcript span group. The staged
 * copy is the raw ANSI transcript, so it only needs to hold the same size as
 * the transcript itself (no recolor inflation: the span-group renders SGR
 * escapes directly instead of recolor markup).
 */
#define P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES   P4_CONFIG_TRANSCRIPT_BYTES

/**
 * Internal (DMA-capable) heap free threshold in bytes that triggers an
 * automatic transcript scrollback trim.
 *
 * The on-screen transcript renders the coloured scrollback as LVGL spans,
 * and every span carries per-span struct/style/text overhead in the internal
 * heap. Over a long session the accumulated spans can exhaust the internal
 * heap to the point where a tiny stdio allocation (a newlib FILE lock mutex
 * created by printf) fails and aborts the board. When free internal RAM
 * drops below this threshold the shell drops the oldest half of the
 * transcript and frees the corresponding spans, keeping the internal heap
 * above the failure floor. Set to 12288 in v0.35.2 (was 20000): the render
 * staging copies moved to PSRAM, so normal free sits well above this floor
 * and trims fire only under genuine pressure. The trim itself keeps the
 * newest three quarters and is rate-limited to one per 2 s, so scrollback
 * survives long sessions.
 */
#define P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES   4096

/**
 * Transcript scroll step in pixels applied by one on-screen scroll button
 * press (input-row Up/Dn buttons) and by one USB mouse wheel notch.
 */
#define P4_CONFIG_TRANSCRIPT_SCROLL_STEP      60

/**
 * Transcript auto-follow threshold in pixels. When the scroll position is
 * within this distance of the bottom, new output keeps the view pinned to the
 * newest lines (a terminal-style follow). Scrolling up past this threshold
 * stops the follow until the user returns to the bottom or submits a command.
 */
#define P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX 32

/**
 * Maximum on-screen staleness of the transcript label in milliseconds while
 * a command streams output. Repainting the LVGL span group costs O(buffer),
 * so per-line repaints make big listings crawl; appends mark the label dirty
 * and repaint at most this often (plus once per command, before key waits,
 * and before screenshots). Serial output is unaffected and stays live.
 */
#define P4_CONFIG_TRANSCRIPT_FLUSH_MS          200

/** Maximum bytes in the async (background task) transcript staging buffer. */
#define P4_CONFIG_ASYNC_TRANSCRIPT_BYTES     512

/** Maximum bytes for a single command line (input + null terminator). */
#define P4_CONFIG_COMMAND_BYTES              4096

/** Number of commands retained in the recall history. */
#define P4_CONFIG_COMMAND_HISTORY_DEPTH      32

/**
 * Total bytes the heap-backed recall history may hold. A cap keeps very long
 * (up to P4_CONFIG_COMMAND_BYTES) commands from exhausting RAM.
 */
#define P4_CONFIG_HISTORY_TOTAL_BYTES        65536

/** Default SD profile for `history /save` / `history /load`. */
#define P4_CONFIG_HISTORY_PROFILE            "HISTORY.TXT"

/** Auto-load the history profile at boot and auto-save it as it changes. */
#define P4_CONFIG_HISTORY_AUTOSAVE           1

/** Maximum matches reported by Tab completion before truncation. */
#define P4_CONFIG_COMPLETION_MAX_MATCHES     32

/** Show an inline ghost-text completion after the caret as you type. */
#define P4_CONFIG_COMPLETION_GHOST           1

/** Maximum bytes of a file the `edit` editor will load into RAM. The
 *  document, its undo snapshots, and the view caches are PSRAM-backed, so this
 *  is bounded by the PSRAM budget rather than the internal DMA heap. */
#define P4_CONFIG_EDITOR_MAX_BYTES          (1024 * 1024)

/** Maximum lines the `edit` editor will load into RAM. Only a bounded window
 *  of rows is materialized as LVGL spans (P4_CONFIG_EDITOR_RENDER_ROWS). */
#define P4_CONFIG_EDITOR_MAX_LINES           65536

/** Undo/redo depth kept by the `edit` editor (edit operations). The ring is
 *  also byte-budgeted by P4_CONFIG_EDITOR_UNDO_MAX_BYTES. */
#define P4_CONFIG_EDITOR_UNDO_DEPTH          64

/** Total bytes of full-document undo/redo snapshots the editor keeps. Older
 *  steps are evicted once the ring exceeds this, so undo memory is bounded
 *  even for a multi-megabyte document (at least the newest step is kept). */
#define P4_CONFIG_EDITOR_UNDO_MAX_BYTES      (4 * 1024 * 1024)

/** Largest document (approximate bytes) for which the editor takes a
 *  full-document snapshot per edit. Above this, undo is disabled for the
 *  session so editing a large file stays responsive (each edit would otherwise
 *  copy the whole document). Smaller documents keep full undo. */
#define P4_CONFIG_EDITOR_UNDO_MAX_SNAPSHOT_BYTES (256 * 1024)

/** Maximum document rows materialized as LVGL spans at once. Larger documents
 *  render a window around the cursor/scroll position (the overlays and touch
 *  mapping use document coordinates, so they stay aligned). Kept near a
 *  screenful plus a scroll margin: the span group is rebuilt on every edit, so
 *  a large window would make typing and scrolling quadratic in the row count
 *  and trip the task watchdog. */
#define P4_CONFIG_EDITOR_RENDER_ROWS         256

/** Tab stop width (columns) used by the `edit` editor. */
#define P4_CONFIG_EDITOR_TAB_WIDTH           4

/** Line height reserved per editor row, in pixels. */
#define P4_CONFIG_EDITOR_LINE_HEIGHT         20

/** True when the `edit` editor applies batch syntax highlighting. */
#define P4_CONFIG_EDITOR_SYNTAX_BATCH        1

/** Maximum length of the `edit` editor's search / replace string (bytes). */
#define P4_CONFIG_EDITOR_FIND_BYTES          128

/**
 * Capacity of the `edit` editor's prompt buffer (bytes). Large enough to
 * hold a Save-As path, a Go-to-Line number, or a Find / Replace string.
 */
#define P4_CONFIG_EDITOR_PROMPT_BYTES        P4_CONFIG_SD_PATH_BYTES

/** Shared cursor blink period (shell input block cursor anim + editor
 * cursor timer), in milliseconds. 0 = steady cursor (no blink). */
#define P4_CONFIG_CURSOR_BLINK_MS            500

/** RGB colour of the `edit` editor's selection background overlay. */
#define P4_CONFIG_EDITOR_SELECTION_COLOR     0x335577

/** Default case sensitivity for the `edit` editor's Find (0 = case-folded). */
#define P4_CONFIG_EDITOR_FIND_CASE_SENSITIVE 0

/** True when the `edit` editor renders a line-number gutter. */
#define P4_CONFIG_EDITOR_LINE_NUMBERS         1

/**
 * Width of the `edit` editor's line-number gutter in characters (digits).
 * Line numbers are right-aligned in this width followed by one space, so the
 * text column starts just past the gutter.
 */
#define P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS 5

/** True when the `edit` editor highlights the cursor's current line. */
#define P4_CONFIG_EDITOR_CURRENT_LINE         1

/** RGB colour of the `edit` editor's current-line highlight background. */
#define P4_CONFIG_EDITOR_CURRENT_LINE_COLOR   0x16222A

/** Show a live word count in the `edit` status bar (writerdeck). */
#define P4_CONFIG_EDITOR_WORD_COUNT           1

/** Enable focus / typewriter mode (`edit ... /focus`). */
#define P4_CONFIG_EDITOR_FOCUS                1

/* ---- Spellcheck (writerdeck) ----
 * An offline wordlist is loaded from sd:/DICTS/<name>.words (one lower-case
 * word per line) and misspellings are underlined while editing. The wordlist
 * is read through an internal DMA bounce buffer (PSRAM is not DMA-capable). */
#define P4_CONFIG_SPELL_ENABLE                1
#define P4_CONFIG_SPELL_DEFAULT               0      /**< Underlines on at session start */
#define P4_CONFIG_SPELL_DICT_DIR_NAME         "DICTS"
#define P4_CONFIG_SPELL_DICT_NAME             "en"   /**< Default <name>.words stem */
#define P4_CONFIG_SPELL_WORD_MAX              64     /**< Retained for compatibility (token lookups compare any length directly, single-character included) */
#define P4_CONFIG_SPELL_MAX_WORDS             65536  /**< Wordlist capacity */
#define P4_CONFIG_SPELL_MAX_BYTES             (1024 * 1024) /**< Wordlist file cap */

/* ---- Document templates (writerdeck) ----
 * New-file seeds live in sd:/TEMPLATES/<name>.MD (`edit <file> /template <name>`). */
#define P4_CONFIG_TEMPLATES_DIR_NAME          "TEMPLATES"
#define P4_CONFIG_TEMPLATE_NAME_BYTES         32
#define P4_CONFIG_TEMPLATE_MAX_BYTES          8192  /**< Seeded template cap */

/** Render-target byte cap for `markdown export` (text/HTML document). */
#define P4_CONFIG_MD_EXPORT_MAX_BYTES         (256 * 1024)

/** Print-to-file layout (writerdeck): fixed page geometry for the `print`
 *  export format, wrapped and paginated for sharing/printing. */
#define P4_CONFIG_PRINT_COLUMNS               80
#define P4_CONFIG_PRINT_ROWS                  60

/** Default font for the reading role (viewer + editor markdown preview).
 *  Proportional is allowed here (unlike the terminal role); install a serif
 *  TTF on SD and `font set reading <name> /save` for a book-like face. */
#define P4_CONFIG_FONT_READING_DEFAULT        "montserrat_14"

/** Serif reading face vendored in assets/fonts/ and pushed to sd:/FONTS/;
 *  auto-selected for the reading role at first mount when no choice is saved. */
#define P4_CONFIG_FONT_READING_SERIF          "DejaVuSerif"

/** Extra line spacing (px) added to the reading role's line height in the
 *  viewer and the editor Markdown preview (typography polish). */
#define P4_CONFIG_READING_LINE_SPACING        4

/** Number of entries in the debug/error log ring buffer. */
#define P4_CONFIG_DEBUG_LOG_DEPTH            5

/** Maximum bytes per debug log entry. */
#define P4_CONFIG_DEBUG_ENTRY_BYTES          192

/** Default SD profile for `debug save` (mirrors P4_CONFIG_HISTORY_PROFILE). */
#define P4_CONFIG_DEBUG_LOG_PROFILE          "DEBUG.LOG"

/* ========================================================================
 * UI LAYOUT
 * ======================================================================== */

/** Height of the on-screen LVGL keyboard in pixels. */
#define P4_CONFIG_KEYBOARD_HEIGHT            240

/**
 * Adaptive header status-refresh cadence (milliseconds). The header is polled
 * at the FIRST matching interval below; see header_refresh.c:
 *   display off by idle -> WAKE, OTA/bg job -> BUSY,
 *   Wi-Fi starting -> CONNECTING, early boot -> STARTUP, else IDLE
 * (IDLE is also clamped to the next minute boundary for an exact clock and to
 * the pending idle-display-off deadline).
 */
#define P4_CONFIG_HEADER_REFRESH_IDLE_MS        2000
#define P4_CONFIG_HEADER_REFRESH_WAKE_MS        150
#define P4_CONFIG_HEADER_REFRESH_BUSY_MS        1000
#define P4_CONFIG_HEADER_REFRESH_CONNECTING_MS  750
#define P4_CONFIG_HEADER_REFRESH_STARTUP_MS     500
#define P4_CONFIG_HEADER_REFRESH_MIN_MS         100

/**
 * Expensive telemetry (heap, CPU runtime-stats snapshot, battery ADC) is
 * sampled no faster than this, independent of the faster status poll, so a
 * busy/connecting poll never multiplies the task-snapshot allocation (critical
 * during a C6 OTA, when PSRAM is unavailable).
 */
#define P4_CONFIG_HEADER_TELEMETRY_PERIOD_MS    5000

/* Stack for the header telemetry sampler task. It runs the heap/task-snapshot/
 * battery reads OFF the LVGL task (a uxTaskGetSystemState() on the LVGL task
 * stalled the MIPI-DSI fetch and flashed the panel blue). Internal RAM: it must
 * survive a C6 OTA and never touches host flash. */
#define P4_CONFIG_TELEMETRY_TASK_STACK          3072

/** Uptime (seconds) below which the header uses the faster STARTUP cadence. */
#define P4_CONFIG_HEADER_STARTUP_GRACE_S        15

/** Backward-compatible alias for the old fixed refresh period. */
#define P4_CONFIG_HEADER_REFRESH_PERIOD_MS      P4_CONFIG_HEADER_REFRESH_IDLE_MS

/** Height of the input row (prompt line) in pixels. */
#define P4_CONFIG_INPUT_ROW_HEIGHT           52

/* ========================================================================
 * DISPLAY MANAGER PARAMETERS
 * ======================================================================== */

/** Default backlight brightness percentage on boot (0-100). */
#define P4_CONFIG_DISPLAY_DEFAULT_BRIGHTNESS 100

/** Display panel driver name for diagnostics. */
#define P4_CONFIG_DISPLAY_PANEL_DRIVER       "JD9165"

/** Touch controller driver name for diagnostics. */
#define P4_CONFIG_DISPLAY_TOUCH_DRIVER       "GT911"

/** `display stress` full-screen invalidation period (ms). A small value forces
 *  continuous redraws to reproduce a MIPI-DSI underrun (the "BSOD"). */
#define P4_CONFIG_DISPLAY_STRESS_PERIOD_MS   5

/**
 * AXI-ICM QoS for the MIPI-DSI framebuffer fetch (fixes the DSI underrun
 * "BSOD"). IDF never programs these; all masters default to priority 0, so the
 * DSI DW-GDMA read loses PSRAM arbitration under Wi-Fi/boot load and the panel
 * flashes blue. Raise the DW-GDMA read priority/burst so the display stays fed.
 */
#define P4_CONFIG_DISPLAY_ICM_QOS_ENABLE        1
/** DW-GDMA read QoS priority (0-15, higher wins). */
#define P4_CONFIG_DISPLAY_ICM_DW_GDMA_READ_PRIO 15
/** DW-GDMA read token-bucket depth (1-256; deeper tolerates longer stalls). */
#define P4_CONFIG_DISPLAY_ICM_DW_GDMA_BURST     256

/* ========================================================================
 * WINDOW MANAGER PARAMETERS
 * ======================================================================== */

/** Header height as percentage of display height. */
#define P4_CONFIG_WINDOW_HEADER_HEIGHT_PCT   7

/** Header height clamp minimum in pixels. */
#define P4_CONFIG_WINDOW_HEADER_HEIGHT_MIN   32

/** Header height clamp maximum in pixels. */
#define P4_CONFIG_WINDOW_HEADER_HEIGHT_MAX   56

/** Input row height as percentage of display height. */
#define P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_PCT 8

/** Input row height clamp minimum in pixels. */
#define P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_MIN 40

/** Input row height clamp maximum in pixels. */
#define P4_CONFIG_WINDOW_INPUT_ROW_HEIGHT_MAX 56

/** Minimum transcript height in pixels. */
#define P4_CONFIG_WINDOW_TRANSCRIPT_HEIGHT_MIN 40

/** Horizontal margin in pixels added to the screen root so no shell region
 *  (header, transcript, input row, keyboard) ever touches the display edges. */
#define P4_CONFIG_WINDOW_SCREEN_PAD_HOR       8

/** Width of the transcript scroll buttons (input-row Up/Dn) in pixels. */
#define P4_CONFIG_WINDOW_SCROLL_BUTTON_WIDTH  64

/* ========================================================================
 * FONT PARAMETERS
 * ========================================================================
 * Unified font registry (components/font): TERMINAL role for monospace
 * surfaces (transcript, TUI, editor) and UI role for chrome (header, input,
 * keyboard, dialogs). Phase 1: built-in bitmap names only ("unscii_16",
 * "montserrat_14"); terminal refuses proportional fonts. Live changes via
 * `font set <role> <name> [/save]` (saved to sd:/APPS/SHELL.INI, restored at
 * boot); pixel sizes arrive with SD TTFs in Phase 2.
 */

/** Default primary for the terminal role (must be monospace). */
#define P4_CONFIG_FONT_TERMINAL_DEFAULT       "unscii_16"

/** Default primary for the UI role (chained with a Montserrat fallback). */
#define P4_CONFIG_FONT_UI_DEFAULT             "unscii_16"

/** Maximum bytes for a font name (registry + SHELL.INI values). */
#define P4_CONFIG_FONT_NAME_BYTES             32

/** Nominal pixel size of built-in bitmap roles (unscii_16/montserrat_14). */
#define P4_CONFIG_FONT_DEFAULT_PX             16

/** Minimum selectable TTF pixel size. */
#define P4_CONFIG_FONT_SIZE_MIN               10

/** Maximum selectable TTF pixel size. */
#define P4_CONFIG_FONT_SIZE_MAX               28

/** Registry slots for loaded SD TTF objects ((stem, size) keyed). */
#define P4_CONFIG_FONT_TTF_SLOTS              6

/* ========================================================================
 * TUI / MODAL SURFACE PARAMETERS
 * ========================================================================
 * The TUI (text-mode) layer renders inside the shell's transcript container,
 * so its pixel size follows the transcript region dynamically (rotation,
 * on-screen keyboard). The logical grid size below is the DOS-like coordinate
 * system batch files address with `draw`/`locate` (1-based rows/cols).
 */

 /** Logical TUI grid columns (DOS 80 is the default). */
#define P4_CONFIG_TUI_COLS                      80

/** Logical TUI grid rows (DOS 25 is the default). */
#define P4_CONFIG_TUI_ROWS                      25

/** Maximum items `list` will show (truncates with a warning). */
#define P4_CONFIG_TUI_LIST_MAX_ITEMS            64

/** Maximum bytes for a file-browser selected path. */
#define P4_CONFIG_TUI_BROWSE_PATH_BYTES         P4_CONFIG_SD_PATH_BYTES

/** Maximum file bytes the text viewer / hex viewer will load. */
#define P4_CONFIG_TUI_VIEW_MAX_BYTES            (64 * 1024)

/** Default auto-cancel timeout for modal surfaces in ms (0 = no timeout). */
#define P4_CONFIG_TUI_TIMEOUT_DEFAULT_MS        0

/** Target frame rate for TUI frame-pacing stats (`tui stats`): frames slower
 * than 150% of this interval count as dropped. 0 disables dropped tracking. */
#define P4_CONFIG_TUI_TARGET_FPS                30

/* ========================================================================
 * IMAGE SUPPORT (BMP)
 * ========================================================================
 * One BMP decoder lives in components/gfx (24/32-bit BI_RGB, top-down or
 * bottom-up). The viewer, `draw image`, and `gfx image` reuse it. A source
 * image is never materialized at native size: it is decoded straight to a
 * fit-to-screen RGB565 target (bounded by GFX_IMAGE_MAX_W/H), so these caps
 * bound the file that can be opened while memory stays predictable. */

/** Largest BMP file the image path will ingest (a 1024x640 24-bit BMP is
 * ~1.9 MB; this covers it with headroom while keeping the staging buffer
 * bounded). */
#define P4_CONFIG_IMAGE_MAX_BYTES               (2 * 1024 * 1024)

/** Show the viewer fit-to-screen (1) or at native size only (0). */
#define P4_CONFIG_IMAGE_VIEWER_FIT              1

/* ========================================================================
 * KEYBOARD PARAMETERS
 * ======================================================================== */

/** Keyboard height as percentage of display height. */
#define P4_CONFIG_KEYBOARD_HEIGHT_PCT         35

/** Keyboard height clamp minimum in pixels. */
#define P4_CONFIG_KEYBOARD_HEIGHT_MIN         180

/** Keyboard height clamp maximum in pixels. */
#define P4_CONFIG_KEYBOARD_HEIGHT_MAX         280

/**
 * On-screen keyboard duplicate-press debounce window in milliseconds. The
 * shell drops a re-fire of the same button id within this window, which is
 * the same LVGL event reaching a second, stray handler (the root cause of
 * double OSK input). Deliberate fast repeats (auto-repeat, quick double-taps)
 * are far slower than this window and are never merged.
 */
#define P4_CONFIG_OSK_DEBOUNCE_MS             30

/* ========================================================================
 * WI-FI PARAMETERS
 * ======================================================================== */

/** Maximum bytes for a Wi-Fi SSID (including null terminator). */
#define P4_CONFIG_WIFI_SSID_BYTES            33

/** Maximum bytes for a Wi-Fi password (including null terminator). */
#define P4_CONFIG_WIFI_PASSWORD_BYTES        65

/** Maximum bytes for a Wi-Fi detail/status message. */
#define P4_CONFIG_WIFI_DETAIL_BYTES          256

/** Maximum bytes for a Wi-Fi origin/backend description string. */
#define P4_CONFIG_WIFI_ORIGIN_BYTES          32

/** Runtime guard: true when any Wi-Fi path is enabled in sdkconfig. */
#define P4_CONFIG_WIFI_RUNTIME_ENABLED \
     (CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_HOST_WIFI_ENABLED || CONFIG_ESP_HOSTED)

/** Skip ESP-Hosted version compatibility gate. When 1, the Wi-Fi init path
 *  does not reject C6 firmware version mismatches. Useful for development
 *  and testing with mismatched host/co-processor firmware. Set to 0 in
 *  production to enforce the version gate. */
#define P4_CONFIG_HOSTED_SKIP_VERSION_GATE  0

/** ESP-Hosted wire-protocol major the host requires on the C6. esp_hosted 3.x
 *  froze its public compat version macros (`ESP_HOSTED_VERSION_MAJOR_1` et
 *  al.) at the 2.12.6 baseline, so the gate must not use them. Minor/patch
 *  float within a major (RPC-V2 is wire-stable there); only the major gates. */
#define P4_CONFIG_HOSTED_COMPAT_MAJOR       3

/** I2C transaction timeout (ms) for the INA226 battery gauge. */
#define P4_CONFIG_BATTERY_I2C_TIMEOUT_MS    100

/** Maximum access points surfaced by `wifi scan`. Scan results are sorted by
 *  RSSI (strongest first) and capped at this value, so a busy channel cannot
 *  flood the transcript. */
#define P4_CONFIG_WIFI_SCAN_LIMIT           32

/**
 * Wi-Fi throughput bench (`wifi throughput`) parameters. The bench is a plain
 * lwIP TCP/UDP flood used to measure the ESP-Hosted SDIO transport; the host
 * endpoint is `tools/wifi_bench.py`.
 */
/** Payload bytes per send()/datagram (matches the Wi-Fi MTU). */
#define P4_CONFIG_WIFI_BENCH_CHUNK_BYTES    1460
/** Payload size used when `mb=` is not given. */
#define P4_CONFIG_WIFI_BENCH_DEFAULT_MB     16
/** Upper bound on `mb=` (guards against an accidental huge transfer). */
#define P4_CONFIG_WIFI_BENCH_MAX_MB         1024
/** Overall transfer budget before the bench aborts. */
#define P4_CONFIG_WIFI_BENCH_TIMEOUT_MS     30000
/** Default TCP/UDP port for the bench. */
#define P4_CONFIG_WIFI_BENCH_PORT           5201

/**
 * Maximum number of previously-used Wi-Fi networks kept in the persistent
 * known-network list (sd:/WIFI.KNOWN). When the list is full, the least
 * preferred / lowest-priority / oldest entry is dropped to make room.
 */
#define P4_CONFIG_WIFI_KNOWN_MAX            16

/**
 * SD-card relative filename of the persistent known-network list. Resolved
 * against the SD root via shell_sd_resolve_path() (accepts sd:/..., /sdcard/...,
 * or an SD-root-relative name). The file is plain text and hand-editable.
 */
#define P4_CONFIG_WIFI_KNOWN_FILE           "WIFI.KNOWN"

/**
 * When 1, a successful Wi-Fi connection (interactive `wifi connect`, CONFIG.SYS
 * WIFI_SSID/WIFI_PASSWORD, or known-network auto-connect) automatically updates
 * the known-network list on the SD card when the card is mounted. When 0, only
 * the explicit `wifi save` command persists to the list.
 */
#define P4_CONFIG_WIFI_KNOWN_AUTOSAVE       1

/**
 * Maximum bytes for a single line in the known-network file (one entry plus
 * delimiters and a trailing newline).
 */
#define P4_CONFIG_WIFI_KNOWN_LINE_BYTES     256

/* ========================================================================
 * PING AND DNS
 * ========================================================================
 * `ping` and `dns` (alias `nslookup`) are classic DOS-style connectivity
 * commands implemented inside components/networking (the sole owner of the
 * lwIP / esp_ping surface). They set ERRORLEVEL so batch files can branch,
 * and they participate in redirection and pipes like every other command. */

/** Default echo-request count for `ping` when no count is given. */
#define P4_CONFIG_PING_COUNT_DEFAULT        4

/* ========================================================================
 * TCP TERMINAL (tcpterm)
 * ========================================================================
 * One-shot TCP request/response sessions in components/networking (the
 * sole owner of the lwIP socket surface alongside ping/dns/http). Bounded
 * connect/send/idle budgets; remote SGR colours pass the sanitizer. */

/** Milliseconds a `tcpterm` connect may take before it is abandoned. */
#define P4_CONFIG_TCP_CONNECT_TIMEOUT_MS    10000

/** Default idle milliseconds with no reply data before `tcpterm` stops. */
#define P4_CONFIG_TCP_IDLE_TIMEOUT_MS       5000

/** Maximum reply bytes a `tcpterm` session buffers/prints. */
#define P4_CONFIG_TCP_RX_MAX_BYTES          65536

/** Maximum request bytes a `tcpterm` session sends. */
#define P4_CONFIG_TCP_TX_MAX_BYTES          65536

/** Hard upper bound for `ping <host> <count>`. A larger request is clamped. */
#define P4_CONFIG_PING_COUNT_MAX            10

/** Milliseconds each `ping` waits for a single reply before reporting a
 *  timeout. Also bounds how long the worker task can be blocked per reply. */
#define P4_CONFIG_PING_TIMEOUT_MS           1000

/** Milliseconds between two consecutive `ping` echo requests. */
#define P4_CONFIG_PING_INTERVAL_MS          1000

/** Payload size in bytes of each ICMP echo request. */
#define P4_CONFIG_PING_DATA_BYTES           32

/** Maximum A records `dns` / `nslookup` prints for one hostname. */
#define P4_CONFIG_DNS_RESULT_LIMIT          8

/* ========================================================================
 * CLOCK AND SNTP (date / time / timezone / sntp)
 * ========================================================================
 * The clock component owns the C-library system clock, timezone, and the SNTP
 * client. `date`, `time`, `timezone`, and `sntp`/`ntpsync` surface it from the
 * shell. */

/** NTP server hostname the SNTP client polls. */
#define P4_CONFIG_NTP_SERVER                "pool.ntp.org"

/** Maximum timezone string length (POSIX TZ string, e.g. "UTC" or
 *  "CET-1CEST,M3.5.0,M10.5.0/3"). */
#define P4_CONFIG_TIMEZONE_BYTES             64

/**
 * Unix timestamp (2020-01-01 UTC) at or above which the system clock is
 * considered set. Below it the clock is still at the boot epoch and the header
 * shows "--:--". Lets a manual `time`/`date` set count as valid, not only SNTP.
 */
#define P4_CONFIG_CLOCK_VALID_EPOCH          1577836800

/**
 * IP-geolocation endpoint used to auto-detect the local timezone from the
 * network (no hardcoded zone). The `/line/` form returns plain text lines
 * (timezone name, then UTC offset in seconds) so no JSON parser is needed.
 */
#define P4_CONFIG_TIMEZONE_URL               "http://ip-api.com/line/?fields=timezone,offset"

/** Re-detect the timezone this often (seconds) so DST/travel stays correct. */
#define P4_CONFIG_TIMEZONE_RESYNC_SECS       21600

/** Max timezone probes before giving up (then SNTP still runs in UTC). */
#define P4_CONFIG_TIMEZONE_MAX_ATTEMPTS      5

/** Stopwatch slots for the `timer` command (each holds one named run). */
#define P4_CONFIG_TIMER_SLOTS                8

/** Maximum bytes for a `timer` slot name (plus the terminator). */
#define P4_CONFIG_TIMER_NAME_BYTES            16

/* ---- RTC backup (clock_rtc.c: NVS anchor + optional external chip) ---- */

/** Refresh the NVS time anchor this often (seconds); 0 disables the timer
 *  (anchors still land on SNTP sync, manual set, and reboot). */
#define P4_CONFIG_RTC_ANCHOR_PERIOD_S        3600

/** Enable the external I2C RTC driver path (0 = internal RTC only). */
#define P4_CONFIG_RTC_EXT_ENABLE             0

/** 7-bit I2C address of the external RTC (DS3231 register map). */
#define P4_CONFIG_RTC_EXT_ADDR               0x68

/** I2C GPIO numbers for the external RTC (-1 = unset, driver stays off). */
#define P4_CONFIG_RTC_EXT_SDA                (-1)
#define P4_CONFIG_RTC_EXT_SCL                (-1)

/* ---- ARCHIVE (USTAR .p4a backups: archive create|extract|list|verify) ---- */

/** Maximum members (files+dirs+skipped) per archive operation. */
#define P4_CONFIG_ARCHIVE_MAX_ENTRIES        512

/** Streaming chunk bytes for archive data (create/extract/verify). */
#define P4_CONFIG_ARCHIVE_CHUNK_BYTES        4096

/** I2C port number for the external RTC. */
#define P4_CONFIG_RTC_EXT_PORT               0

/** Milliseconds for one external-RTC I2C transaction. */
#define P4_CONFIG_RTC_EXT_TIMEOUT_MS         50

/* ========================================================================
 * HTTP CLIENT (httpget / wget)
 * ========================================================================
 * `httpget <url> [localfile]` performs a simple HTTPS or HTTP GET through the
 * esp_http_client stack (the same one c6ota uses for firmware downloads).
 * All HTTP / TLS code lives inside components/networking; the command layer
 * only dispatches and writes the returned body to SD. ERRORLEVEL is 0 on an
 * HTTP 2xx, non-zero otherwise. */

/** Network timeout in milliseconds for a single `httpget` request. */
#define P4_CONFIG_HTTP_TIMEOUT_MS           15000

/** Maximum response body bytes `httpget` buffers into PSRAM. A larger
 *  response is refused with a clear error rather than exhausting memory. */
#define P4_CONFIG_HTTP_MAX_BODY_BYTES       (512 * 1024)

/** Follow HTTP redirects (1) or treat a 3xx status as an error (0). */
#define P4_CONFIG_HTTP_FOLLOW_REDIRECTS     1

/** User-Agent string sent by `httpget`; keep it short and identifiable. */
#define P4_CONFIG_HTTP_USER_AGENT           "P4MiniShell/" P4_CONFIG_VERSION_STRING " httpget"

/* ========================================================================
 * HTTP FILE SERVER (httpd) AND NETWORK DIAGNOSTICS
 * ========================================================================
 * A lightweight read-only HTTP file server that shares the SD card over the
 * Wi-Fi link, plus `netstat` and `ipconfig` companions. All of it lives in
 * components/networking (the sole owner of the esp_http_server / lwIP
 * surface). The server starts automatically when the station gets an IP and
 * stops on disconnect.
 */

/** Port the `httpd` file server listens on. */
#define P4_CONFIG_HTTPD_PORT                80

/** Maximum simultaneous client sockets (3 are reserved by the server). */
#define P4_CONFIG_HTTPD_MAX_OPEN_SOCKETS    4

/** TCP accept backlog for the server. */
#define P4_CONFIG_HTTPD_BACKLOG             4

/** Stack bytes for the dedicated HTTP server task. The handler builds
 *  path-sized scratch (URL + LFN) so it needs headroom over the IDF default
 *  (4096), but the 16 KB this used to request could not be allocated from
 *  the fragmented internal heap at Wi-Fi-up time and every start failed
 *  with ESP_ERR_HTTPD_TASK (seen on hardware). 8 KB verified sufficient
 *  via the httpd task's HeadB watermark in `ps`. */
#define P4_CONFIG_HTTPD_STACK_BYTES         8192

/** Priority of the HTTP server task. */
#define P4_CONFIG_HTTPD_TASK_PRIORITY       5

/** Receive timeout in seconds for an idle client socket. */
#define P4_CONFIG_HTTPD_RECV_TIMEOUT_S      5

/** Send timeout in seconds for a slow client socket. */
#define P4_CONFIG_HTTPD_SEND_TIMEOUT_S      5

/** Bytes read per chunk when streaming a file (heap-allocated per request). */
#define P4_CONFIG_HTTPD_BLOCK_BYTES         2048

/** Maximum directory-listing entries returned by the server. */
#define P4_CONFIG_HTTPD_LISTING_MAX         128

/** Auto-start the server when the station gets an IP and stop it on
 *  disconnect. When 0, only the manual `httpd start` starts it. */
#define P4_CONFIG_HTTPD_AUTOSTART           1

/** Cooldown in seconds before the auto-start retries after a failed start
 *  (e.g. task creation returning ESP_ERR_HTTPD_TASK while the heap is
 *  tight). Manual `httpd start` bypasses the cooldown. */
#define P4_CONFIG_HTTPD_AUTOSTART_RETRY_SECS  60

/** Basic-auth username; an empty string disables authentication. */
#define P4_CONFIG_HTTPD_AUTH_USERNAME       "admin"

/** Basic-auth password; used only when P4_CONFIG_HTTPD_AUTH_USERNAME is set. */
#define P4_CONFIG_HTTPD_AUTH_PASSWORD       "p4mini"

/** Maximum rows printed by `netstat` per TCP/UDP list. */
#define P4_CONFIG_NETSTAT_ROW_MAX           64

/** Maximum response body bytes printed to the transcript when no localfile is
 *  given. The full body is always available via `httpget <url> <localfile>`. */
#define P4_CONFIG_HTTP_PRINT_BODY_BYTES     4096

/* ========================================================================
 * BLUETOOTH PARAMETERS
 * ======================================================================== */

/** Log tag for Bluetooth operations. */
#define P4_CONFIG_BLUETOOTH_TAG              "bluetooth"

/** BLE device name used during advertising. */
#define P4_CONFIG_BLUETOOTH_DEVICE_NAME      "P4MiniShell BLE"

/** Maximum number of BLE scan results to report. */
#define P4_CONFIG_BLUETOOTH_DISCOVERY_LIMIT  16

/** Maximum bytes for a Bluetooth device name string. */
#define P4_CONFIG_BLUETOOTH_NAME_BYTES       32

/** Maximum bytes for a Bluetooth address string (XX:XX:XX:XX:XX:XX). */
#define P4_CONFIG_BLUETOOTH_ADDR_BYTES       18

/** Maximum BLE scan results surfaced through the shell bt/scan command. */
#define P4_CONFIG_BT_SCAN_LIMIT              8

/** Duration in milliseconds of a single `bluetooth scan` run. The scan is
 *  bounded so the command always terminates and the worker task never hangs. */
#define P4_CONFIG_BT_SCAN_DURATION_MS        8000

/* ========================================================================
 * SD CARD AND FILESYSTEM
 * ======================================================================== */

/** FatFs drive letter prefix for SD card access. */
#define P4_CONFIG_SD_FATFS_DRIVE             "0:"

/** DOS-style drive letter shown by `prompt $n` and `tree`. */
#define P4_CONFIG_SD_DRIVE_LETTER            "A:"

/** Maximum bytes for an SD card path (resolved absolute path). */
#define P4_CONFIG_SD_PATH_BYTES              320

/** Maximum directory entries to list in a single sd ls command. */
#define P4_CONFIG_SD_LIST_LIMIT              128

/** Default preview size in bytes for sd cat. */
#define P4_CONFIG_SD_CAT_DEFAULT_BYTES       1024

/** Maximum preview size in bytes for sd cat. */
#define P4_CONFIG_SD_CAT_MAX_BYTES           8192

/** I/O buffer size for SD file operations. Larger buffers reduce the number
 *  of small read/write calls for copy, pipe, and sd cat operations. Matches
 *  P4_CONFIG_FILE_IO_BUFFER_BYTES for consistency. */
#define P4_CONFIG_SD_IO_BUFFER_BYTES         512

/**
 * Pre-allocated SDMMC DMA scratch buffer size in bytes.
 *
 * The IDF sdmmc driver allocates a temporary DMA-capable buffer for every
 * multi-block SD card transaction when the host has no cached buffer
 * (`card->host.dma_aligned_buffer == NULL`). That allocation comes from the
 * internal (DMA-capable) heap, which is also shared with the WiFi/SDIO
 * transport and LVGL, and it can fail with `sdmmc_cmd: allocate_dma_buf:
 * not enough mem` once the internal heap fragments under load, taking every
 * SD command down with it.
 *
 * Pre-allocating the buffer once at mount time (when internal RAM is
 * abundant) and caching it in `card->host.dma_aligned_buffer` makes every
 * later transaction reuse it, so SD operations can never fail on memory.
 * The value must be an integer multiple of the card sector size (512 B);
 * the SDMMC chunk size is derived from it. Larger values speed up multi-
 * block transfers but reserve more internal RAM permanently.
 */
#define P4_CONFIG_SD_DMA_BUFFER_BYTES        4096

/**
 * Raise the calling task's priority for the duration of a guarded SD session.
 *
 * The SD card (slot 0) and the ESP-Hosted C6 transport (slot 1) share one
 * SDMMC controller, and every transaction on either slot takes the same
 * global driver mutex with an unbounded wait. The hosted transport tasks run
 * at priority 22 while the shell worker runs at 2, so under sustained hosted
 * traffic the worker loses every arbitration and an SD op can stall for a
 * minute or more while the console still echoes (O6). Raising the worker to
 * P4_CONFIG_SD_OP_BOOST_PRIORITY on session entry (restored on exit) lets it
 * win the next arbitration, so SD ops complete instead of starving. The boost
 * applies only inside SD sessions; a worker-loop backstop restores the base
 * priority after every command even if a session leaks.
 */
#define P4_CONFIG_SD_OP_BOOST              1

/** Priority used by the SD-session boost. Must outrank the hosted transport
 *  tasks (22) to break the starvation; kept just above them to minimize the
 *  time higher-priority work (hosted RX/TX, LVGL) waits behind an SD op. */
#define P4_CONFIG_SD_OP_BOOST_PRIORITY     23

/**
 * Enable the SD-op timing probe. When 1, guarded sessions timestamp entry
 * and `shell_sd_end()` logs any op slower than P4_CONFIG_SD_OP_TIMING_MS to
 * the serial log (no transcript spam). Off by default; enable for soaks when
 * hunting the O6 latency tail.
 */
#define P4_CONFIG_SD_OP_TIMING             0

/** SD-op overshoot threshold in milliseconds for the timing probe. */
#define P4_CONFIG_SD_OP_TIMING_MS          500

/**
 * Attempts for idempotent read-only filesystem queries (f_getfree and friends)
 * before reporting failure. The shared SDMMC controller serializes the SD card
 * and the hosted C6 link on one mutex, so a query can fail transiently under
 * contention (DMA OOM, bus CRC/timeout) even though the card is healthy; the
 * next attempt succeeds. Reads are side-effect free, so retrying is safe.
 * Writes are never retried implicitly.
 */
#define P4_CONFIG_SD_OP_RETRIES            3

/** Delay in milliseconds between read-only query attempts. Lets a burst of
 *  hosted SDIO traffic or a DMA-pressure spike clear before retrying. */
#define P4_CONFIG_SD_OP_RETRY_DELAY_MS     50

/* ========================================================================
 * DIRECTORY LISTING AND STORAGE GUARDRAILS
 * ======================================================================== */

/** Maximum entries buffered per directory level for `dir` sorting. */
#define P4_CONFIG_DIR_SORT_ENTRY_MAX         128

/** Columns printed by `dir /w` (wide listing). */
#define P4_CONFIG_DIR_WIDE_COLUMNS           4

/** Column width in characters for each `dir /w` cell. */
#define P4_CONFIG_DIR_WIDE_COLUMN_WIDTH      18

/** Rows shown between pauses by `dir /p`. */
#define P4_CONFIG_DIR_PAGE_LINES             20

/** Maximum directory recursion depth for `dir /s`. */
#define P4_CONFIG_DIR_RECURSE_DEPTH_MAX      8

/** Maximum matches printed by `find` in file-discovery mode before it stops
 *  and reports truncation, so a huge tree cannot flood the transcript. */
#define P4_CONFIG_FIND_MATCH_MAX             256

/**
 * Free-space safety margin in bytes. A write that would leave less than this
 * free is refused, so the volume never fills to the point where FAT metadata
 * updates start failing.
 */
#define P4_CONFIG_STORAGE_FREE_MARGIN_BYTES  (64 * 1024)

/**
 * File size in bytes above which copy operations report progress.
 * Keeps small copies silent while giving feedback on large transfers.
 */
#define P4_CONFIG_COPY_PROGRESS_THRESHOLD    (256 * 1024)

/** Percent step between `copy` progress updates for large files. */
#define P4_CONFIG_COPY_PROGRESS_STEP_PCT     10

/** Exact confirmation word required by any destructive operation (format,
 *  disk clean/delete, recursive delete, trash empty/purge). The `format`
 *  and `trash` commands share this one word (no per-command aliases). */
#define P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD   "YES"

/** Default allocation unit size in bytes requested when formatting. 0 = scale
 *  the cluster with the card capacity (storage_format_pick_alloc_unit): 4 KiB
 *  up to 512 MiB, 8 KiB to 8 GiB, 16 KiB to 16 GiB, 32 KiB to 32 GiB, 64 KiB
 *  above. Do NOT pass 0 through to esp_vfs_fat_sdcard_format_cfg: IDF clamps it
 *  up to the 512-byte sector size, which gives a 29 GiB card 1-sector clusters
 *  (a ~230 MiB FAT per copy) and a multi-minute blocking format. */
#define P4_CONFIG_FORMAT_ALLOC_UNIT_BYTES    0

/** Smallest `/A:` allocation unit (cluster) size accepted by `format`. */
#define P4_CONFIG_FORMAT_ALLOC_UNIT_MIN      4096

/** Largest `/A:` allocation unit (cluster) size accepted by `format`. */
#define P4_CONFIG_FORMAT_ALLOC_UNIT_MAX      (128 * 1024)

/** MBR partition-table alignment for `disk create partition` (in 512-byte
 *  sectors). 2048 sectors = 1 MiB, the diskpart/SD standard alignment. */
#define P4_CONFIG_DISK_PARTITION_ALIGN_SECTORS 2048

/* ========================================================================
 * RECYCLE BIN (TRASH)
 * ========================================================================
 * `del`/`erase` move matching files and directories into a hidden `.trash`
 * folder instead of deleting them, and `undelete` / `trash restore` bring
 * them back. `trash`/`recycle` manage the bin. All limits are enforced on
 * every trash operation (oldest entries are purged first).
 */

/** Enable the recycle bin. When 0, `del`/`erase` delete permanently again. */
#define P4_CONFIG_TRASH_ENABLE               1

/** Trash folder path (hidden from plain `dir`). */
#define P4_CONFIG_TRASH_PATH                 "sd:/.trash"

/** Maximum total bytes kept in the trash before the oldest entries are purged. */
#define P4_CONFIG_TRASH_MAX_BYTES            (32 * 1024 * 1024)

/** Maximum age in seconds of a trash entry before it is auto-purged. */
#define P4_CONFIG_TRASH_MAX_AGE_SEC          (7 * 24 * 3600)

/** Maximum number of trash entries before the oldest are purged. */
#define P4_CONFIG_TRASH_MAX_ENTRIES          256

/** Maximum file matches collected by one recursive `del /s` operation. */
#define P4_CONFIG_TRASH_OPERATION_MAX        256

/* ========================================================================
 * BOOT CONFIGURATION (CONFIG.SYS / AUTOEXEC.BAT)
 * ========================================================================
 * Optional DOS-style boot scripting. On every boot the firmware looks for
 * CONFIG.SYS and AUTOEXEC.BAT on the SD card, generates default files when
 * they are absent, applies the CONFIG.SYS directives, then runs AUTOEXEC.BAT
 * through the normal batch pipeline. All toggles, names and limits live here
 * and are mirrored in p4minishell_config.yaml. */

/** Filename checked for boot directives on the SD volume root. */
#define P4_CONFIG_BOOT_CONFIG_SYS_NAME       "CONFIG.SYS"

/** Filename checked for the boot batch file on the SD volume root. */
#define P4_CONFIG_BOOT_AUTOEXEC_BAT_NAME     "AUTOEXEC.BAT"

/** When no CONFIG.SYS / AUTOEXEC.BAT exist, generate default files once. */
#define P4_CONFIG_BOOT_GENERATE_DEFAULTS     1

/** Run CONFIG.SYS + AUTOEXEC.BAT on every normal boot. */
#define P4_CONFIG_BOOT_RUN_ON_STARTUP        1

/** Maximum line length accepted by the CONFIG.SYS parser (bytes). */
#define P4_CONFIG_BOOT_LINE_BYTES            256

/** Maximum number of directives CONFIG.SYS may contain. */
#define P4_CONFIG_BOOT_MAX_DIRECTIVES        64

/** Stack size for the dedicated task that applies CONFIG.SYS / AUTOEXEC.BAT
 *  once the SD card first mounts. Internal RAM: the boot script can run
 *  commands that touch flash/NVS, which cannot run from a PSRAM stack. */
#define P4_CONFIG_BOOT_SCRIPT_TASK_STACK     16384

/** Maximum bytes the `config` command reads/writes for the CONFIG.SYS file. */
#define P4_CONFIG_CONFIG_MAX_BYTES           16384

/**
 * Maximum bytes the generic INI (`ini` command, applib state) reads/writes for
 * a `KEY=VALUE` config file on the SD card.
 */
#define P4_CONFIG_INI_MAX_BYTES              16384

/**
 * Name of the directory (under the SD mount point) where temporary files
 * live: `sd:/<P4_CONFIG_TEMP_DIR_NAME>/`. All temp files live on the SD card.
 */
#define P4_CONFIG_TEMP_DIR_NAME              "tmp"

/* ========================================================================
 * BATCH ENGINE AND ENVIRONMENT VARIABLES
 * ======================================================================== */

/** Maximum number of RAM-only environment variables. Sized above the largest
 *  shipped reference app (SNAKE holds ~22) plus boot/alias variables, since
 *  the table is global across nested frames; a full table used to make `set`
 *  fail mid-loop and stall the worker (W2). */
#define P4_CONFIG_ENV_VAR_MAX                64

/* ========================================================================
 * ALIASES (alias / unalias, DOSKEY-style)
 * ========================================================================
 * RAM-only macro table. An alias's first word is expanded to its value when a
 * command is typed at the prompt (never inside batch files, matching DOSKEY),
 * so `alias ll=dir /s` then typing `ll` runs `dir /s`. The table persists to a
 * batch-style profile file on the SD card (`alias name=value` lines), which
 * boot.c auto-loads after CONFIG.SYS. */

/** Maximum number of RAM-only aliases. */
#define P4_CONFIG_ALIAS_MAX                  32

/** Maximum bytes for an alias name (including the null terminator). */
#define P4_CONFIG_ALIAS_NAME_BYTES           32

/** Maximum bytes for an alias value (including the null terminator). */
#define P4_CONFIG_ALIAS_VALUE_BYTES          256

/** SD-root-relative profile filename that `alias /save` writes and boot.c
 *  auto-loads after CONFIG.SYS (a batch file of `alias name=value` lines). */
#define P4_CONFIG_ALIAS_PROFILE              "ALIASES.BAT"

/* ========================================================================
 * KEY BINDS (bind F1..F12 + Ctrl+letter chords, USB keys)
 * ========================================================================
 * A small RAM-only table mapping USB HID function keys (F1 = 0x3A ..
 * F12 = 0x45) and Ctrl+letter chords (^A..^Z, ^C reserved for break) to
 * command lines. A bound key fires its line onto the command worker while
 * the prompt is idle, and chords additionally fire while a non-editor
 * modal owns the screen (never inside a key wait or a batch file). The
 * table persists to a batch-style profile file on the SD card
 * (`bind F5 <line>` / `bind ^G <line>` lines), which boot.c auto-loads
 * after the alias profile. */

/** Maximum number of bound keys (F-keys and chords share the table). */
#define P4_CONFIG_BIND_MAX                   12

/** Maximum bytes for a bind command line (including the null terminator). */
#define P4_CONFIG_BIND_VALUE_BYTES           256

/** SD-root-relative profile filename that `bind /save` writes and boot.c
 *  auto-loads after the alias profile (a batch file of `bind` lines). */
#define P4_CONFIG_BIND_PROFILE               "BIND.BAT"

/* ---- MACRO RECORDER (macro record/stop/play) ---- */

/** Capture buffer bytes for `macro record` (lines + newlines + NUL). */
#define P4_CONFIG_MACRO_BYTES                4096

/** Default capture file for `macro record` with no path (cwd-relative). */
#define P4_CONFIG_MACRO_DEFAULT_FILE         "MACRO.BAT"

/** Maximum bytes for an environment variable name. */
#define P4_CONFIG_ENV_NAME_BYTES             32

/** Maximum bytes for an environment variable value. */
#define P4_CONFIG_ENV_VALUE_BYTES            256

/** Maximum bytes for a single batch file line. */
#define P4_CONFIG_BATCH_LINE_BYTES           384

/** Maximum batch script arguments (%1 through %9). */
#define P4_CONFIG_BATCH_ARGS_MAX             9

/**
 * Maximum batch file size loaded into RAM for execution. A batch file at or
 * below this size is read once into a PSRAM buffer and executed from memory,
 * so `goto`-heavy loops never touch the SD card mid-loop. Larger files fall
 * back to streaming reads (identical semantics, SD-bound).
 */
#define P4_CONFIG_BATCH_FILE_MAX_BYTES       131072

/* ---- App packages (`pkg` verb) ----
 * A package is metadata (`APPS/<APP>.APPINFO`: title/description/version)
 * plus a contents manifest (`APPS/<APP>.ASSETS`: `path=HEXCRC` lines). The
 * install source is a bundle directory `PKGS/<APP>/` holding the same manifest
 * plus the payload files mirrored at their install-relative paths. */
#define P4_CONFIG_PKG_BUNDLE_DIR_NAME        "PKGS"
#define P4_CONFIG_PKG_APPS_DIR_NAME          "APPS"
#define P4_CONFIG_PKG_MAX_ENTRIES            48
#define P4_CONFIG_PKG_MANIFEST_BYTES         16384
#define P4_CONFIG_PKG_LINE_BYTES             512

/** Native-app ABI tag (see docs/native_packaging.md). `pkg install` warns
 *  when a native bundle's `abi=` differs; the future loader will refuse. */
#define P4_CONFIG_NATIVE_ABI                 "applib-1"

/** Maximum nested batch file call depth. */
#define P4_CONFIG_BATCH_DEPTH_MAX            4

/** Maximum background batch tasks (`start` pool) beyond the main worker.
 * Each extra slot is one ctx struct (BSS); tasks themselves are created on
 * demand with the same stack as the main worker. */
#define P4_CONFIG_BG_TASKS                   1

/** I/O buffer size for general file operations. */
#define P4_CONFIG_FILE_IO_BUFFER_BYTES       512

/** Maximum `:label` targets tracked per batch file for goto/call/gosub/on.
 *  Must stay above the label count of the largest shipped app (TCMD.BAT uses
 *  34): an over-limit label is dropped from the table, so a jump to it fails
 *  like any other missing label. */
#define P4_CONFIG_BATCH_LABEL_MAX            128

/**
 * Maximum bytes for a single `:label` name.
 * A label is one identifier, so this is far smaller than a command line.
 * The label table is sized LABEL_MAX * LABEL_BYTES, so keeping this tight
 * matters for the batch frame footprint. Each frame is heap-allocated, so
 * this only affects the heap, never the worker stack.
 */
#define P4_CONFIG_BATCH_LABEL_BYTES          64

/** Maximum argv slots produced by the command tokenizer. */
#define P4_CONFIG_COMMAND_ARGV_MAX           32

/** Maximum lines the `sort` command can hold in memory. */
#define P4_CONFIG_SORT_LINE_MAX              1024

/** Lines printed per page by the `more` command. */
#define P4_CONFIG_MORE_PAGE_LINES            20

/**
 * Fallback delay between `more` pages in milliseconds. Used only when no
 * interactive key source is attached; otherwise `more` waits for a keypress.
 */
#define P4_CONFIG_MORE_PAGE_DELAY_MS         1500

/** Maximum length of a single `findstr` search string or regex pattern. */
#define P4_CONFIG_FINDSTR_PATTERN_BYTES      256

/** Maximum number of `findstr` search strings accepted on one command line. */
#define P4_CONFIG_FINDSTR_MAX_STRINGS        8

/**
 * Maximum matches `findstr /S` prints before it stops and reports truncation,
 * so a huge tree cannot flood the transcript.
 */
#define P4_CONFIG_FINDSTR_MATCH_MAX          256

/**
 * Mismatches `comp` reports before ending the comparison (DOS prints the
 * first 10 and stops).
 */
#define P4_CONFIG_COMP_MISMATCH_MAX          10

/**
 * Fallback delay used by `pause` in milliseconds. Used only when no
 * interactive key source is attached; otherwise `pause` waits for a keypress.
 */
#define P4_CONFIG_PAUSE_DELAY_MS             2000

/** Settle delay between two stages of a piped command in milliseconds. */
#define P4_CONFIG_PIPE_SETTLE_DELAY_MS       100

/** Maximum command stages in a single `a | b | c` pipeline. */
#define P4_CONFIG_PIPE_STAGE_MAX             4

/* ========================================================================
 * QUOTING, ESCAPING, AND COMMAND CHAINING
 * ======================================================================== */

/**
 * Escape character. Follows COMMAND.COM: a caret makes the next character
 * literal, so `^&`, `^|`, `^>`, `^"`, and `^^` lose their special meaning.
 */
#define P4_CONFIG_ESCAPE_CHAR                '^'

/** Maximum commands in a single `a & b && c || d` chain. */
#define P4_CONFIG_CHAIN_SEGMENT_MAX          8

/**
 * Errorlevel reported when a command name is not recognized.
 * COMMAND.COM uses 9009 for "command not found"; conditional chaining relies
 * on a non-zero value here so `badcmd || echo failed` behaves correctly.
 */
#define P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND 9009

/** Maximum nesting depth for `setlocal` / `endlocal` environment scopes. */
#define P4_CONFIG_SETLOCAL_DEPTH_MAX         8

/** Maximum parenthesis nesting depth in a `set /a` arithmetic expression. */
#define P4_CONFIG_SET_EXPR_DEPTH_MAX         16

/** Maximum bytes of user input accepted by `set /p`. */
#define P4_CONFIG_SET_PROMPT_INPUT_BYTES     128

/* ========================================================================
 * CALCULATOR (`calc` command + floating-point expression evaluator)
 * ========================================================================
 * `calc` evaluates a floating-point expression with the FX-870P/VX-4 BASIC
 * math and string functions (ABS, SIN, COS, TAN, SINH, COSH, TANH, ASINH,
 * ACOSH, ATANH, ASN, ACS, ATN, HYP, SQR, EXP, LN, LOG, FACT, NCR, NPR, INT,
 * FIX, FRAC, ROUND, SGN, MOD, PI, RAN#, POL, REC, DMS/DMS$, DEG, CUR,
 * VAL/VALF, STR$, HEX$, ASC, CHR$, LEN, LEFT$, MID$, RIGHT$, `&H`/`0x` hex
 * literals), financial functions (PV, FV, PMT, NPER, RATE, NPV, IRR, SLN,
 * SYD, DB) and date functions (DATE, YEAR, MONTH, DAY, DOW, TODAY, DATEADD,
 * DAYS, EOMONTH, DATEVALUE, DATESTR) and an ANGLE degree/radian mode. It lives in components/batch
 * (calc.c) and is a batch language verb like `set`.
 */

/** Maximum bytes of a string result or string argument in a `calc` expression. */
#define P4_CONFIG_CALC_STR_BYTES             32

/** Maximum function arguments in a `calc` call (financial NPV/IRR lists,
 *  RATE with guess). */
#define P4_CONFIG_CALC_ARG_MAX               8

/** Maximum parenthesis / function nesting depth in a `calc` expression. */
#define P4_CONFIG_CALC_MAX_DEPTH             16

/** Trig angle mode at boot: 1 = degrees (calculator default), 0 = radians. */
#define P4_CONFIG_CALC_ANGLE_DEFAULT_DEG     1

/** Significant digits used when printing a `calc` numeric result. */
#define P4_CONFIG_CALC_PRINT_PRECISION       12

/**
 * `plot` coordinate-layer verbs (plot_commands.c): scientific graphs, charts
 * and world-coordinate drawings over the `gfx` canvas or the TUI grid,
 * sampling `calc` expressions. All bounds keep batch loops and data files
 * from flooding the transcript or the heap.
 */

/** Default/max samples per `plot func|polar|para` curve. */
#define P4_CONFIG_PLOT_SAMPLES               240

/** Maximum data points (plot data), values (plot bar) or table rows. */
#define P4_CONFIG_PLOT_MAX_POINTS            512

/** Longest data/bar file line `plot` will read. */
#define P4_CONFIG_PLOT_LINE_BYTES            128

/** Target tick count for `plot axes` nice-step spacing. */
#define P4_CONFIG_PLOT_TICK_TARGET           8

/**
 * Maximum batch lines joined by trailing `^` continuations.
 * Bounds a runaway continuation chain so a malformed file cannot loop.
 */
#define P4_CONFIG_LINE_CONTINUATION_MAX      8

/* ========================================================================
 * `for /f` FILE-LINE LOOPS
 * ========================================================================
 * `for /f "eol=c skip=n delims=xyz tokens=a,b,m-n" %%v in (file-set) do cmd`
 * iterates over the lines of a file (or the active `< file` / pipe input).
 * These bounds keep one line's worth of state small enough for the recursive
 * batch path's heap budget.
 */

/** Maximum tokens a `for /f` line may be split into (tokens=a,b,c). */
#define P4_CONFIG_FORF_TOKEN_MAX             8

/** Maximum lines one `for /f` iteration pass reads from a source. */
#define P4_CONFIG_FORF_LINE_MAX              4096

/** Maximum length of the `for /f` `delims=` character set (bytes). */
#define P4_CONFIG_FORF_DELIMS_BYTES          16

/* ========================================================================
 * CSV GRID VERBS (csv rows|cols|cell|eval)
 * ========================================================================
 * Minimal spreadsheet substrate for batch apps: RFC-4180-subset parsing
 * (comma separators, `"quoted"` fields, `""` escapes) plus `=EXPR` formula
 * evaluation through the `calc` engine with `R<row>C<col>` references.
 * Bounds keep one sheet in the heap budget of the command worker path. */

 /** Maximum columns the `csv` verbs track per row (extras are ignored). */
#define P4_CONFIG_CSV_MAX_COLS               32

/** Maximum bytes of one dequoted CSV field. */
#define P4_CONFIG_CSV_FIELD_BYTES            256

/** Maximum rows the `csv eval` grid holds (extras are ignored). */
#define P4_CONFIG_CSV_ROWS_MAX               256

/** Formula-resolution passes for `csv eval` (`R1C1`-style references). */
#define P4_CONFIG_CSV_PASSES                 8

/* ========================================================================
 * APPLIB (native-app runtime library, components/applib)
 * ========================================================================
 * The shell SDK surface for native apps: console output through the transcript
 * (the redirection layer), a shared memory-allocation policy, time/sleep/
 * system-info helpers, and Wi-Fi state accessors via a registered ops table.
 */

/**
 * Blocks of this size or larger are allocated from PSRAM (when available) by
 * `app_alloc`/`app_calloc`/`app_realloc`; smaller blocks use the internal
 * heap. The PSRAM path falls back to the internal heap when PSRAM is absent
 * or exhausted, so callers only ever NULL-check the result.
 */
#define P4_CONFIG_APPLIB_PSRAM_THRESHOLD_BYTES  512

/** Maximum number of native apps registered in the applib app table. */
#define P4_CONFIG_APP_MAX                       8

/** Maximum length of a registered native app's command name. */
#define P4_CONFIG_APP_NAME_BYTES                32

/** Maximum length of a registered native app's one-line description. */
#define P4_CONFIG_APP_DESC_BYTES                64

/** Maximum number of script apps (.bat/.cmd) surfaced by the `launch`
 * command / menu. Sized past the bundled app count so neither extension
 * starves the other during discovery. */
#define P4_CONFIG_LAUNCH_MAX                    24

/** Maximum length of the `choice` /C: key list (characters). Single-char
 *  keys only; DOS caps this list at 26, Windows at 99. */
#define P4_CONFIG_CHOICE_KEY_MAX              64

/**
 * Maximum bytes captured for a command's `>` / `>>` redirection. The capture
 * is a dedicated buffer (independent of the 16 KB transcript), so a command
 * producing more than the transcript size is still redirected correctly up to
 * this cap. When the cap is exceeded the excess is dropped and the shell warns.
 */
#define P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES  (256 * 1024)

/**
 * Maximum nesting depth of redirection captures. A redirected command that
 * internally runs another redirected command (a pipeline whose stages spool to
 * their own files inside an outer `>` / `>>`) nests one capture per stage; the
 * stack keeps the outer capture intact until the inner ones finish.
 */
#define P4_CONFIG_REDIRECT_CAPTURE_MAX_DEPTH  8

/** Maximum directory recursion depth for the `tree` command. */
#define P4_CONFIG_TREE_DEPTH_MAX             8

/** Maximum bytes for a user-defined `prompt` template string. */
#define P4_CONFIG_PROMPT_TEMPLATE_BYTES      64

/** Default `prompt` template. `$p` expands to the path, `$g` to `>`. */
#define P4_CONFIG_PROMPT_DEFAULT_TEMPLATE    "PS $p$g "

/** Depth of the interactive keypress queue used by `pause`, `choice`, `more`. */
#define P4_CONFIG_KEY_QUEUE_DEPTH            16

/**
 * Timeout in milliseconds for a single blocking keypress wait. Prevents a
 * headless board from stalling a batch file forever.
 */
#define P4_CONFIG_KEY_WAIT_TIMEOUT_MS        10000

/** Delay before esp_restart() so the reboot message reaches the transcript. */
#define P4_CONFIG_REBOOT_DELAY_MS            500

/** Default duration in seconds for `sleep` when no duration is given. */
#define P4_CONFIG_POWER_SLEEP_DEFAULT_SECS   60

/** Maximum duration in seconds accepted by `sleep`/`deepsleep`. */
#define P4_CONFIG_POWER_SLEEP_MAX_SECS       86400

/** Delay in ms after printing before entering sleep so the transcript paints. */
#define P4_CONFIG_POWER_SLEEP_PRE_DELAY_MS   150

/** Maximum milliseconds accepted by the `delay` command (pure wait). */
#define P4_CONFIG_DELAY_MAX_MS               10000

/** Tear down Wi-Fi/hosted state when entering light sleep (`sleep`). */
#define P4_CONFIG_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI 1

/**
 * Turn the display off (backlight) after this many seconds without user input
 * (touch, USB keyboard/mouse, or a serial command). 0 disables the idle
 * timeout. Managed at runtime with `power idle <seconds|off>` and at boot with
 * the CONFIG.SYS `DISPLAY_TIMEOUT=` directive.
 */
#define P4_CONFIG_POWER_IDLE_DISPLAY_OFF_SECS   0

/** Maximum idle timeout accepted by `power idle` (seconds). */
#define P4_CONFIG_POWER_IDLE_DISPLAY_MAX_SECS   86400

/**
 * GPIO that wakes light/deep sleep when pulled to its active level. Defaults
 * to GPIO_NUM_NC (no GPIO wake); set it to a user-wired button/switch pin.
 * The GT911 touch interrupt line (BOARD_CFG_LCD_TOUCH_INT_GPIO) is not wired
 * on this board, so touch cannot wake sleep directly.
 */
#define P4_CONFIG_POWER_WAKE_GPIO               GPIO_NUM_NC

/** Wake level for P4_CONFIG_POWER_WAKE_GPIO (0 = low, 1 = high). */
#define P4_CONFIG_POWER_WAKE_LEVEL              1

/** Line buffer size for text-processing commands (find, more, fc, sort). */
#define P4_CONFIG_TEXT_LINE_BYTES            512

/** Buffer size for FATFS long file names (matches CONFIG_FATFS_MAX_LFN + 1). */
#define P4_CONFIG_LFN_BYTES                  256

/** Default speaker volume percentage applied at boot. */
#define P4_CONFIG_VOLUME_DEFAULT_PCT         60

/** Default frequency (Hz) and duration (ms) of the `beep` command. */
#define P4_CONFIG_BEEP_FREQ_HZ               880
#define P4_CONFIG_BEEP_DURATION_MS           100

/** Accepted frequency range for `tone <freq> [ms]`. */
#define P4_CONFIG_TONE_FREQ_MIN              20
#define P4_CONFIG_TONE_FREQ_MAX              20000

/** Maximum duration in ms accepted by `tone`/`wavplay` (bounded so the audio
 *  playback task always terminates). */
#define P4_CONFIG_TONE_DURATION_MAX_MS       5000
/** Default duration in ms of `tone <freq>` when no duration is given. */
#define P4_CONFIG_TONE_DURATION_DEFAULT_MS   200

/** Peak amplitude of a generated tone as a percentage of full scale (kept
 *  below 100 so a loud codec volume does not clip). */
#define P4_CONFIG_TONE_AMPLITUDE_PCT         40

/** Samples generated per chunk while playing a tone (16-bit mono). */
#define P4_CONFIG_TONE_CHUNK_SAMPLES         1024

/** Maximum WAV file size in bytes accepted by `wavplay`. */
#define P4_CONFIG_WAV_MAX_BYTES              (1024 * 1024)

/** Stack size for the background audio playback task. */
#define P4_CONFIG_AUDIO_TASK_STACK           8192

/** Priority of the background audio playback task. */
#define P4_CONFIG_AUDIO_TASK_PRIORITY        2

/**
 * Size of the RAM clipboard used by `clip` / `paste`. It holds either copied
 * transcript text, a text file's contents (`clip read`), or a file reference
 * (`clip file`).
 */
#define P4_CONFIG_CLIPBOARD_BYTES            2048

/** Maximum lines `clip copy [N]` accepts from the transcript. */
#define P4_CONFIG_CLIP_COPY_LINES_MAX        64

/* ========================================================================
 * GPIO AND HARDWARE CONTROL
 * ======================================================================== */

/** GPIO number for the ESP32-C6 hosted reset line. */
#define P4_CONFIG_C6_HOST_RESET_GPIO         54

/** ADC attenuation used for battery voltage reading. */
#define P4_CONFIG_BATTERY_ATTEN              3  /* ADC_ATTEN_DB_12 */

/** Minimum CPU frequency in MHz for light sleep entry. */
#define P4_CONFIG_BATTERY_MIN_SLEEP_FREQ_MHZ 40

/* ========================================================================
 * PERIPHERAL TOOLKIT (pwm, freq, adc, i2c, spi)
 * ========================================================================
 * The `pwm`/`freq`/`adc`/`i2c`/`spi` shell commands drive the LEDC, ADC,
 * I2C master, and SPI master drivers on non-reserved GPIOs. All values here
 * are tunable; the pin-safety table lives in command.c (board reserved
 * lines are refused by every peripheral command).
 */

/** PWM: maximum frequency in Hz accepted by `pwm`/`freq`. */
#define P4_CONFIG_PWM_FREQ_MAX_HZ             100000

/** PWM: assumed timer source clock in Hz used to pick the duty resolution.
 *  Matches the shared LEDC global clock (XTAL, 40 MHz) that the display
 *  backlight auto-selects. */
#define P4_CONFIG_PWM_SRC_CLK_HZ              40000000

/** PWM: LEDC clock source for toolkit timers. Must match the global LEDC
 *  clock the display backlight auto-selects (SOC_MOD_CLK_XTAL, the value
 *  behind LEDC_USE_XTAL_CLK), because all LEDC timers share one global
 *  clock and a mismatch fails with "timer clock conflict". */
#define P4_CONFIG_PWM_CLK_SOURCE              18

/** PWM: maximum concurrent outputs (limited by free LEDC timers on the P4). */
#define P4_CONFIG_PWM_CHANNEL_MAX             3

/** PWM: default duty in percent applied by `freq` (50 % square wave). */
#define P4_CONFIG_PWM_DUTY_DEFAULT_PCT        50

/** ADC: default number of samples averaged by `adc <pin> [samples]`. */
#define P4_CONFIG_ADC_DEFAULT_SAMPLES         1

/** ADC: maximum number of samples accepted by the `adc` command. */
#define P4_CONFIG_ADC_MAX_SAMPLES             64

/** ADC: attenuation used by the `adc` command (ADC_ATTEN_DB_12 = 0..3.3 V). */
#define P4_CONFIG_ADC_ATTEN                   3

/** I2C: timeout for scan/peek/poke transactions in milliseconds. */
#define P4_CONFIG_I2C_TOOL_TIMEOUT_MS         100

/** I2C: per-address probe timeout for `i2c scan` in milliseconds. A scan
 *  only needs a quick ACK check, so this stays small to keep the bus
 *  busy time bounded (117 addresses * probe timeout). */
#define P4_CONFIG_I2C_SCAN_PROBE_TIMEOUT_MS   10

/** I2C: first 7-bit address probed by `i2c scan`. */
#define P4_CONFIG_I2C_SCAN_FIRST_ADDR         0x03

/** I2C: last 7-bit address probed by `i2c scan`. */
#define P4_CONFIG_I2C_SCAN_LAST_ADDR          0x77

/** I2C: clock speed in Hz for user-supplied sda/scl buses. */
#define P4_CONFIG_I2C_TOOL_CLK_HZ             100000

/** SPI: default clock speed in Hz for tool transactions. */
#define P4_CONFIG_SPI_TOOL_CLK_HZ             1000000

/** SPI: timeout for a single transaction in milliseconds. */
#define P4_CONFIG_SPI_TOOL_TIMEOUT_MS         100

/** SPI: maximum transaction buffer size in bytes (loopback pattern). */
#define P4_CONFIG_SPI_TOOL_BUFFER_BYTES       16

/** SPI: host controller used by the toolkit (SPI3_HOST = 2). SPI3 is chosen
 *  because SPI2's direct IOMUX pins overlap the board's I2C/I2S lines; SPI3
 *  routes through the GPIO matrix only. */
#define P4_CONFIG_SPI_TOOL_HOST               2

/* ========================================================================
 * RGB STATUS LED (WS2812 on GPIO26)
 * ========================================================================
 * The Guition JC1060P470 board has a WS2812 (NeoPixel-style) RGB LED on the
 * back panel, wired to GPIO26. components/led owns the driver (via the
 * espressif/led_strip component over RMT) plus an auto status layer and
 * transient event notifications. The `rgb` shell command and the CONFIG.SYS
 * `RGB=` directive drive it.
 */

/** WS2812 data GPIO (LED1 on the JC1060P470 back panel). */
#define P4_CONFIG_LED_GPIO                    26

/** RMT tick resolution used by the WS2812 driver, in Hz. */
#define P4_CONFIG_LED_RMT_RESOLUTION_HZ       (10 * 1000 * 1000)

/** RMT symbol blocks allocated for the one-LED strip. */
#define P4_CONFIG_LED_RMT_SYMBOLS             64

/** Maximum colour intensity as a percentage (scales every colour so the
 *  back-panel LED is never blindingly bright). */
#define P4_CONFIG_LED_MAX_BRIGHTNESS_PCT      50

/** Stack bytes for the LED animation/notification task. */
#define P4_CONFIG_LED_TASK_STACK_BYTES        2048

/** Animation tick period in milliseconds (the frame rate of effects). */
#define P4_CONFIG_LED_TICK_MS                 10

/** Duration in milliseconds of a transient event notification colour before
 *  the LED returns to its persistent status colour. */
#define P4_CONFIG_LED_NOTIFY_MS               1500

/** Duration in milliseconds of the boot-OK confirmation flash. */
#define P4_CONFIG_LED_BOOT_FLASH_MS           600

/** Default effect speed for `rgb <effect> [speed]` (1..10). */
#define P4_CONFIG_LED_EFFECT_SPEED_DEFAULT    5

/** Start with the auto status layer enabled (Wi-Fi state colours). */
#define P4_CONFIG_LED_AUTO_STATUS             1

/** Event colours as 0xRRGGBB. */
#define P4_CONFIG_LED_COLOR_BOOT_OK           0x00FF00
#define P4_CONFIG_LED_COLOR_WIFI_CONNECTING   0xFF9900
#define P4_CONFIG_LED_COLOR_WIFI_CONNECTED    0x00FF00
#define P4_CONFIG_LED_COLOR_WIFI_DISCONNECTED 0xFF0000
#define P4_CONFIG_LED_COLOR_WIFI_ERROR        0xFF4040
#define P4_CONFIG_LED_COLOR_HTTPD             0x0080FF
#define P4_CONFIG_LED_COLOR_ALARM             0x00FFFF

/* ========================================================================
 * TAB5 KEYBOARD (components/tab5kbd)
 * ========================================================================
 * The M5Stack Tab5Keyboard is an optional I2C module (STM32F030, address
 * 0x6D) on the Tab5 expansion port. It is compiled on every board but only
 * activates where BOARD_CFG_TAB5KBD_PRESENT is set; the pins/address live in
 * board_config.h. The driver is compile-verified only (no Tab5 hardware test
 * yet). */

/** I2C transaction timeout for one Tab5Keyboard register access (ms). */
#define P4_CONFIG_TAB5KBD_I2C_TIMEOUT_MS      50

/** Poll cadence for pending Tab5Keyboard events (ms). The module also has an
 *  interrupt line; polling is the board-agnostic fallback. */
#define P4_CONFIG_TAB5KBD_POLL_PERIOD_MS      20

/** Stack bytes for the Tab5Keyboard event poll task. */
#define P4_CONFIG_TAB5KBD_TASK_STACK          3072

/* ========================================================================
 * HEADER BAR VISUAL STYLING
 * ======================================================================== */

/** Maximum bytes for a header notification string. */
#define P4_CONFIG_HEADER_NOTIFICATION_BYTES  160

/** Header text color (light green). */
#define P4_CONFIG_HEADER_TEXT_COLOR          0xC7FFD0

/** Header muted/inactive color (dim green). */
#define P4_CONFIG_HEADER_MUTED_COLOR         0x5E7063

/** Header accent/active color (bright green). */
#define P4_CONFIG_HEADER_ACCENT_COLOR        0x8DFF96

/** Header warning color (amber). */
#define P4_CONFIG_HEADER_WARN_COLOR          0xF0C36E

/** Header background color (dark green-black). */
#define P4_CONFIG_HEADER_BG_COLOR            0x111816

/** Header panel/transcript background color. */
#define P4_CONFIG_HEADER_PANEL_COLOR         0x050806

/** Header CPU bar warning threshold (percent). */
#define P4_CONFIG_HEADER_CPU_WARN_PCT        85

/** Header CPU critical threshold (percent) — indicator turns red at or above. */
#define P4_CONFIG_HEADER_CPU_CRIT_PCT        95

/**
 * Show a small CPU history sparkline in the header system panel instead of
 * the single-value CPU bar. 1 = sparkline, 0 = the plain bar.
 */
#define P4_CONFIG_HEADER_CPU_GRAPH           1

/** Number of CPU samples held by the header sparkline ring. */
#define P4_CONFIG_HEADER_CPU_GRAPH_POINTS    12

/** Width of the header CPU sparkline canvas in pixels. */
#define P4_CONFIG_HEADER_CPU_GRAPH_WIDTH_PX  26

/** Header memory low threshold (percent). */
#define P4_CONFIG_HEADER_MEM_LOW_PCT         30

/** Header memory critical threshold (percent free) — indicator turns red at or below. */
#define P4_CONFIG_HEADER_MEM_CRIT_PCT        15

/** Header battery low threshold (percent). */
#define P4_CONFIG_HEADER_BAT_LOW_PCT         15

/** Header battery critical threshold (percent) — indicator turns red at or below. */
#define P4_CONFIG_HEADER_BAT_CRIT_PCT        5

/** Display duration for transient header notifications in milliseconds. */
#define P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS   3000

/**
 * Depth of the header notification queue. Notifications are shown FIFO; when
 * the queue is full the oldest QUEUED entry is dropped (the one currently
 * displayed is never dropped) so the newest alert always enters.
 */
#define P4_CONFIG_HEADER_NOTIFY_QUEUE        4

/** Show the local time (HH:MM) in the idle notification area (1 = enabled). */
#define P4_CONFIG_HEADER_CLOCK               1

/** Show the activity indicator (`A`) while a C6 OTA or a bg job runs. */
#define P4_CONFIG_HEADER_ACTIVITY            1

/**
 * Minimum usable width (px) reserved for the center notification region.
 * When the side panels plus this budget do not fit the screen, the
 * notification yields first (see header_layout.c) before any indicator is
 * abbreviated or dropped.
 */
#define P4_CONFIG_HEADER_CENTER_MIN_PX       56

/**
 * Allow the header to drop to a smaller chained font when even the most
 * compact side-panel layout does not fit (dynamic font sizing). Only used in
 * AUTO mode. 1 = enabled.
 */
#define P4_CONFIG_HEADER_DYNAMIC_FONT        1

/**
 * Status-indicator presentation style.
 *   words  — verbose labels ("WiFi HI", "USB ON", "SD ON")
 *   glyph  — one compact colored ASCII glyph per indicator ("W", "U", "S"),
 *            which returns the reclaimed width to the notification area.
 * Both styles color the glyph/label by state (see header_status.c).
 */
#define P4_CONFIG_HEADER_STATUS_WORDS        0
#define P4_CONFIG_HEADER_STATUS_GLYPH        1
#define P4_CONFIG_HEADER_STATUS_STYLE        P4_CONFIG_HEADER_STATUS_GLYPH

/** Enable tap/long-press detail on a status indicator (1 = enabled). */
#define P4_CONFIG_HEADER_DETAIL_ON_TAP       1

/** Wi-Fi RSSI signal thresholds (dBm), strongest first. */
#define P4_CONFIG_HEADER_RSSI_STRONG         (-55)
#define P4_CONFIG_HEADER_RSSI_GOOD           (-68)
#define P4_CONFIG_HEADER_RSSI_WEAK           (-80)

/** Sentinel RSSI reported when no Wi-Fi AP information is available (dBm). */
#define P4_CONFIG_HEADER_RSSI_UNKNOWN        (-127)

/* ========================================================================
 * ANSI/VT TERMINAL COLOR PALETTE — Windows 11 PowerShell Theme
 * ========================================================================
 * Colors match the Windows 11 PowerShell default color scheme.
 * Reference: Windows Terminal "Campbell PowerShell" / PSReadLine defaults.
 *   - Default text: Light gray (#CCCCCC) on dark background (#012456)
 *   - Errors: Bright red (#E74856)
 *   - Warnings: Yellow (#F9F1A5)
 *   - Success: Green (#16C60C)
 *   - Paths: Cyan (#61D6D6)
 *   - Prompt path: Bright yellow (#F9F1A5) on dark blue bg
 *   - Commands: Bright white (#F2F2F2)
 *   - Parameters/args: Light gray (#CCCCCC)
 *   - Operators: Bright cyan (#61D6D6)
 *   - Strings: Dark yellow (#C19C00)
 *   - Numbers: Bright magenta (#B4009E)
 */

/** ANSI default foreground color (PS default: light gray). */
#define P4_CONFIG_ANSI_DEFAULT_FG            0xCCCCCC

/** ANSI default background color (PS default: dark blue). */
#define P4_CONFIG_ANSI_DEFAULT_BG            0x012456

/** ANSI standard colors — Windows 11 PowerShell theme. */
#define P4_CONFIG_ANSI_BLACK                 0x0C0C0C
#define P4_CONFIG_ANSI_RED                   0xC50F1F
#define P4_CONFIG_ANSI_GREEN                 0x13A10E
#define P4_CONFIG_ANSI_YELLOW                0xC19C00
#define P4_CONFIG_ANSI_BLUE                  0x0037DA
#define P4_CONFIG_ANSI_MAGENTA               0x881798
#define P4_CONFIG_ANSI_CYAN                  0x3A96DD
#define P4_CONFIG_ANSI_WHITE                 0xCCCCCC

/** ANSI bright colors — Windows 11 PowerShell bright variants. */
#define P4_CONFIG_ANSI_BRIGHT_BLACK          0x767676
#define P4_CONFIG_ANSI_BRIGHT_RED            0xE74856
#define P4_CONFIG_ANSI_BRIGHT_GREEN          0x16C60C
#define P4_CONFIG_ANSI_BRIGHT_YELLOW         0xF9F1A5
#define P4_CONFIG_ANSI_BRIGHT_BLUE           0x3B78FF
#define P4_CONFIG_ANSI_BRIGHT_MAGENTA        0xB4009E
#define P4_CONFIG_ANSI_BRIGHT_CYAN           0x61D6D6
#define P4_CONFIG_ANSI_BRIGHT_WHITE          0xF2F2F2

/** Maximum bytes for an ANSI-formatted string buffer. */
#define P4_CONFIG_ANSI_BUFFER_BYTES          512

/* ========================================================================
 * POWERSHELL-STYLE PROMPT CONFIGURATION
 * ======================================================================== */

/** PowerShell-style prompt: "PS " prefix in bright white. */
#define P4_CONFIG_PS_PREFIX                  "PS "

/** PowerShell-style prompt path separator. */
#define P4_CONFIG_PS_PATH_SEPARATOR          "\\"

/** PowerShell-style prompt suffix: "> ". */
#define P4_CONFIG_PS_SUFFIX                  "> "

/** Maximum length of the displayed path in the prompt before truncation.
 *  Longer paths show "...\\" prefix with the last components. */
#define P4_CONFIG_PS_PATH_MAX_DISPLAY        48

/** ANSI SGR for the "PS" prefix token. */
#define P4_CONFIG_PS_COLOR_PREFIX            97  /* Bright white */

/** ANSI SGR for the path token. */
#define P4_CONFIG_PS_COLOR_PATH              93  /* Bright yellow */

/** ANSI SGR for the ">" suffix token. */
#define P4_CONFIG_PS_COLOR_SUFFIX            97  /* Bright white */

/** ANSI SGR for error output prefix "ERROR: ". */
#define P4_CONFIG_PS_COLOR_ERROR             91  /* Bright red */

/** ANSI SGR for warning output prefix "WARNING: ". */
#define P4_CONFIG_PS_COLOR_WARNING           93  /* Bright yellow */

/** ANSI SGR for success output prefix "SUCCESS: ". */
#define P4_CONFIG_PS_COLOR_SUCCESS           92  /* Bright green */

/** ANSI SGR for info output prefix. */
#define P4_CONFIG_PS_COLOR_INFO              96  /* Bright cyan */

/** ANSI SGR for string values. */
#define P4_CONFIG_PS_COLOR_STRING            33  /* Yellow */

/** ANSI SGR for numeric values. */
#define P4_CONFIG_PS_COLOR_NUMBER            95  /* Bright magenta */

/** ANSI SGR for path values. */
#define P4_CONFIG_PS_COLOR_PATH_VALUE        36  /* Cyan */

/** ANSI SGR for prompt/input indicators. */
#define P4_CONFIG_PS_COLOR_PROMPT            97  /* Bright white */

/* ========================================================================
 * USB HOST PARAMETERS
 * ======================================================================== */

/** Stack size for the USB Host Library task. */
#define P4_CONFIG_USB_HOST_LIB_TASK_STACK    4096

/** Stack size for the USB event processing task. */
#define P4_CONFIG_USB_EVENT_TASK_STACK       6144

/** Stack size for USB class driver tasks. */
#define P4_CONFIG_USB_DRIVER_TASK_STACK      4096

/** Depth of the USB event queue. */
#define P4_CONFIG_USB_EVENT_QUEUE_DEPTH      16

/**
 * USB host bring-up retries. The HCD root-hub install and the driver task
 * stacks need contiguous internal RAM that the boot-time Wi-Fi/ESP-Hosted
 * burst can transiently exhaust; retrying after a short delay lets the
 * subsystem come up instead of staying dead for the whole boot.
 */
#define P4_CONFIG_USB_INIT_RETRIES           4

/** Delay in milliseconds between USB host bring-up attempts. */
#define P4_CONFIG_USB_INIT_RETRY_DELAY_MS    500

/** Maximum bytes for a USB status text message. */
#define P4_CONFIG_USB_TEXT_BYTES             192

/** Maximum bytes for a USB filesystem path. */
#define P4_CONFIG_USB_PATH_BYTES             320

/** VFS mount point for USB MSC devices. */
#define P4_CONFIG_USB_MSC_BASE_PATH          "/usb0"

/** Maximum directory entries for USB ls. */
#define P4_CONFIG_USB_LIST_LIMIT             128

/** Maximum bytes in a USB HID report. */
#define P4_CONFIG_USB_HID_REPORT_MAX_BYTES   64

/** Number of simultaneous keys in a USB keyboard report. */
#define P4_CONFIG_USB_KEYBOARD_KEYS          6

/* ---- USB CDC-ACM serial (`usb userial`, userial.c + command verbs) ---- */

/** RX ring bytes for the open CDC-ACM serial device. */
#define P4_CONFIG_USERIAL_RING_BYTES         4096

/** Bulk-transfer chunk / TX scratch bytes for the serial device. */
#define P4_CONFIG_USERIAL_CHUNK_BYTES        1024

/** Milliseconds a single CDC write may block before it is reported failed. */
#define P4_CONFIG_USERIAL_OP_TIMEOUT_MS      4000

/** Milliseconds `userial open` waits for a matching device to appear. */
#define P4_CONFIG_USERIAL_OPEN_TIMEOUT_MS    5000

/** Default idle milliseconds for `userial term` before it closes itself. */
#define P4_CONFIG_USERIAL_TERM_IDLE_MS       30000

/* ---- VT100 terminal mode (`usb userial term`, userial_commands.c) ---- */

/** Render `userial term` as a VT100 screen (TUI grid + SGR/cursor/erase)
 *  instead of the legacy sanitized-transcript passthrough. 1 = enabled. */
#define P4_CONFIG_VT100_ENABLE               1

/** Bytes reserved for holding a split CSI sequence across RX reads. */
#define P4_CONFIG_VT100_PENDING_BYTES        64

/* ========================================================================
 * C6 OTA PARAMETERS
 * ======================================================================== */

/** Stack size for the OTA worker task. */
#define P4_CONFIG_C6OTA_TASK_STACK           8192

/** Block size for HTTP downloads during OTA. */
#define P4_CONFIG_C6OTA_HTTP_BLOCK_BYTES     2048

/** Transfer chunk size for ESP-Hosted SDIO OTA. */
#define P4_CONFIG_C6OTA_TRANSFER_CHUNK       1500

/** Progress reporting interval in percent. */
#define P4_CONFIG_C6OTA_PROGRESS_STEP        5

/** Maximum bytes for an OTA source URL. */
#define P4_CONFIG_C6OTA_URL_BYTES            256

/** Keyword that triggers the default OTA source lookup. */
#define P4_CONFIG_C6OTA_DEFAULT_SOURCE       "default"

/** Primary filename for default OTA source on SD card. */
#define P4_CONFIG_C6OTA_DEFAULT_PRIMARY      "esp32c6_hosted_slave.bin"

/** Fallback filename for default OTA source on SD card. */
#define P4_CONFIG_C6OTA_DEFAULT_FALLBACK     "network_adapter.bin"

/** Expected ESP32-C6 chip ID in the app image header. */
#define P4_CONFIG_C6OTA_EXPECTED_CHIP_ID     0x000D

/** Minimum C6 firmware major version for reliable OTA. */
#define P4_CONFIG_C6OTA_MIN_MAJOR            2

/** Minimum C6 firmware minor version for reliable OTA. */
#define P4_CONFIG_C6OTA_MIN_MINOR            9

/** Minimum C6 firmware patch version for reliable OTA. */
#define P4_CONFIG_C6OTA_MIN_PATCH            7

/** FatFs drive letter for SD-based OTA sources. */
#define P4_CONFIG_C6OTA_SD_FATFS_DRIVE       "0:"

/* ========================================================================
 * TASK STACK SIZES
 * ======================================================================== */

/** Stack size for the Wi-Fi initialization background task. */
#define P4_CONFIG_WIFI_INIT_TASK_STACK       12288

/** Stack size for the shell command worker task. */
#define P4_CONFIG_COMMAND_TASK_STACK         32768

/** Depth of the async command queue (pointers only; the worker owns them).
 *  Raised from 4: bursts (pasted lines, scripted drivers, modal chains)
 *  dropped the 5th command with a zero-timeout submit. Raised again to 32
 *  alongside PSRAM-backed request payloads so a transiently-busy worker does
 *  not drop a normal burst without costing internal DMA-capable RAM. */
#define P4_CONFIG_COMMAND_QUEUE_DEPTH        32

/** Bounded wait when the async queue is full (submit runs on the LVGL/UART
 *  tasks, never the worker, so waiting cannot deadlock the pipeline). */
#define P4_CONFIG_COMMAND_QUEUE_SEND_TIMEOUT_MS 500

/** Stack size for the UART/serial console reader task. Sized above the old
 *  12 KB because the task now also runs the streaming `screenshot` capture
 *  (LVGL snapshot) while a modal blocks the worker. The stack is PSRAM-backed,
 *  so this costs no internal RAM. */
#define P4_CONFIG_UART_CONSOLE_TASK_STACK    16384

/**
 * Bounded wait, in milliseconds, for each segment of a UART transcript mirror
 * write. The mirror bypasses the IDF VFS (whose write path drops the whole line
 * when the SOF-based connection monitor reports a false disconnect under load);
 * this bound stops a stalled TX ring from blocking the writer indefinitely.
 */
#define P4_CONFIG_UART_MIRROR_WRITE_TIMEOUT_MS   50

/** Total budget (ms) for one `shell_uart_console_write_bytes()` payload. Used
 *  by the binary `send`/`screenshot` path, which retries partial writes so
 *  transient TX backpressure cannot truncate a multi-megabyte frame; the writer
 *  gives up only after this many ms with no progress (a genuinely unresponsive
 *  host). The transcript mirror does NOT use this bound (see shell.c): retrying
 *  a full TX ring there would starve the console reader. */
#define P4_CONFIG_UART_WRITE_TOTAL_MS            10000

/**
 * Grace period, in milliseconds, after the connection monitor first reports
 * "disconnected" before the mirror gives up. The SOF monitor can falsely report
 * a disconnect for a few ms under load (the host missing a Start-Of-Frame);
 * within this window lines are still written, so a transient flip never drops
 * output. Only a persistent absence (no host attached) stops the mirror.
 */
#define P4_CONFIG_UART_MIRROR_DISCONNECT_GRACE_MS 2000

/** Stack bytes for the LVGL task (created by the BSP display driver).
 *  Raised above the 7168-byte esp_lvgl_port default: a full-screen redraw
 *  (transcript span group, input line, on-screen keyboard, header) recurses
 *  deep enough that the stock stack overflowed into a boot-loop panic. The
 *  modal editor adds row rendering on this same task. */
#define P4_CONFIG_LVGL_TASK_STACK            24576

/** Log tag for networking/Wi-Fi module. */
#define P4_CONFIG_NETWORKING_TAG             "wifi"

/* ========================================================================
 * TASK INTROSPECTION (ps / tasks / top)
 * ========================================================================
 * Read-only FreeRTOS task introspection surfaced by the `ps`, `tasks`, and
 * `top` commands. The snapshot array is heap-allocated (never on the worker
 * stack); this value caps how many tasks are shown so a burst of task
 * creation cannot blow the allocation or flood the transcript. */

/** Maximum number of tasks shown by `ps` / `tasks` / `top`. */
#define P4_CONFIG_TASK_SNAPSHOT_MAX          64

/* ========================================================================
 * SCREENSHOT (screenshot / scr / capture)
 * ========================================================================
 * Captures the current LVGL screen as a BMP image and streams it over the
 * serial console or writes it to the SD card. Uses LVGL's snapshot API to
 * grab a pixel-perfect copy of the active screen. */

/** Four-byte magic prefix of the screenshot BMP frame ("BMPX"). */
#define P4_CONFIG_SCREENSHOT_BMP_MAGIC       "BMPX"

/*
 * SERIAL FILE TRANSFER (receive / send)
 * Host<->device binary transfer over the USB-Serial/JTAG console.
 */

/** Bytes read/written per chunk during a `receive`/`send` transfer. */
#define P4_CONFIG_SERIAL_XFER_CHUNK_BYTES    4096

/** Milliseconds of no incoming data before a `receive` transfer aborts. */
#define P4_CONFIG_SERIAL_XFER_IDLE_TIMEOUT_MS 4000

/** Milliseconds a raw `send`/screenshot write may wait for TX ring space
 * before the transfer is aborted, so a host that stops reading can never
 * hang the command worker. */
#define P4_CONFIG_SERIAL_SEND_TIMEOUT_MS     10000

/** Hard upper bound (bytes) for a single `send` stream, including `send /diag`. */
#define P4_CONFIG_SERIAL_SEND_MAX_BYTES      16777216

/** Hard upper bound (bytes) for a single `receive` payload. Sized for the
 * 17 MB Noto Sans SC board font (streams to SD in 4 KB chunks; the free-
 * space pre-check still guards the card). */
#define P4_CONFIG_SERIAL_RX_MAX_BYTES        25165824

/** Magic marker printed before a `receive` starts streaming. */
#define P4_CONFIG_SERIAL_RX_READY_MARKER     "=== RX READY ==="

/** Magic marker printed after a `receive` completes successfully. */
#define P4_CONFIG_SERIAL_RX_DONE_MARKER      "=== RX DONE ==="

/** Magic marker printed after a `send` completes successfully. */
#define P4_CONFIG_SERIAL_TX_DONE_MARKER      "=== TX DONE ==="

/** Four-byte magic prefix of the `send` framed payload ("SDFX" little-endian). */
#define P4_CONFIG_SERIAL_SEND_MAGIC          "SDFX"

/** Maximum size in bytes of the `send /diag` diagnostic report payload. */
#define P4_CONFIG_SERIAL_DIAG_BYTES          1024

/* ========================================================================
 * PASSWORD FILE ENCRYPTION (`crypt` lock/unlock)
 * AES-256-GCM with a PBKDF2-HMAC-SHA256 key over the user password.
 * Envelope: `P4CRYPT1` magic + salt + nonce + ciphertext + tag. Files
 * stream in chunk-sized pieces through internal (DMA-safe) buffers; the
 * password buffer is zeroed after every run.
 * ======================================================================== */

/** Bytes encrypted per chunk during a `crypt` file run. */
#define P4_CONFIG_CRYPT_CHUNK_BYTES          4096

/** PBKDF2 iterations for the `crypt` file key (costly enough to slow
 *  guessing, cheap enough for one interactive run). */
#define P4_CONFIG_CRYPT_PBKDF2_ITERS         10000

/** Salt bytes mixed into the `crypt` key derivation. */
#define P4_CONFIG_CRYPT_SALT_BYTES           16

/** Maximum password characters accepted by `crypt` (plus the terminator). */
#define P4_CONFIG_CRYPT_PASS_BYTES           128

/* ========================================================================
 * DATABASE (`db`) — Palm-OS-style SD-backed record store
 * All database data lives on the SD card under sd:/DBS/<name>.DB/:
 *   HEADER.INI, CATEGORIES.INI, INDEX.TXT, RECORDS/R<id>.DAT
 * Every operation uses a guarded SD session, a free-space pre-check, and an
 * atomic temp+rename write. Buffers on the batch path are heap-allocated.
 */

/** Maximum number of named databases that can exist on the card. */
#define P4_CONFIG_DB_MAX_DATABASES            64

/** Maximum records (soft-deleted included) in a single database. */
#define P4_CONFIG_DB_MAX_RECORDS              256

/** Maximum payload bytes for a single record (text or binary). */
#define P4_CONFIG_DB_RECORD_MAX_BYTES         4096

/** Maximum results returned by a `db find` scan (bounded, linear). */
#define P4_CONFIG_DB_FIND_MAX                 64

/** Maximum characters in a database name (the directory part of the path). */
#define P4_CONFIG_DB_NAME_BYTES               32

/** Maximum characters in a record key. */
#define P4_CONFIG_DB_KEY_BYTES                32

/** Maximum length of one `INDEX.TXT` line ("id cat flags key size"). */
#define P4_CONFIG_DB_INDEX_LINE_BYTES         128

/** Number of categories (0..P4_CONFIG_DB_CATEGORY_COUNT-1). */
#define P4_CONFIG_DB_CATEGORY_COUNT           16

/** Maximum characters in the stored creator/type id strings. */
#define P4_CONFIG_DB_ID_BYTES                 8

/** Maximum lines in one `db export` / `db import` text file. */
#define P4_CONFIG_DB_EXPORT_MAX_RECORDS       256

/** Maximum bytes in one `db export` / `db import` text file. */
#define P4_CONFIG_DB_EXPORT_MAX_BYTES         (256 * 1024)

/** Maximum bytes of one `k=v` field value read by `db find /field:` and
 *  `db get /field:` (values are truncated to this for compare/sort/print). */
#define P4_CONFIG_DB_FIELD_VALUE_BYTES        64

/** Flag bit: the record's payload is secret and redacted in list/transcript
 *  output unless an explicit reveal is requested. */
#define P4_CONFIG_DB_FLAG_SECRET              0x01

/** Flag bit: the record is soft-deleted (kept until `purge`). */
#define P4_CONFIG_DB_FLAG_DELETED             0x02

/* ========================================================================
 * ALARM / CALENDAR (`alarm`, `cal`)
 * A small SD-persisted event store + a single background checker task. All
 * event data lives on the SD card under P4_CONFIG_ALARM_PATH (default
 * sd:/ALARMS): INDEX.INI + one E<id>.INI per event. The checker posts to the
 * existing header notification, LED, and audio surfaces (never a private
 * loop) and optionally queues a `/run:` batch file onto the command worker
 * via a registered host-ops hook.
 */

/** Maximum events stored at once (also bounds the checker's per-poll scan). */
#define P4_CONFIG_ALARM_MAX_EVENTS            64

/** Maximum characters in an alarm title. */
#define P4_CONFIG_ALARM_TITLE_BYTES           48

/** Maximum characters in an alarm message (header notification width). */
#define P4_CONFIG_ALARM_MSG_BYTES             160

/** Checker poll interval in milliseconds (fires within this granularity). */
#define P4_CONFIG_ALARM_POLL_MS               30000

/** Stack size for the background alarm checker task. Sized generously: the
 *  fire pass carries a per-event struct + path locals and newlib's snprintf
 *  (used to render the event files and notifications) alone needs ~1.5 KB of
 *  stack frame. 8192 matches the audio playback task. */
#define P4_CONFIG_ALARM_TASK_STACK            8192

/** Priority of the alarm checker task. */
#define P4_CONFIG_ALARM_TASK_PRIORITY         1

/** Base directory for the alarm store (resolved at init). */
#define P4_CONFIG_ALARM_PATH                  "sd:/ALARMS"

/** Fire once on boot for alarms that became due while powered off. */
#define P4_CONFIG_ALARM_CATCHUP_ON_BOOT       1

/** Default header-notification timeout in seconds when an alarm fires. */
#define P4_CONFIG_ALARM_DEFAULT_NOTIFY_SECS   5

/** Compile out the `/run:` batch action entirely for size (0 = keep it). */
#define P4_CONFIG_ALARM_ENABLE_RUN_ACTION     1

#endif /* P4MINISHELL_CONFIG_H */
