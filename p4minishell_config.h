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

/** Board name requested by the user (may differ from detected BSP name). */
#define P4_CONFIG_BOARD_REQUESTED            "JC1060P470C"

/** Board name detected from the BSP component. */
#define P4_CONFIG_BOARD_DETECTED             "ESP32-P4-Function-EV-Board"

/**
 * P4MiniShell semantic version.
 * Update these when the project version changes in changelog.md.
 * The boot message and all version commands read from these macros.
 */
#define P4_CONFIG_VERSION_MAJOR             0
#define P4_CONFIG_VERSION_MINOR             32
#define P4_CONFIG_VERSION_PATCH             0

/** Full version string assembled from the components above. */
#define P4_CONFIG_VERSION_STRING             "v" STR(P4_CONFIG_VERSION_MAJOR) "." STR(P4_CONFIG_VERSION_MINOR) "." STR(P4_CONFIG_VERSION_PATCH)

/** Helper for stringification. */
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/** Product display name. */
#define P4_CONFIG_PRODUCT_NAME               "P4MiniShell"

/** Proprietary copyright/notice surfaced by `about` and the license notice. */
#define P4_CONFIG_COPYRIGHT_NOTICE           "Copyright (c) 2026 P4MiniShell. Proprietary - all rights reserved."

/** Boot banner displayed in the transcript on startup. */
#define P4_CONFIG_BOOT_MESSAGE               P4_CONFIG_PRODUCT_NAME " " P4_CONFIG_VERSION_STRING " ready | " P4_CONFIG_BOARD_REQUESTED " | type help"

/** Shell prompt string shown on the input line and serial console.
 *  The LVGL input line uses a plain-text version; the UART console
 *  renders with ANSI color codes for PowerShell-style coloring.
 *  Format: "PS " (bright white) + path (bright yellow) + "> " (bright white) */
#define P4_CONFIG_SHELL_PROMPT               "PS " P4_CONFIG_PS_PATH_SEPARATOR "> "

/* ========================================================================
 * TRANSCRIPT AND COMMAND BUFFER SIZING
 * ======================================================================== */

/** Maximum bytes stored in the on-screen transcript buffer.
 *  Sized so the scrollable transcript holds a useful history (~30 lines of
 *  coloured output) instead of only the last screenful; the LVGL span group
 *  shows ~15 lines and scrolls through the rest. The buffer is allocated from
 *  PSRAM (with an internal-RAM fallback) so it does not compete with the
 *  DMA-capable heap used by the WiFi/SDIO transport mempool and the
 *  USB-Serial/JTAG ring buffers. */
#define P4_CONFIG_TRANSCRIPT_BYTES           16384

/**
 * Maximum bytes staged for the on-screen transcript span group. The staged
 * copy is the raw ANSI transcript, so it only needs to hold the same size as
 * the transcript itself (no recolor inflation: the span-group renders SGR
 * escapes directly instead of recolor markup).
 */
#define P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES   P4_CONFIG_TRANSCRIPT_BYTES

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

/** Maximum bytes in the async (background task) transcript staging buffer. */
#define P4_CONFIG_ASYNC_TRANSCRIPT_BYTES     2048

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

/** Maximum matches reported by Tab completion before truncation. */
#define P4_CONFIG_COMPLETION_MAX_MATCHES     32

/** Maximum bytes of a file the `edit` editor will load into RAM. */
#define P4_CONFIG_EDITOR_MAX_BYTES           (64 * 1024)

/** Maximum lines the `edit` editor will load into RAM. */
#define P4_CONFIG_EDITOR_MAX_LINES           2048

/** Undo/redo depth kept by the `edit` editor (edit operations). */
#define P4_CONFIG_EDITOR_UNDO_DEPTH          64

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

/** Blink period for the `edit` editor's block cursor, in milliseconds. */
#define P4_CONFIG_EDITOR_CURSOR_BLINK_MS     500

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
#define P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS 4

/** True when the `edit` editor highlights the cursor's current line. */
#define P4_CONFIG_EDITOR_CURRENT_LINE         1

/** RGB colour of the `edit` editor's current-line highlight background. */
#define P4_CONFIG_EDITOR_CURRENT_LINE_COLOR   0x16222A

/** Number of entries in the debug/error log ring buffer. */
#define P4_CONFIG_DEBUG_LOG_DEPTH            5

/** Maximum bytes per debug log entry. */
#define P4_CONFIG_DEBUG_ENTRY_BYTES          192

/* ========================================================================
 * UI LAYOUT
 * ======================================================================== */

/** Height of the on-screen LVGL keyboard in pixels. */
#define P4_CONFIG_KEYBOARD_HEIGHT            240

