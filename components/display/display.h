/**
 * @file display.h
 * @brief Display manager for P4MiniShell — rotation, resolution, refresh rate, backlight.
 *
 * Central display controller that owns all display hardware state and operations.
 * All display-related shell commands (brightness, rotate, resolution, refresh rate)
 * go through this module. Provides a stable public API for the shell and other
 * components that need display information.
 *
 * Features:
 *   - Rotation control (0/90/180/270) with automatic touch remapping
 *   - Resolution query (native and current rotated)
 *   - Refresh rate control (via DSI lane bitrate adjustment)
 *   - Backlight brightness control (PWM via BSP)
 *   - Display sleep/wake (low-power transitions)
 *   - Display info query (timing, buffer config, color format)
 *   - Thread-safe state tracking (atomic writes, LVGL async dispatch for UI rebuilds)
 *
 * Architecture:
 *   This module OWNS the display state. The shell layer (main.c) calls into this
 *   module for all display operations. The module internally uses BSP display
 *   functions and LVGL APIs. It does NOT own LVGL widgets or UI layout — that
 *   remains the shell's responsibility.
 *
 * Backward compatibility:
 *   - All existing shell commands (brightness, rotate) continue to work
 *   - Internal functions in main.c are replaced with calls to this module
 *   - Board config values (BOARD_CFG_*) remain the hardware source of truth
 *   - p4minishell_config.h values (P4_CONFIG_*) remain the tunable source of truth
 */

#ifndef P4MINISHELL_DISPLAY_H
#define P4MINISHELL_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * DISPLAY ROTATION
 * ======================================================================== */

/** Display rotation angles supported by the manager. */
typedef enum {
    DISPLAY_ROTATION_0   = 0,    /**< 0 degrees (native/landscape) */
    DISPLAY_ROTATION_90  = 90,   /**< 90 degrees clockwise (portrait) */
    DISPLAY_ROTATION_180 = 180,  /**< 180 degrees (inverted landscape) */
    DISPLAY_ROTATION_270 = 270,  /**< 270 degrees clockwise (portrait inverted) */
} display_rotation_t;

/** Convert a display_rotation_t to LVGL's lv_display_rotation_t. */
lv_display_rotation_t display_rotation_to_lvgl(display_rotation_t rotation);

/** Convert an LVGL rotation to our display_rotation_t. */
display_rotation_t display_rotation_from_lvgl(lv_display_rotation_t lvgl_rotation);

/** Parse a rotation string ("0", "90", "180", "270") into display_rotation_t.
 *  Returns ESP_OK on success, ESP_ERR_INVALID_ARG on invalid input. */
esp_err_t display_rotation_parse(const char *str, display_rotation_t *rotation_out);

/** Get the current display rotation. */
display_rotation_t display_get_rotation(void);

/**
 * Apply a new display rotation.
 * This triggers LVGL software rotation, touch controller remapping,
 * and schedules a full UI rebuild via lv_async_call.
 *
 * @param rotation  New rotation angle.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if display not initialized.
 */
esp_err_t display_set_rotation(display_rotation_t rotation);

/** Get the rotation as a human-readable string. */
const char *display_rotation_to_string(display_rotation_t rotation);

/* ========================================================================
 * DISPLAY RESOLUTION
 * ======================================================================== */

/** Resolution info for current display state. */
typedef struct {
    int32_t native_width;       /**< Native panel width (BOARD_CFG_LCD_WIDTH) */
    int32_t native_height;      /**< Native panel height (BOARD_CFG_LCD_HEIGHT) */
    int32_t current_width;      /**< Current effective width (after rotation) */
    int32_t current_height;     /**< Current effective height (after rotation) */
} display_resolution_t;

/** Get the current display resolution, accounting for rotation. */
display_resolution_t display_get_resolution(void);

/** Get the native (unrotated) panel resolution. */
display_resolution_t display_get_native_resolution(void);

/** Convenience: get current display width (after rotation). */
int32_t display_get_width(void);

/** Convenience: get current display height (after rotation). */
int32_t display_get_height(void);

/* ========================================================================
 * DISPLAY REFRESH RATE
 * ======================================================================== */

/** Refresh rate configuration. */
typedef struct {
    uint32_t target_hz;             /**< Target refresh rate in Hz */
    uint32_t current_hz;            /**< Current estimated refresh rate */
    uint32_t pixel_clock_mhz;       /**< Current pixel clock in MHz */
    uint32_t dsi_lane_bitrate_mbps; /**< Current DSI lane bitrate in Mbps */
} display_refresh_config_t;

/** Get the current refresh rate configuration. */
display_refresh_config_t display_get_refresh_config(void);

