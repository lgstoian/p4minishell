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

/** Boot banner displayed in the transcript on startup. */
#define P4_CONFIG_BOOT_MESSAGE               "P4MiniShell v0.2 ready | JC1060P470C | type help"

/** Shell prompt string shown on the input line and serial console. */
#define P4_CONFIG_SHELL_PROMPT               "P4Shell> "

/* ========================================================================
 * TRANSCRIPT AND COMMAND BUFFER SIZING
 * ======================================================================== */

/** Maximum bytes stored in the on-screen transcript buffer. */
#define P4_CONFIG_TRANSCRIPT_BYTES           8192

/** Maximum bytes in the async (background task) transcript staging buffer. */
#define P4_CONFIG_ASYNC_TRANSCRIPT_BYTES     2048

/** Maximum bytes for a single command line (input + null terminator). */
#define P4_CONFIG_COMMAND_BYTES              256

/** Number of commands retained in the recall history. */
#define P4_CONFIG_COMMAND_HISTORY_DEPTH      10

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
    (CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_HOST_WIFI_ENABLED || CONFIG_ESP_HOSTED_ENABLED)

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

/* ========================================================================
 * SD CARD AND FILESYSTEM
 * ======================================================================== */

/** FatFs drive letter prefix for SD card access. */
#define P4_CONFIG_SD_FATFS_DRIVE             "0:"

/** Maximum bytes for an SD card path (resolved absolute path). */
#define P4_CONFIG_SD_PATH_BYTES              320

/** Maximum directory entries to list in a single sd ls command. */
#define P4_CONFIG_SD_LIST_LIMIT              128

/** Default preview size in bytes for sd cat. */
#define P4_CONFIG_SD_CAT_DEFAULT_BYTES       1024

/** Maximum preview size in bytes for sd cat. */
#define P4_CONFIG_SD_CAT_MAX_BYTES           8192

/** I/O buffer size for SD file operations. */
#define P4_CONFIG_SD_IO_BUFFER_BYTES         128

/* ========================================================================
 * BATCH ENGINE AND ENVIRONMENT VARIABLES
 * ======================================================================== */

/** Maximum number of RAM-only environment variables. */
#define P4_CONFIG_ENV_VAR_MAX                24

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
#define P4_CONFIG_WIFI_INIT_TASK_STACK       6144

/** Stack size for the shell command worker task. */
#define P4_CONFIG_COMMAND_TASK_STACK         8192

/** Stack size for the UART/serial console reader task. */
#define P4_CONFIG_UART_CONSOLE_TASK_STACK    4096

/** Log tag for networking/Wi-Fi module. */
#define P4_CONFIG_NETWORKING_TAG             "wifi"

#endif /* P4MINISHELL_CONFIG_H */
