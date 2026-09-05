/**
 * @file led.c
 * @brief RGB status LED (WS2812 on GPIO26) driver and status/notification engine.
 *
 * The strip is created once through the espressif/led_strip component (RMT
 * backend) and all strip I/O happens on a dedicated animation task, so the
 * command worker, the Wi-Fi event handler, and main() only touch a mutex-
 * protected state snapshot. Effects (rainbow / breath / pulse / blink) are
 * synthesised from the tick time; transient event notifications override the
 * persistent colour for P4_CONFIG_LED_NOTIFY_MS and then fall back to it.
 */

#include <string.h>
#include <strings.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/rmt_types.h"
#include "led_strip.h"

#include "led.h"
#include "p4minishell_config.h"

#define LED_TAG                "led"

#define LED_GPIO               P4_CONFIG_LED_GPIO
#define LED_RMT_RESOLUTION_HZ  P4_CONFIG_LED_RMT_RESOLUTION_HZ
#define LED_RMT_SYMBOLS        P4_CONFIG_LED_RMT_SYMBOLS
#define LED_BRIGHTNESS_PCT     P4_CONFIG_LED_MAX_BRIGHTNESS_PCT
#define LED_TASK_STACK         P4_CONFIG_LED_TASK_STACK_BYTES
#define LED_TICK_MS            P4_CONFIG_LED_TICK_MS
#define LED_NOTIFY_MS          P4_CONFIG_LED_NOTIFY_MS
#define LED_BOOT_FLASH_MS      P4_CONFIG_LED_BOOT_FLASH_MS
#define LED_EFFECT_SPEED       P4_CONFIG_LED_EFFECT_SPEED_DEFAULT
#define LED_AUTO_STATUS        P4_CONFIG_LED_AUTO_STATUS
#define LED_COLOR_BOOT_OK           P4_CONFIG_LED_COLOR_BOOT_OK
#define LED_COLOR_WIFI_CONNECTING   P4_CONFIG_LED_COLOR_WIFI_CONNECTING
#define LED_COLOR_WIFI_CONNECTED    P4_CONFIG_LED_COLOR_WIFI_CONNECTED
#define LED_COLOR_WIFI_DISCONNECTED P4_CONFIG_LED_COLOR_WIFI_DISCONNECTED
#define LED_COLOR_WIFI_ERROR        P4_CONFIG_LED_COLOR_WIFI_ERROR
#define LED_COLOR_HTTPD             P4_CONFIG_LED_COLOR_HTTPD
#define LED_COLOR_ALARM             P4_CONFIG_LED_COLOR_ALARM

/** One persistent frame: a base colour plus the effect applied to it. */
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    led_effect_t effect;
} led_frame_t;

/** A transient notification override with an expiry timestamp. */
typedef struct {
    bool active;
    led_frame_t frame;
    uint64_t started_us;
    uint32_t duration_ms;
} led_notification_t;

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static bool s_initialized;
static bool s_auto_status;
static uint8_t s_speed;
static led_frame_t s_status_frame;   /* persistent auto status colour */
static led_frame_t s_manual_frame;   /* persistent manual colour/effect */
static led_notification_t s_notification;

static void led_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void led_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/** Split a packed 0xRRGGBB value into components. */
static void led_unpack(uint32_t rgb, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    *red = (uint8_t)((rgb >> 16) & 0xFF);
    *green = (uint8_t)((rgb >> 8) & 0xFF);
    *blue = (uint8_t)(rgb & 0xFF);
}

/** Map an event to its colour and effect. */
static void led_event_frame(led_event_t event, led_frame_t *frame)
{
    switch (event) {
    case LED_EVENT_BOOT_OK:
        led_unpack(LED_COLOR_BOOT_OK, &frame->red, &frame->green, &frame->blue);
        frame->effect = LED_EFFECT_SOLID;
        break;
    case LED_EVENT_WIFI_CONNECTING:
        led_unpack(LED_COLOR_WIFI_CONNECTING, &frame->red, &frame->green, &frame->blue);
        frame->effect = LED_EFFECT_PULSE;
        break;
    case LED_EVENT_WIFI_CONNECTED:
        led_unpack(LED_COLOR_WIFI_CONNECTED, &frame->red, &frame->green, &frame->blue);
        frame->effect = LED_EFFECT_SOLID;
        break;
    case LED_EVENT_WIFI_DISCONNECTED:
        led_unpack(LED_COLOR_WIFI_DISCONNECTED, &frame->red, &frame->green, &frame->blue);
        frame->effect = LED_EFFECT_BLINK;
        break;
    case LED_EVENT_WIFI_ERROR:
        led_unpack(LED_COLOR_WIFI_ERROR, &frame->red, &frame->green, &frame->blue);
        frame->effect = LED_EFFECT_PULSE;
        break;
    case LED_EVENT_HTTPD_STARTED:
        led_unpack(LED_COLOR_HTTPD, &frame->red, &frame->green, &frame->blue);
        frame->effect = LED_EFFECT_PULSE;
        break;
    case LED_EVENT_ALARM:
        led_unpack(LED_COLOR_ALARM, &frame->red, &frame->green, &frame->blue);
        frame->effect = LED_EFFECT_PULSE;
        break;
    default:
        frame->red = frame->green = frame->blue = 0;
        frame->effect = LED_EFFECT_SOLID;
        break;
    }
}