/** Period for the header status refresh timer in milliseconds. */
#define P4_CONFIG_HEADER_REFRESH_PERIOD_MS   5000

/** Height of the input row (prompt line) in pixels. */
#define P4_CONFIG_INPUT_ROW_HEIGHT           52

/* ========================================================================
 * DISPLAY MANAGER PARAMETERS
 * ======================================================================== */

/** Default backlight brightness percentage on boot (0-100). */
#define P4_CONFIG_DISPLAY_DEFAULT_BRIGHTNESS 100

/** Default display rotation on boot (0, 90, 180, or 270). */
#define P4_CONFIG_DISPLAY_DEFAULT_ROTATION   0

/** Default display power state on boot: 0=on, 1=sleep, 2=off. */
#define P4_CONFIG_DISPLAY_DEFAULT_POWER      0

/** Whether dynamic refresh rate changes are supported by the panel. */
#define P4_CONFIG_DISPLAY_REFRESH_DYNAMIC    0

/** Display panel driver name for diagnostics. */
#define P4_CONFIG_DISPLAY_PANEL_DRIVER       "JD9165"

/** Touch controller driver name for diagnostics. */
#define P4_CONFIG_DISPLAY_TOUCH_DRIVER       "GT911"

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

/** Keyboard height as percentage of display height. */
#define P4_CONFIG_WINDOW_KEYBOARD_HEIGHT_PCT 35

/** Keyboard height clamp minimum in pixels. */
#define P4_CONFIG_WINDOW_KEYBOARD_HEIGHT_MIN 180

/** Keyboard height clamp maximum in pixels. */
#define P4_CONFIG_WINDOW_KEYBOARD_HEIGHT_MAX 280

/** Minimum transcript height in pixels. */
#define P4_CONFIG_WINDOW_TRANSCRIPT_HEIGHT_MIN 40

/** Horizontal margin in pixels added to the screen root so no shell region
 *  (header, transcript, input row, keyboard) ever touches the display edges. */
#define P4_CONFIG_WINDOW_SCREEN_PAD_HOR       8

/** Width of the transcript scroll buttons (input-row Up/Dn) in pixels. */
#define P4_CONFIG_WINDOW_SCROLL_BUTTON_WIDTH  64

/* ========================================================================
 * KEYBOARD PARAMETERS
 * ======================================================================== */

/** Default keyboard visibility on boot: 0=hidden, 1=visible. */
#define P4_CONFIG_KEYBOARD_DEFAULT_VISIBLE    1

/** Keyboard height as percentage of display height. */
#define P4_CONFIG_KEYBOARD_HEIGHT_PCT         35

/** Keyboard height clamp minimum in pixels. */
#define P4_CONFIG_KEYBOARD_HEIGHT_MIN         180

/** Keyboard height clamp maximum in pixels. */
#define P4_CONFIG_KEYBOARD_HEIGHT_MAX         280

/** Keyboard log tag. */
#define P4_CONFIG_KEYBOARD_TAG                "keyboard"

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

/** Default SSID from sdkconfig (empty if not configured). */
#ifndef CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID
#define P4_CONFIG_WIFI_DEFAULT_SSID          ""
#else
#define P4_CONFIG_WIFI_DEFAULT_SSID          CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID
#endif

/** Default password from sdkconfig (empty if not configured). */
#ifndef CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD
#define P4_CONFIG_WIFI_DEFAULT_PASSWORD      ""
#else
#define P4_CONFIG_WIFI_DEFAULT_PASSWORD      CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD
#endif

/** Runtime guard: true when any Wi-Fi path is enabled in sdkconfig. */
#define P4_CONFIG_WIFI_RUNTIME_ENABLED \
     (CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_HOST_WIFI_ENABLED || CONFIG_ESP_HOSTED)

/** Skip ESP-Hosted version compatibility gate. When 1, the Wi-Fi init path
 *  does not reject C6 firmware version mismatches. Useful for development
 *  and testing with mismatched host/co-processor firmware. Set to 0 in
 *  production to enforce the version gate. */
#define P4_CONFIG_HOSTED_SKIP_VERSION_GATE  0

/** Maximum access points surfaced by `wifi scan`. Scan results are sorted by
 *  RSSI (strongest first) and capped at this value, so a busy channel cannot
 *  flood the transcript. */
#define P4_CONFIG_WIFI_SCAN_LIMIT           32

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
 *  path-sized scratch (URL + LFN) so it needs headroom over the default. */
#define P4_CONFIG_HTTPD_STACK_BYTES         16384

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

