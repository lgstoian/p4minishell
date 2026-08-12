#ifndef P4MINISHELL_LED_H
#define P4MINISHELL_LED_H

/**
 * @file led.h
 * @brief RGB status LED (WS2812 on GPIO26) driver and status/notification engine.
 *
 * The Guition JC1060P470 board carries a WS2812 (NeoPixel-style) addressable
 * RGB LED on the back panel, wired to GPIO26. This module owns the strip
 * (created with the espressif/led_strip component over RMT) and a small
 * animation task that renders the current colour, effect, or transient event
 * notification.
 *
 * Consumers push state and events; the module never queries other subsystems,
 * so it is a leaf that the shell command module, the networking module, and
 * main() can all depend on without a layering cycle.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Transient event colours pushed by the owning subsystems. */
typedef enum {
    LED_EVENT_NONE = 0,          /**< No event (clear any override). */
    LED_EVENT_BOOT_OK,           /**< Green confirmation flash after boot scripting. */
    LED_EVENT_WIFI_CONNECTING,   /**< Amber pulse while connecting. */
    LED_EVENT_WIFI_CONNECTED,    /**< Green steady; becomes the status colour in auto mode. */
    LED_EVENT_WIFI_DISCONNECTED, /**< Red blink; becomes the status colour in auto mode. */
    LED_EVENT_WIFI_ERROR,        /**< Red pulse (transient). */
    LED_EVENT_HTTPD_STARTED,     /**< Blue pulse when the HTTP server starts. */
    LED_EVENT_HTTPD_STOPPED,     /**< Transient cue back to the status colour. */
} led_event_t;

/** Persistent rendering modes. */
typedef enum {
    LED_EFFECT_SOLID = 0,        /**< Steady colour. */
    LED_EFFECT_RAINBOW,          /**< Hue sweep through the spectrum. */
    LED_EFFECT_BREATH,           /**< Smooth fade in/out of the base colour. */
    LED_EFFECT_PULSE,            /**< Fast square-wave blink of the base colour. */
    LED_EFFECT_BLINK,            /**< Square-wave blink of the base colour. */
} led_effect_t;

/** Snapshot of the LED state for `rgb status`. */
typedef struct {
    bool initialized;        /**< Strip driver created successfully. */
    bool auto_status;        /**< Colour follows system status events. */
    led_effect_t effect;     /**< Currently selected effect. */
    uint8_t red;             /**< Solid / base colour component (0-255). */
    uint8_t green;           /**< Solid / base colour component (0-255). */
    uint8_t blue;            /**< Solid / base colour component (0-255). */
    uint8_t speed;           /**< Effect speed, 1 (slow) .. 10 (fast). */
    uint8_t brightness_pct;  /**< Applied colour scale (0-100). */
} led_state_t;

/**
 * Initialize the WS2812 strip and the animation task. Idempotent; safe to
 * call more than once. On failure the module stays uninitialized and every
 * API below becomes a no-op returning ESP_ERR_INVALID_STATE.
 */
void led_init(void);

/** Report whether the strip driver was created successfully. */
bool led_is_initialized(void);

/**
 * Set a solid colour and switch off the auto status layer.
 * @param red   0-255
 * @param green 0-255
 * @param blue  0-255
 */
esp_err_t led_set_color(uint8_t red, uint8_t green, uint8_t blue);

/** Set a solid colour from a packed 0xRRGGBB value. */
esp_err_t led_set_hex(uint32_t rgb);

/** Turn the LED off (solid black). */
esp_err_t led_off(void);

/**
 * Select an effect and switch off the auto status layer.
 * @param effect  One of LED_EFFECT_*.
 * @param speed   1 (slow) .. 10 (fast); 0 selects the configured default.
 */
esp_err_t led_set_effect(led_effect_t effect, uint8_t speed);

/**
 * Enable or disable the auto status layer. When enabled, Wi-Fi state events
 * drive the persistent colour (amber while connecting, green when connected,
 * red blink when disconnected); when disabled the LED shows the last manual
 * colour/effect and events are transient.
 */
esp_err_t led_set_auto_status(bool enable);

/** Copy the current state into @p out (NULL-safe). */
void led_get_state(led_state_t *out);

/**
 * Push a transient event colour. In auto mode, Wi-Fi state events update the
 * persistent status colour; every other event (boot OK, HTTP server, errors)
 * flashes for P4_CONFIG_LED_NOTIFY_MS and then returns to the status colour.
 */
void led_notify(led_event_t event);

/** Map an effect name ("rainbow", "breath", "pulse", "blink") to an enum.
 *  Unknown names resolve to LED_EFFECT_SOLID. */
led_effect_t led_effect_from_name(const char *name);

/** Map an effect enum to its command-line name. */
const char *led_effect_name(led_effect_t effect);

#endif /* P4MINISHELL_LED_H */