/** Wi-Fi state events are persistent in auto mode; everything else is a
 *  transient notification. */
static bool led_event_is_status(led_event_t event)
{
    return event == LED_EVENT_WIFI_CONNECTING ||
           event == LED_EVENT_WIFI_CONNECTED ||
           event == LED_EVENT_WIFI_DISCONNECTED;
}

/** Standard HSV to RGB (hue 0-359, sat/val 0-255). */
static void led_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value,
                           uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint8_t region;
    uint8_t remainder;
    uint8_t p;
    uint8_t q;
    uint8_t t;

    if (saturation == 0) {
        *red = *green = *blue = value;
        return;
    }
    region = (uint8_t)(hue / 60);
    remainder = (uint8_t)((hue % 60) * 255 / 60);
    p = (uint8_t)((uint16_t)value * (255 - saturation) / 255);
    q = (uint8_t)((uint16_t)value * (255 - ((uint16_t)saturation * remainder / 255)) / 255);
    t = (uint8_t)((uint16_t)value * (255 - ((uint16_t)saturation * (255 - remainder) / 255)) / 255);

    switch (region) {
    case 0: *red = value; *green = t; *blue = p; break;
    case 1: *red = q; *green = value; *blue = p; break;
    case 2: *red = p; *green = value; *blue = t; break;
    case 3: *red = p; *green = q; *blue = value; break;
    case 4: *red = t; *green = p; *blue = value; break;
    default: *red = value; *green = p; *blue = q; break;
    }
}

/** Compute the colour of a frame at the given time, before brightness scaling. */
static void led_render_frame(const led_frame_t *frame, uint64_t now_us,
                             uint8_t speed, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint32_t now_ms = (uint32_t)(now_us / 1000);
    uint8_t rr = frame->red;
    uint8_t gg = frame->green;
    uint8_t bb = frame->blue;

    if (speed < 1) {
        speed = 1;
    }

    switch (frame->effect) {
    case LED_EFFECT_RAINBOW: {
        uint32_t period_ms = 3000 / speed;
        uint16_t hue = (uint16_t)((now_ms % period_ms) * 360 / period_ms);
        led_hsv_to_rgb(hue, 255, 255, &rr, &gg, &bb);
        break;
    }
    case LED_EFFECT_BREATH: {
        uint32_t period_ms = 2000 / speed;
        uint32_t phase = now_ms % period_ms;
        uint32_t k = (phase < period_ms / 2)
                         ? (phase * 255 * 2 / period_ms)
                         : ((period_ms - phase) * 255 * 2 / period_ms);
        rr = (uint8_t)((uint16_t)rr * k / 255);
        gg = (uint8_t)((uint16_t)gg * k / 255);
        bb = (uint8_t)((uint16_t)bb * k / 255);
        break;
    }
    case LED_EFFECT_PULSE:
    case LED_EFFECT_BLINK: {
        uint32_t period_ms = (frame->effect == LED_EFFECT_PULSE ? 500 : 1000) / speed;
        bool on = (now_ms % period_ms) < (period_ms / 2);
        if (!on) {
            rr = gg = bb = 0;
        }
        break;
    }
    default:
        break; /* LED_EFFECT_SOLID */
    }

    *red = rr;
    *green = gg;
    *blue = bb;
}