/** Hard compile gate: 0 = disabled, 1 = enabled (legacy Bluedroid path). */
#define P4_CONFIG_BT_HOSTED_RUNTIME_SUPPORTED 0

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
 *  disk clean/delete, recursive delete, trash empty/purge). */
#define P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD   "YES"

/** Backward-compatible alias: `format` uses the shared destructive word. */
#define P4_CONFIG_FORMAT_CONFIRM_WORD        P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD

/** Default allocation unit size in bytes requested when formatting. 0 = let
 *  FATFS (via esp_vfs_fat_sdcard_format_cfg) choose a size-appropriate value. */
#define P4_CONFIG_FORMAT_ALLOC_UNIT_BYTES    0

/** Smallest `/A:` allocation unit (cluster) size accepted by `format`. */
#define P4_CONFIG_FORMAT_ALLOC_UNIT_MIN      4096

/** Largest `/A:` allocation unit (cluster) size accepted by `format`. */
#define P4_CONFIG_FORMAT_ALLOC_UNIT_MAX      (128 * 1024)

/** MBR partition-table alignment for `disk create partition` (in 512-byte
 *  sectors). 2048 sectors = 1 MiB, the diskpart/SD standard alignment. */
#define P4_CONFIG_DISK_PARTITION_ALIGN_SECTORS 2048

/** Maximum number of `disk` / `format` volume targets. The firmware currently
 *  supports the SD card only; USB OTG MSC is a future target. */
#define P4_CONFIG_STORAGE_VOLUME_MAX         1

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

/** Exact confirmation word required by `trash empty` and `trash purge`. */
#define P4_CONFIG_TRASH_CONFIRM_WORD         P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD

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

/** Maximum number of GPIO directives CONFIG.SYS may contain. */
#define P4_CONFIG_BOOT_MAX_GPIO_LINES        16

/** Maximum bytes the `config` command reads/writes for the CONFIG.SYS file. */
#define P4_CONFIG_CONFIG_MAX_BYTES           16384

/* ========================================================================
 * BATCH ENGINE AND ENVIRONMENT VARIABLES
 * ======================================================================== */

/** Maximum number of RAM-only environment variables. */
#define P4_CONFIG_ENV_VAR_MAX                24

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

/** Maximum bytes for an environment variable name. */
#define P4_CONFIG_ENV_NAME_BYTES             32

/** Maximum bytes for an environment variable value. */
#define P4_CONFIG_ENV_VALUE_BYTES            256

/** Maximum bytes for a single batch file line. */
#define P4_CONFIG_BATCH_LINE_BYTES           384

/** Maximum batch script arguments (%1 through %9). */
#define P4_CONFIG_BATCH_ARGS_MAX             9

/** Maximum nested batch file call depth. */
#define P4_CONFIG_BATCH_DEPTH_MAX            4

/** I/O buffer size for general file operations. */
#define P4_CONFIG_FILE_IO_BUFFER_BYTES       512

/** Maximum `:label` targets tracked per batch file for goto/call. */
#define P4_CONFIG_BATCH_LABEL_MAX            32

/**
 * Maximum bytes for a single `:label` name.
 * A label is one identifier, so this is far smaller than a command line.
 * The label table is sized LABEL_MAX * LABEL_BYTES, so keeping this tight
 * matters for the batch frame footprint.
 */
#define P4_CONFIG_BATCH_LABEL_BYTES          48

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
 * math and string functions (ABS, SIN, COS, TAN, ASN, ACS, ATN, HYP, SQR,
 * EXP, LN, LOG, FACT, NCR, NPR, INT, FIX, FRAC, ROUND, SGN, MOD, PI, RAN#,
 * POL, REC, DMS/DMS$, VAL/VALF, STR$, HEX$, ASC, CHR$, LEN, LEFT$, MID$,
 * RIGHT$, `&H`/`0x` hex literals) and an ANGLE degree/radian mode. It lives
 * in components/batch (calc.c) and is a batch language verb like `set`.
 */

/** Maximum bytes of a string result or string argument in a `calc` expression. */
#define P4_CONFIG_CALC_STR_BYTES             32

/** Maximum parenthesis / function nesting depth in a `calc` expression. */
#define P4_CONFIG_CALC_MAX_DEPTH             16

/** Trig angle mode at boot: 1 = degrees (calculator default), 0 = radians. */
#define P4_CONFIG_CALC_ANGLE_DEFAULT_DEG     1

/** Significant digits used when printing a `calc` numeric result. */
#define P4_CONFIG_CALC_PRINT_PRECISION       12

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

/** Maximum bytes for a GPIO pin name string. */
#define P4_CONFIG_GPIO_NAME_BYTES            32