/**
 * Set the target refresh rate by adjusting DSI lane bitrate.
 * The actual achievable rate depends on panel timing constraints.
 * Supported values: 30, 60 Hz (others may be achievable depending on panel).
 *
 * @param target_hz  Desired refresh rate in Hz.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if unsupported,
 *         ESP_ERR_NOT_SUPPORTED if hardware doesn't support dynamic rate change.
 */
esp_err_t display_set_refresh_rate(uint32_t target_hz);

/* ========================================================================
 * BACKLIGHT / BRIGHTNESS
 * ======================================================================== */

/** Get the current backlight brightness percentage (0-100). */
int display_get_brightness(void);

/**
 * Set the backlight brightness.
 * @param percent  0-100 brightness percentage.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range,
 *         ESP_FAIL if BSP call fails.
 */
esp_err_t display_set_brightness(int percent);

/* ========================================================================
 * DISPLAY POWER MANAGEMENT
 * ======================================================================== */

/** Display power state. */
typedef enum {
    DISPLAY_POWER_ON = 0,       /**< Display fully on */
    DISPLAY_POWER_SLEEP,        /**< Display in sleep/low-power mode */
    DISPLAY_POWER_OFF,          /**< Display fully off */
} display_power_state_t;

/** Get the current display power state. */
display_power_state_t display_get_power_state(void);

/**
 * Set the display power state.
 * - DISPLAY_POWER_ON: Backlight on, display active
 * - DISPLAY_POWER_SLEEP: Backlight off, display in low-power mode
 * - DISPLAY_POWER_OFF: Backlight off, display powered down
 *
 * @return ESP_OK on success.
 */
esp_err_t display_set_power_state(display_power_state_t state);

/** Convenience: put display to sleep (backlight off, low power). */
esp_err_t display_sleep(void);

/** Convenience: wake display from sleep (backlight on). */
esp_err_t display_wake(void);

/* ========================================================================
 * DISPLAY INFO & DIAGNOSTICS
 * ======================================================================== */

/** Comprehensive display info for sysinfo/diag output. */
typedef struct {
    display_resolution_t resolution;
    display_rotation_t rotation;
    display_refresh_config_t refresh;
    display_power_state_t power_state;
    int brightness_percent;
    const char *panel_driver;       /**< Panel driver name (e.g., "JD9165") */
    const char *touch_driver;       /**< Touch driver name (e.g., "GT911") */
    uint32_t draw_buffer_size;      /**< LVGL draw buffer size in bytes */
    bool double_buffer;             /**< Whether double buffering is enabled */
    bool buffer_dma;                /**< Whether DMA is used for buffer */
    bool buffer_spiram;             /**< Whether PSRAM is used for buffer */
    bool sw_rotate;                 /**< Whether software rotation is active */
    uint32_t hsync;                 /**< Horizontal sync */
    uint32_t hbp;                   /**< Horizontal back porch */
    uint32_t hfp;                   /**< Horizontal front porch */
    uint32_t vsync;                 /**< Vertical sync */
    uint32_t vbp;                   /**< Vertical back porch */
    uint32_t vfp;                   /**< Vertical front porch */
    uint8_t mipi_lane_num;          /**< Number of MIPI DSI lanes */
} display_info_t;

/** Get comprehensive display information. */
display_info_t display_get_info(void);

/** Print display info to the shell transcript (formatted for sysinfo). */
void display_print_info(void (*print_fn)(const char *format, ...));

/* ========================================================================
 * INITIALIZATION & LIFECYCLE
 * ======================================================================== */

/**
 * Initialize the display manager and the physical display hardware.
 * Must be called once during boot, before any other display operations.
 *
 * This wraps bsp_display_start_with_config() and stores the LVGL display
 * handle for all subsequent operations.
 *
 * @return ESP_OK on success, ESP_FAIL if display init fails.
 */
esp_err_t display_init(void);

/**
 * Deinitialize the display manager.
 * Does NOT power off the display hardware — use display_set_power_state()
 * for that. This just resets internal state tracking.
 */
void display_deinit(void);

/**
 * Check if the display manager has been initialized.
 * @return true if display_init() completed successfully.
 */
bool display_is_initialized(void);

/**
 * Get the LVGL display handle for direct LVGL operations.
 * @return The lv_display_t pointer, or NULL if not initialized.
 */
lv_display_t *display_get_lvgl_handle(void);

/**
 * Get the touch handle for direct touch operations.
 * @return The esp_lcd_touch_handle_t, or NULL if not available.
 */
void *display_get_touch_handle(void);

/**
 * Register a callback to be invoked when the UI needs rebuilding after
 * rotation or resolution changes. The display manager calls this via
 * lv_async_call on the LVGL task.
 *
 * @param rebuild_fn  Function to call for full UI rebuild. Pass NULL to clear.
 */
void display_register_ui_rebuild_callback(void (*rebuild_fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_DISPLAY_H */