/** Animation task: render one frame per tick. */
static void led_task(void *arg)
{
    (void)arg;

    while (1) {
        led_frame_t frame;
        bool use_notification;
        uint64_t now_us;
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        led_lock();
        if (!s_initialized) {
            led_unlock();
            vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
            continue;
        }

        now_us = esp_timer_get_time();
        use_notification = s_notification.active &&
                           (now_us - s_notification.started_us) < (uint64_t)s_notification.duration_ms * 1000;
        if (use_notification) {
            frame = s_notification.frame;
        } else {
            frame = s_auto_status ? s_status_frame : s_manual_frame;
            s_notification.active = false;
        }

        led_render_frame(&frame, now_us, s_speed, &red, &green, &blue);

        red = (uint8_t)((uint16_t)red * LED_BRIGHTNESS_PCT / 100);
        green = (uint8_t)((uint16_t)green * LED_BRIGHTNESS_PCT / 100);
        blue = (uint8_t)((uint16_t)blue * LED_BRIGHTNESS_PCT / 100);

        if (s_strip != NULL) {
            led_strip_set_pixel(s_strip, 0, red, green, blue);
            led_strip_refresh(s_strip);
        }
        led_unlock();

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

void led_init(void)
{
    esp_err_t error;
    led_strip_handle_t strip = NULL;

    if (s_initialized) {
        return;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = LED_RMT_SYMBOLS,
        .flags.with_dma = false,
    };

    error = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (error != ESP_OK) {
        ESP_LOGE(LED_TAG, "WS2812 init failed on GPIO%d (%s)", LED_GPIO, esp_err_to_name(error));
        return;
    }

    led_lock();
    s_strip = strip;
    s_auto_status = (LED_AUTO_STATUS != 0);
    s_speed = LED_EFFECT_SPEED;
    /* Start in the Wi-Fi connecting state; events update it from here. */
    led_event_frame(LED_EVENT_WIFI_CONNECTING, &s_status_frame);
    memset(&s_manual_frame, 0, sizeof(s_manual_frame));
    s_manual_frame.effect = LED_EFFECT_SOLID;
    s_notification.active = false;
    led_strip_clear(strip);
    s_initialized = true;
    led_unlock();

    if (xTaskCreate(led_task, "led", LED_TASK_STACK, NULL, tskIDLE_PRIORITY + 2, &s_task) != pdPASS) {
        ESP_LOGE(LED_TAG, "failed to create LED animation task");
    }
}

bool led_is_initialized(void)
{
    return s_initialized;
}

esp_err_t led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    led_lock();
    s_manual_frame.red = red;
    s_manual_frame.green = green;
    s_manual_frame.blue = blue;
    s_manual_frame.effect = LED_EFFECT_SOLID;
    s_auto_status = false;
    s_notification.active = false;
    led_unlock();
    return ESP_OK;
}

esp_err_t led_set_hex(uint32_t rgb)
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    led_unpack(rgb, &red, &green, &blue);
    return led_set_color(red, green, blue);
}

esp_err_t led_off(void)
{
    return led_set_color(0, 0, 0);
}

esp_err_t led_set_effect(led_effect_t effect, uint8_t speed)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (speed == 0) {
        speed = LED_EFFECT_SPEED;
    }
    if (speed > 10) {
        speed = 10;
    }
    led_lock();
    s_manual_frame.effect = effect;
    s_speed = speed;
    s_auto_status = false;
    s_notification.active = false;
    led_unlock();
    return ESP_OK;
}

esp_err_t led_set_auto_status(bool enable)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    led_lock();
    s_auto_status = enable;
    if (enable) {
        /* Keep the current status frame; events will refresh it. */
    } else {
        s_notification.active = false;
    }
    led_unlock();
    return ESP_OK;
}

void led_get_state(led_state_t *out)
{
    const led_frame_t *frame;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    led_lock();
    out->initialized = s_initialized;
    if (!s_initialized) {
        led_unlock();
        return;
    }
    out->auto_status = s_auto_status;
    frame = s_auto_status ? &s_status_frame : &s_manual_frame;
    out->red = frame->red;
    out->green = frame->green;
    out->blue = frame->blue;
    out->effect = frame->effect;
    out->speed = s_speed;
    out->brightness_pct = LED_BRIGHTNESS_PCT;
    led_unlock();
}

void led_notify(led_event_t event)
{
    if (!s_initialized || event == LED_EVENT_NONE) {
        return;
    }
    led_lock();
    if (s_auto_status && led_event_is_status(event)) {
        led_event_frame(event, &s_status_frame);
        s_notification.active = false;
    } else {
        led_event_frame(event, &s_notification.frame);
        s_notification.started_us = esp_timer_get_time();
        s_notification.duration_ms = (event == LED_EVENT_BOOT_OK) ? LED_BOOT_FLASH_MS : LED_NOTIFY_MS;
        s_notification.active = true;
    }
    led_unlock();
}

led_effect_t led_effect_from_name(const char *name)
{
    if (name == NULL) {
        return LED_EFFECT_SOLID;
    }
    if (strcasecmp(name, "rainbow") == 0) {
        return LED_EFFECT_RAINBOW;
    }
    if (strcasecmp(name, "breath") == 0) {
        return LED_EFFECT_BREATH;
    }
    if (strcasecmp(name, "pulse") == 0) {
        return LED_EFFECT_PULSE;
    }
    if (strcasecmp(name, "blink") == 0) {
        return LED_EFFECT_BLINK;
    }
    return LED_EFFECT_SOLID;
}

const char *led_effect_name(led_effect_t effect)
{
    switch (effect) {
    case LED_EFFECT_RAINBOW: return "rainbow";
    case LED_EFFECT_BREATH:  return "breath";
    case LED_EFFECT_PULSE:   return "pulse";
    case LED_EFFECT_BLINK:   return "blink";
    default:                 return "solid";
    }
}
