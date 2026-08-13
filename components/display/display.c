/**
 * @file display.c
 * @brief Display manager implementation for P4MiniShell.
 *
 * Central display controller owning all display hardware state and operations.
 * Manages rotation, resolution queries, refresh rate, backlight brightness,
 * power state, and display diagnostics. All display-related shell commands
 * route through this module.
 *
 * Thread safety:
 *   - State variables are protected by atomic writes (bool/int on this platform)
 *   - LVGL operations are dispatched via lv_async_call when called from non-LVGL tasks
 *   - Touch handle acquisition is lazy and cached
 *   - UI rebuild is scheduled via registered callback, not performed here
 */

#include "display.h"
#include "board_config.h"
#include "p4minishell_config.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

/* Backward-compatibility aliases from p4minishell_config.h */
#define DISPLAY_TAG                     P4_CONFIG_SHELL_TAG

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

/** Display manager internal state. All fields protected by atomic access pattern. */
static struct {
    bool initialized;
    lv_display_t *lvgl_display;
    void *touch_handle;                         /* esp_lcd_touch_handle_t */
    display_rotation_t rotation;
    int brightness_percent;
    display_power_state_t power_state;
    void (*ui_rebuild_callback)(void);          /* Called via lv_async_call after rotation */
    portMUX_TYPE lock;                          /* Spinlock for multi-task access */
} s_display = {
    .initialized = false,
    .lvgl_display = NULL,
    .touch_handle = NULL,
    .rotation = DISPLAY_ROTATION_0,
    .brightness_percent = 100,
    .power_state = DISPLAY_POWER_ON,
    .ui_rebuild_callback = NULL,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static esp_err_t display_acquire_touch_handle(void);
static void display_update_touch_rotation(display_rotation_t rotation);
static void display_async_rebuild_ui(void *user_data);

/* ========================================================================
 * ROTATION CONVERSION HELPERS
 * ======================================================================== */

lv_display_rotation_t display_rotation_to_lvgl(display_rotation_t rotation)
{
    switch (rotation) {
    case DISPLAY_ROTATION_0:   return LV_DISPLAY_ROTATION_0;
    case DISPLAY_ROTATION_90:  return LV_DISPLAY_ROTATION_90;
    case DISPLAY_ROTATION_180: return LV_DISPLAY_ROTATION_180;
    case DISPLAY_ROTATION_270: return LV_DISPLAY_ROTATION_270;
    default:                   return LV_DISPLAY_ROTATION_0;
    }
}

display_rotation_t display_rotation_from_lvgl(lv_display_rotation_t lvgl_rotation)
{
    switch (lvgl_rotation) {
    case LV_DISPLAY_ROTATION_0:   return DISPLAY_ROTATION_0;
    case LV_DISPLAY_ROTATION_90:  return DISPLAY_ROTATION_90;
    case LV_DISPLAY_ROTATION_180: return DISPLAY_ROTATION_180;
    case LV_DISPLAY_ROTATION_270: return DISPLAY_ROTATION_270;
    default:                      return DISPLAY_ROTATION_0;
    }
}

esp_err_t display_rotation_parse(const char *str, display_rotation_t *rotation_out)
{
    if (str == NULL || rotation_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(str, "0") == 0) {
        *rotation_out = DISPLAY_ROTATION_0;
    } else if (strcmp(str, "90") == 0) {
        *rotation_out = DISPLAY_ROTATION_90;
    } else if (strcmp(str, "180") == 0) {
        *rotation_out = DISPLAY_ROTATION_180;
    } else if (strcmp(str, "270") == 0) {
        *rotation_out = DISPLAY_ROTATION_270;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

display_rotation_t display_get_rotation(void)
{
    display_rotation_t rotation;
    portENTER_CRITICAL(&s_display.lock);
    rotation = s_display.rotation;
    portEXIT_CRITICAL(&s_display.lock);
    return rotation;
}

const char *display_rotation_to_string(display_rotation_t rotation)
{
    switch (rotation) {
    case DISPLAY_ROTATION_0:   return "0";
    case DISPLAY_ROTATION_90:  return "90";
    case DISPLAY_ROTATION_180: return "180";
    case DISPLAY_ROTATION_270: return "270";
    default:                   return "0";
    }
}

/* ========================================================================
 * ROTATION APPLICATION
 * ======================================================================== */

static esp_err_t display_acquire_touch_handle(void)
{
    lv_indev_t *touch_indev;

    if (s_display.touch_handle != NULL) {
        return ESP_OK;
    }

    if (s_display.lvgl_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    touch_indev = bsp_display_get_input_dev();
    if (touch_indev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    /* The LVGL port stores the esp_lcd_touch_handle_t in the indev driver data.
     * We use a known struct layout from the esp_lvgl_port component. */
    typedef struct {
        esp_lcd_touch_handle_t handle;
        lv_indev_t *indev;
        struct { float x; float y; } scale;
    } touch_ctx_t;

    touch_ctx_t *touch_ctx = (touch_ctx_t *)lv_indev_get_driver_data(touch_indev);
    if (touch_ctx == NULL || touch_ctx->handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_display.touch_handle = touch_ctx->handle;
    return ESP_OK;
}

static void display_update_touch_rotation(display_rotation_t rotation)
{
    /* When LVGL software rotation is enabled (sw_rotate=true), LVGL handles
     * touch coordinate transformation automatically based on the display
     * rotation. The touch controller must stay in native panel orientation.
     * Manually remapping the touch controller would double-transform
     * coordinates, causing mirroring and incorrect rotation.
     *
     * When sw_rotate is false and the panel supports hardware rotation,
     * the touch controller would need manual remapping to match. But the
     * JD9165 panel does not support hardware rotation, so we always use
     * software rotation. */
    (void)rotation;

    if (display_acquire_touch_handle() != ESP_OK || s_display.touch_handle == NULL) {
        return;
    }

    /* Always keep touch in native orientation — LVGL handles the transform */
    (void)esp_lcd_touch_set_swap_xy((esp_lcd_touch_handle_t)s_display.touch_handle, false);
    (void)esp_lcd_touch_set_mirror_x((esp_lcd_touch_handle_t)s_display.touch_handle, false);
    (void)esp_lcd_touch_set_mirror_y((esp_lcd_touch_handle_t)s_display.touch_handle, false);
}

static void display_async_rebuild_ui(void *user_data)
{
    (void)user_data;

    portENTER_CRITICAL(&s_display.lock);
    void (*cb)(void) = s_display.ui_rebuild_callback;
    portEXIT_CRITICAL(&s_display.lock);

    if (cb != NULL) {
        cb();
    }
}

esp_err_t display_set_rotation(display_rotation_t rotation)
{
    lv_display_t *disp;
    display_rotation_t current;

    portENTER_CRITICAL(&s_display.lock);
    disp = s_display.lvgl_display;
    current = s_display.rotation;
    portEXIT_CRITICAL(&s_display.lock);

    if (disp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* No-op if rotation hasn't changed */
    if (rotation == current) {
        return ESP_OK;
    }

    /* Apply LVGL software rotation.
     * With sw_rotate=true in the LVGL port config, the flush callback
     * uses lv_draw_sw_rotate() to rotate pixel data before sending to
     * the panel. LVGL also handles touch coordinate transformation
     * automatically based on the display rotation.
     *
     * The lv_display_set_rotation() call triggers a resolution change
     * event which causes the LVGL port to call lvgl_port_disp_rotation_update()
     * to update its internal rotation tracking. */
    lv_display_set_rotation(disp, display_rotation_to_lvgl(rotation));

    /* Keep touch controller in native orientation — LVGL handles the
     * coordinate transformation automatically when sw_rotate is enabled. */
    display_update_touch_rotation(rotation);

    /* Track the current rotation atomically */
    portENTER_CRITICAL(&s_display.lock);
    s_display.rotation = rotation;
    portEXIT_CRITICAL(&s_display.lock);

    /* Schedule UI rebuild via lv_async_call so it runs on the LVGL task
     * with adequate stack, avoiding stack overflow when called from
     * the UART console task or other small-stack contexts. */
    lv_async_call(display_async_rebuild_ui, NULL);

    return ESP_OK;
}

/* ========================================================================
 * RESOLUTION QUERIES
 * ======================================================================== */

display_resolution_t display_get_native_resolution(void)
{
    display_resolution_t res = {
        .native_width = BOARD_CFG_LCD_WIDTH,
        .native_height = BOARD_CFG_LCD_HEIGHT,
        .current_width = BOARD_CFG_LCD_WIDTH,
        .current_height = BOARD_CFG_LCD_HEIGHT,
    };
    return res;
}

display_resolution_t display_get_resolution(void)
{
    display_resolution_t res;
    display_rotation_t rotation;

    portENTER_CRITICAL(&s_display.lock);
    rotation = s_display.rotation;
    portEXIT_CRITICAL(&s_display.lock);

    res.native_width = BOARD_CFG_LCD_WIDTH;
    res.native_height = BOARD_CFG_LCD_HEIGHT;

    /* For 90/270 degree rotations, width and height are swapped */
    if (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270) {
        res.current_width = BOARD_CFG_LCD_HEIGHT;
        res.current_height = BOARD_CFG_LCD_WIDTH;
    } else {
        res.current_width = BOARD_CFG_LCD_WIDTH;
        res.current_height = BOARD_CFG_LCD_HEIGHT;
    }

    return res;
}

int32_t display_get_width(void)
{
    display_resolution_t res = display_get_resolution();
    return res.current_width;
}

int32_t display_get_height(void)
{
    display_resolution_t res = display_get_resolution();
    return res.current_height;
}

/* ========================================================================
 * REFRESH RATE CONTROL
 * ======================================================================== */

display_refresh_config_t display_get_refresh_config(void)
{
    display_refresh_config_t config = {
        .target_hz = 60,
        .current_hz = 60,
        .pixel_clock_mhz = BOARD_CFG_LCD_PIXEL_CLOCK_MHZ,
        .dsi_lane_bitrate_mbps = BOARD_CFG_LCD_DSI_BUS_LANE_BITRATE_MBPS_RUNTIME,
    };

    /* Estimate current refresh rate from timing parameters:
     * Total pixels per frame = hsync * vsync
     * Pixel clock = pixel_clock_mhz * 1,000,000
     * Refresh rate = pixel_clock / total_pixels_per_frame */
    if (config.pixel_clock_mhz > 0 && BOARD_CFG_LCD_HSYNC > 0 && BOARD_CFG_LCD_VSYNC > 0) {
        uint64_t total_pixels = (uint64_t)BOARD_CFG_LCD_HSYNC * BOARD_CFG_LCD_VSYNC;
        uint64_t pixel_clock_hz = (uint64_t)config.pixel_clock_mhz * 1000000ULL;
        if (total_pixels > 0) {
            config.current_hz = (uint32_t)(pixel_clock_hz / total_pixels);
        }
    }

    config.target_hz = config.current_hz;
    return config;
}

esp_err_t display_set_refresh_rate(uint32_t target_hz)
{
    /* Dynamic refresh rate change via DSI bitrate adjustment is not
     * supported on the current JD9165 panel baseline. The panel timing
     * is fixed at 80 MHz pixel clock with the current hsync/vsync values.
     *
     * Future panels or board revisions may support this through:
     *   - Reconfiguring the MIPI DSI PHY bitrate
     *   - Adjusting panel timing registers via MIPI DCS commands
     *   - Using VRR (Variable Refresh Rate) capable panels
     *
     * For now, we validate the target and report the limitation. */
    if (target_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    display_refresh_config_t current = display_get_refresh_config();

    if (target_hz == current.current_hz) {
        /* Already at the requested rate */
        return ESP_OK;
    }

    /* The JD9165 panel is fixed at ~60 Hz with current timing.
     * 30 Hz could theoretically be achieved by doubling vertical blanking
     * but that requires panel-specific DCS commands not yet implemented. */
    ESP_LOGW(DISPLAY_TAG, "Dynamic refresh rate change not supported on current panel");
    ESP_LOGW(DISPLAY_TAG, "Current rate: %" PRIu32 " Hz, requested: %" PRIu32 " Hz",
             current.current_hz, target_hz);

    return ESP_ERR_NOT_SUPPORTED;
}

/* ========================================================================
 * BACKLIGHT / BRIGHTNESS
 * ======================================================================== */

int display_get_brightness(void)
{
    int brightness;
    portENTER_CRITICAL(&s_display.lock);
    brightness = s_display.brightness_percent;
    portEXIT_CRITICAL(&s_display.lock);
    return brightness;
}

esp_err_t display_set_brightness(int percent)
{
    esp_err_t error;

    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    error = bsp_display_brightness_set(percent);
    if (error != ESP_OK) {
        ESP_LOGE(DISPLAY_TAG, "Failed to set brightness to %d%%: %s",
                 percent, esp_err_to_name(error));
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_display.lock);
    s_display.brightness_percent = percent;
    portEXIT_CRITICAL(&s_display.lock);

    return ESP_OK;
}

/* ========================================================================
 * POWER MANAGEMENT
 * ======================================================================== */

display_power_state_t display_get_power_state(void)
{
    display_power_state_t state;
    portENTER_CRITICAL(&s_display.lock);
    state = s_display.power_state;
    portEXIT_CRITICAL(&s_display.lock);
    return state;
}

esp_err_t display_set_power_state(display_power_state_t state)
{
    display_power_state_t current;

    portENTER_CRITICAL(&s_display.lock);
    current = s_display.power_state;
    portEXIT_CRITICAL(&s_display.lock);

    if (state == current) {
        return ESP_OK;
    }

    switch (state) {
    case DISPLAY_POWER_ON:
        bsp_display_backlight_on();
        break;
    case DISPLAY_POWER_SLEEP:
    case DISPLAY_POWER_OFF:
        bsp_display_backlight_off();
        break;
    }

    portENTER_CRITICAL(&s_display.lock);
    s_display.power_state = state;
    portEXIT_CRITICAL(&s_display.lock);

    return ESP_OK;
}

esp_err_t display_sleep(void)
{
    return display_set_power_state(DISPLAY_POWER_SLEEP);
}

esp_err_t display_wake(void)
{
    return display_set_power_state(DISPLAY_POWER_ON);
}

/* ========================================================================
 * DISPLAY INFO & DIAGNOSTICS
 * ======================================================================== */

display_info_t display_get_info(void)
{
    display_info_t info;
    memset(&info, 0, sizeof(info));

    info.resolution = display_get_resolution();
    info.rotation = display_get_rotation();
    info.refresh = display_get_refresh_config();
    info.power_state = display_get_power_state();
    info.brightness_percent = display_get_brightness();

    info.panel_driver = P4_CONFIG_DISPLAY_PANEL_DRIVER;
    info.touch_driver = P4_CONFIG_DISPLAY_TOUCH_DRIVER;

    info.draw_buffer_size = BOARD_CFG_LCD_DRAW_BUFFER_SIZE;
    info.double_buffer = (BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE != 0);
    info.buffer_dma = (BOARD_CFG_APP_BUFFER_DMA != 0);
    info.buffer_spiram = (BOARD_CFG_APP_BUFFER_SPIRAM != 0);
    info.sw_rotate = (BOARD_CFG_APP_SW_ROTATE != 0);

    info.hsync = BOARD_CFG_LCD_HSYNC;
    info.hbp = BOARD_CFG_LCD_HBP;
    info.hfp = BOARD_CFG_LCD_HFP;
    info.vsync = BOARD_CFG_LCD_VSYNC;
    info.vbp = BOARD_CFG_LCD_VBP;
    info.vfp = BOARD_CFG_LCD_VFP;
    info.mipi_lane_num = BOARD_CFG_LCD_MIPI_DSI_LANE_NUM;

    return info;
}

void display_print_info(void (*print_fn)(const char *format, ...))
{
    display_info_t info;

    if (print_fn == NULL) {
        return;
    }

    info = display_get_info();

    print_fn("display: %" PRId32 " x %" PRId32 " (native %" PRId32 " x %" PRId32
             "), %s, reset GPIO %d, backlight GPIO %d\n",
             info.resolution.current_width,
             info.resolution.current_height,
             info.resolution.native_width,
             info.resolution.native_height,
             info.panel_driver,
             BOARD_CFG_LCD_RST_GPIO,
             BOARD_CFG_LCD_BACKLIGHT_GPIO);

    print_fn("display.state: brightness=%d%% rotation=%s power=%s refresh=%" PRIu32 "Hz\n",
             info.brightness_percent,
             display_rotation_to_string(info.rotation),
             info.power_state == DISPLAY_POWER_ON ? "on" :
             info.power_state == DISPLAY_POWER_SLEEP ? "sleep" : "off",
             info.refresh.current_hz);

    print_fn("display.timing: pclk=%" PRIu32 "MHz, lanes=%d, bitrate=%" PRIu32
             "Mbps, hsync=%" PRIu32 " hbp=%" PRIu32 " hfp=%" PRIu32
             " vsync=%" PRIu32 " vbp=%" PRIu32 " vfp=%" PRIu32 "\n",
             info.refresh.pixel_clock_mhz,
             info.mipi_lane_num,
             info.refresh.dsi_lane_bitrate_mbps,
             info.hsync, info.hbp, info.hfp,
             info.vsync, info.vbp, info.vfp);

    print_fn("display.buffer: draw=%" PRIu32 ", double=%d, dma=%d, spiram=%d, sw_rotate=%d\n",
             info.draw_buffer_size,
             info.double_buffer ? 1 : 0,
             info.buffer_dma ? 1 : 0,
             info.buffer_spiram ? 1 : 0,
             info.sw_rotate ? 1 : 0);

    print_fn("display.touch: %s on I2C%d, SDA GPIO %d, SCL GPIO %d, %dHz, pullup=%d\n",
             info.touch_driver,
             BOARD_CFG_I2C_PORT,
             BOARD_CFG_I2C_SDA_GPIO,
             BOARD_CFG_I2C_SCL_GPIO,
             BOARD_CFG_I2C_CLK_SPEED_HZ,
             BOARD_CFG_I2C_ENABLE_INTERNAL_PULLUP);
}

/* ========================================================================
 * INITIALIZATION & LIFECYCLE
 * ======================================================================== */

esp_err_t display_init(void)
{
    lv_display_t *display;

    if (s_display.initialized) {
        ESP_LOGW(DISPLAY_TAG, "Display manager already initialized");
        return ESP_OK;
    }

    /* Build the BSP display configuration from board_config.h values.
     * Start from the port defaults (task_priority, task_affinity,
     * task_max_sleep_ms, timer_period_ms, task_stack_caps all included) and
     * only raise the LVGL task stack above the 7168-byte default: a
     * full-screen redraw (transcript span group, input line, keyboard,
     * header) recurses deep enough to overflow the stock stack. */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BOARD_CFG_LCD_DRAW_BUFFER_SIZE,
        .double_buffer = BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE,
        .flags = {
            .buff_dma = BOARD_CFG_APP_BUFFER_DMA,
            .buff_spiram = BOARD_CFG_APP_BUFFER_SPIRAM,
            .sw_rotate = BOARD_CFG_APP_SW_ROTATE,
        }
    };
    cfg.lvgl_port_cfg.task_stack = P4_CONFIG_LVGL_TASK_STACK;

    display = bsp_display_start_with_config(&cfg);
    if (display == NULL) {
        ESP_LOGE(DISPLAY_TAG, "Display initialization failed");
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_display.lock);
    s_display.lvgl_display = display;
    s_display.initialized = true;
    s_display.rotation = DISPLAY_ROTATION_0;
    s_display.brightness_percent = P4_CONFIG_DISPLAY_DEFAULT_BRIGHTNESS;
    s_display.power_state = DISPLAY_POWER_ON;
    portEXIT_CRITICAL(&s_display.lock);

    /* Turn on the backlight by default */
    bsp_display_backlight_on();

    /* Attempt to acquire touch handle (non-fatal if it fails) */
    esp_err_t touch_err = display_acquire_touch_handle();
    if (touch_err != ESP_OK) {
        ESP_LOGW(DISPLAY_TAG, "GT911 touch handle lookup failed: %s; "
                 "rotation will be display-only", esp_err_to_name(touch_err));
    }

    ESP_LOGI(DISPLAY_TAG, "Display manager initialized: %" PRId32 "x%" PRId32
             " %s panel, %s touch",
             BOARD_CFG_LCD_WIDTH, BOARD_CFG_LCD_HEIGHT,
             P4_CONFIG_DISPLAY_PANEL_DRIVER,
             touch_err == ESP_OK ? P4_CONFIG_DISPLAY_TOUCH_DRIVER " ready"
                                : P4_CONFIG_DISPLAY_TOUCH_DRIVER " unavailable");

    return ESP_OK;
}

void display_deinit(void)
{
    portENTER_CRITICAL(&s_display.lock);
    s_display.initialized = false;
    s_display.lvgl_display = NULL;
    s_display.touch_handle = NULL;
    s_display.rotation = DISPLAY_ROTATION_0;
    s_display.brightness_percent = P4_CONFIG_DISPLAY_DEFAULT_BRIGHTNESS;
    s_display.power_state = DISPLAY_POWER_ON;
    s_display.ui_rebuild_callback = NULL;
    portEXIT_CRITICAL(&s_display.lock);
}

bool display_is_initialized(void)
{
    bool initialized;
    portENTER_CRITICAL(&s_display.lock);
    initialized = s_display.initialized;
    portEXIT_CRITICAL(&s_display.lock);
    return initialized;
}

lv_display_t *display_get_lvgl_handle(void)
{
    lv_display_t *handle;
    portENTER_CRITICAL(&s_display.lock);
    handle = s_display.lvgl_display;
    portEXIT_CRITICAL(&s_display.lock);
    return handle;
}

void *display_get_touch_handle(void)
{
    void *handle;
    portENTER_CRITICAL(&s_display.lock);
    handle = s_display.touch_handle;
    portEXIT_CRITICAL(&s_display.lock);
    return handle;
}

void display_register_ui_rebuild_callback(void (*rebuild_fn)(void))
{
    portENTER_CRITICAL(&s_display.lock);
    s_display.ui_rebuild_callback = rebuild_fn;
    portEXIT_CRITICAL(&s_display.lock);
}