/** Maximum exposed GPIO pins in the board pin table. */
#define P4_CONFIG_GPIO_PIN_LIMIT             24

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

/** Header battery low threshold (percent). */
#define P4_CONFIG_HEADER_BAT_LOW_PCT         15

/** Display duration for transient header notifications in milliseconds. */
#define P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS   3000

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

/** ANSI SGR for command names in output. */
#define P4_CONFIG_PS_COLOR_COMMAND           97  /* Bright white */

/** ANSI SGR for parameter/flag names. */
#define P4_CONFIG_PS_COLOR_PARAMETER         37  /* White/gray */

/** ANSI SGR for string values. */
#define P4_CONFIG_PS_COLOR_STRING            33  /* Yellow */

/** ANSI SGR for numeric values. */
#define P4_CONFIG_PS_COLOR_NUMBER            95  /* Bright magenta */

/** ANSI SGR for path values. */
#define P4_CONFIG_PS_COLOR_PATH_VALUE        36  /* Cyan */

/** ANSI SGR for subsystem/module labels (e.g., "[wifi]", "[usb]"). */
#define P4_CONFIG_PS_COLOR_SUBSYSTEM         96  /* Bright cyan */

/** ANSI SGR for key names / property labels. */
#define P4_CONFIG_PS_COLOR_KEY               37  /* White */

/** ANSI SGR for muted/secondary text. */
#define P4_CONFIG_PS_COLOR_MUTED             90  /* Bright black (gray) */

/** ANSI SGR for heading/title text. */
#define P4_CONFIG_PS_COLOR_HEADING           93  /* Bright yellow */

/** ANSI SGR for IP addresses. */
#define P4_CONFIG_PS_COLOR_IP                95  /* Bright magenta */

/** ANSI SGR for connected/active status. */
#define P4_CONFIG_PS_COLOR_CONNECTED         92  /* Bright green */

/** ANSI SGR for disconnected/inactive status. */
#define P4_CONFIG_PS_COLOR_DISCONNECTED      90  /* Bright black (gray) */

/** ANSI SGR for progress/step messages. */
#define P4_CONFIG_PS_COLOR_PROGRESS          96  /* Bright cyan */

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

/** Auto-detect USB keyboard: automatically hide on-screen keyboard when USB keyboard is attached. */
#define P4_CONFIG_USB_KEYBOARD_AUTO_DETECT   1

/** USB keyboard input injection: route USB keystrokes to shell CLI input line. */
#define P4_CONFIG_USB_KEYBOARD_CLI_INJECT    1

/** USB keyboard notification timeout in milliseconds. */
#define P4_CONFIG_USB_KEYBOARD_NOTIFY_MS     3000

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
#define P4_CONFIG_COMMAND_TASK_STACK         12288

/** Stack size for the UART/serial console reader task. */
#define P4_CONFIG_UART_CONSOLE_TASK_STACK    12288

/** Stack bytes for the LVGL task (created by the BSP display driver).
 *  Raised above the 7168-byte esp_lvgl_port default: a full-screen redraw
 *  (transcript span group, input line, on-screen keyboard, header) recurses
 *  deep enough that the stock stack overflowed into a boot-loop panic. The
 *  modal editor adds row rendering on this same task, so the budget stays a
 *  generous 12 KB. */
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

/** Display width for BMP header (1024 pixels). */
#define P4_CONFIG_SCREENSHOT_WIDTH           1024

/** Display height for BMP header (600 pixels). */
#define P4_CONFIG_SCREENSHOT_HEIGHT          600

/** Color format for LVGL snapshot (RGB565 = 16-bit, 2 bytes per pixel). */
#define P4_CONFIG_SCREENSHOT_COLOR_FORMAT    0x12  /* LV_COLOR_FORMAT_RGB565 */

/** Magic marker sent before the BMP binary data on serial output. */
#define P4_CONFIG_SCREENSHOT_BMP_BEGIN       "=== SCREENSHOT BMP BEGIN ==="

/** Magic marker sent after the BMP binary data on serial output. */
#define P4_CONFIG_SCREENSHOT_BMP_END         "=== SCREENSHOT BMP END ==="

/** Four-byte magic prefix of the screenshot BMP frame ("BMPX"). */
#define P4_CONFIG_SCREENSHOT_BMP_MAGIC       "BMPX"

/** Maximum bytes per write chunk when streaming BMP to UART. */
#define P4_CONFIG_SCREENSHOT_UART_CHUNK      512

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

/** Hard upper bound (bytes) for a single `receive` payload. */
#define P4_CONFIG_SERIAL_RX_MAX_BYTES        8388608

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

#endif /* P4MINISHELL_CONFIG_H */
