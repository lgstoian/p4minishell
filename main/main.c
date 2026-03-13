#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#define SHELL_TAG "p4minishell"
#define SHELL_BOARD_REQUESTED "JC1060P470C"
#define SHELL_BOARD_DETECTED "ESP32-P4-Function-EV-Board"
#define SHELL_BOOT_MESSAGE "P4MiniShell v0.1 ready | JC1060P470C | type help"
#define SHELL_PROMPT "> "
#define SHELL_HISTORY_BYTES 8192
#define SHELL_RENDER_BYTES (SHELL_HISTORY_BYTES + 256)
#define SHELL_COMMAND_BYTES 256
#define SHELL_KEYBOARD_HEIGHT 240

static lv_obj_t *s_terminal;
static lv_obj_t *s_keyboard;
static char s_history[SHELL_HISTORY_BYTES];
static size_t s_input_start;

static void shell_render(const char *input_text);

static void shell_history_reset(void)
{
    s_history[0] = '\0';
}

static void shell_history_append_text(const char *text)
{
    size_t current_len;
    size_t text_len;
    static const char truncation_marker[] = "\n[history truncated]\n";

    if (text == NULL || text[0] == '\0') {
        return;
    }

    current_len = strlen(s_history);
    text_len = strlen(text);

    if (text_len >= sizeof(s_history)) {
        text += text_len - (sizeof(s_history) - 1);
        text_len = strlen(text);
        shell_history_reset();
        current_len = 0;
    }

    if (current_len + text_len + 1 >= sizeof(s_history)) {
        size_t keep = sizeof(s_history) / 2;

        if (current_len > keep) {
            memmove(s_history, s_history + current_len - keep, keep);
            current_len = keep;
            s_history[current_len] = '\0';
        }

        if (current_len + sizeof(truncation_marker) < sizeof(s_history)) {
            memcpy(s_history + current_len, truncation_marker, sizeof(truncation_marker) - 1);
            current_len += sizeof(truncation_marker) - 1;
            s_history[current_len] = '\0';
        }
    }

    if (current_len + text_len + 1 >= sizeof(s_history)) {
        text_len = sizeof(s_history) - current_len - 1;
    }

    memcpy(s_history + current_len, text, text_len);
    s_history[current_len + text_len] = '\0';
}

static void shell_history_appendf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    shell_history_append_text(buffer);
}

static void shell_move_cursor_to_end(void)
{
    lv_textarea_set_cursor_pos(s_terminal, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_update_layout(s_terminal);
    lv_obj_scroll_to_y(s_terminal, LV_COORD_MAX, LV_ANIM_OFF);
}

static void shell_render(const char *input_text)
{
    char buffer[SHELL_RENDER_BYTES];
    const char *safe_input = input_text != NULL ? input_text : "";

    snprintf(buffer, sizeof(buffer), "%s%s%s", s_history, SHELL_PROMPT, safe_input);
    s_input_start = strlen(s_history) + strlen(SHELL_PROMPT);
    lv_textarea_set_text(s_terminal, buffer);
    shell_move_cursor_to_end();
}

static char *shell_trim(char *text)
{
    char *start = text;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    return start;
}

static void shell_append_prompt_line(const char *command)
{
    shell_history_append_text(SHELL_PROMPT);
    shell_history_append_text(command);
    shell_history_append_text("\n");
}

static void shell_command_help(void)
{
    shell_history_append_text("Built-in commands:\n");
    shell_history_append_text("  help    Show available commands\n");
    shell_history_append_text("  sysinfo Show board/runtime information\n");
    shell_history_append_text("  clear   Clear the terminal history\n");
    shell_history_append_text("  reboot  Restart the board\n");
}

static void shell_command_sysinfo(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#if CONFIG_SPIRAM
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif

    shell_history_appendf("board.requested_name: %s\n", SHELL_BOARD_REQUESTED);
    shell_history_appendf("board.detected_name: %s\n", SHELL_BOARD_DETECTED);
    shell_history_appendf("display: %d x %d, JD9165, reset GPIO %d, backlight GPIO %d\n",
                          BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_RST, BSP_LCD_BACKLIGHT);
    shell_history_appendf("display.timing: pclk=%dMHz, lanes=%d, bitrate=%dMbps, hsync=%d hbp=%d hfp=%d vsync=%d vbp=%d vfp=%d\n",
                          BSP_LCD_PIXEL_CLOCK_MHZ,
                          BSP_LCD_MIPI_DSI_LANE_NUM,
                          BOARD_CFG_LCD_DSI_BUS_LANE_BITRATE_MBPS_RUNTIME,
                          BSP_LCD_MIPI_DSI_LCD_HSYNC,
                          BSP_LCD_MIPI_DSI_LCD_HBP,
                          BSP_LCD_MIPI_DSI_LCD_HFP,
                          BSP_LCD_MIPI_DSI_LCD_VSYNC,
                          BSP_LCD_MIPI_DSI_LCD_VBP,
                          BSP_LCD_MIPI_DSI_LCD_VFP);
    shell_history_appendf("display.buffer: draw=%d, double=%d, dma=%d, spiram=%d, sw_rotate=%d\n",
                          BOARD_CFG_LCD_DRAW_BUFFER_SIZE,
                          BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE,
                          BOARD_CFG_APP_BUFFER_DMA,
                          BOARD_CFG_APP_BUFFER_SPIRAM,
                          BOARD_CFG_APP_SW_ROTATE);
    shell_history_appendf("touch: GT911 on I2C%d, SDA GPIO %d, SCL GPIO %d, %dHz, pullup=%d, swap_xy=%d, mirror_x=%d, mirror_y=%d\n",
                          BSP_I2C_NUM,
                          BSP_I2C_SDA,
                          BSP_I2C_SCL,
                          BOARD_CFG_I2C_CLK_SPEED_HZ,
                          BOARD_CFG_I2C_ENABLE_INTERNAL_PULLUP,
                          BOARD_CFG_TOUCH_SWAP_XY,
                          BOARD_CFG_TOUCH_MIRROR_X,
                          BOARD_CFG_TOUCH_MIRROR_Y);
    shell_history_appendf("storage: spiffs=%s, sd=%s\n", BSP_SPIFFS_MOUNT_POINT, BSP_SD_MOUNT_POINT);
    shell_history_appendf("idf: %s\n", esp_get_idf_version());
    shell_history_appendf("heap: free=%u bytes, internal_free=%u bytes\n",
                          (unsigned int)free_heap,
                          (unsigned int)free_internal);
#if CONFIG_SPIRAM
    shell_history_appendf("psram: enabled, total=%u bytes, free=%u bytes\n",
                          (unsigned int)total_psram,
                          (unsigned int)free_psram);
#else
    shell_history_append_text("psram: disabled\n");
#endif
    shell_history_append_text("wifi: unsupported on current esp32p4 target/config\n");
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static void shell_execute_command(char *command)
{
    char *trimmed = shell_trim(command);

    if (trimmed[0] == '\0') {
        shell_history_append_text("\n");
        return;
    }

    if (strcmp(trimmed, "help") == 0) {
        shell_command_help();
        return;
    }

    if (strcmp(trimmed, "sysinfo") == 0) {
        shell_command_sysinfo();
        return;
    }

    if (strcmp(trimmed, "clear") == 0) {
        shell_history_reset();
        return;
    }

    if (strcmp(trimmed, "reboot") == 0) {
        shell_history_append_text("Rebooting...\n");
        xTaskCreate(reboot_task, "reboot_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
        return;
    }

    shell_history_appendf("Unknown command: %s\n", trimmed);
}

static void shell_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *textarea = lv_event_get_target(event);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        shell_move_cursor_to_end();
        return;
    }

    if (code == LV_EVENT_READY) {
        const char *full_text = lv_textarea_get_text(textarea);
        char command[SHELL_COMMAND_BYTES];
        size_t full_len = strlen(full_text);
        const char *command_start = full_len >= s_input_start ? full_text + s_input_start : "";

        snprintf(command, sizeof(command), "%s", command_start);
        shell_append_prompt_line(command);
        shell_execute_command(command);
        shell_render("");
    }
}

static const lv_font_t *shell_select_font(void)
{
#if LV_FONT_UNSCII_16
    return &lv_font_unscii_16;
#else
    return LV_FONT_DEFAULT;
#endif
}

static void shell_build_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    const lv_font_t *terminal_font = shell_select_font();

    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B0F10), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_layout(screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_row(screen, 0, 0);

    s_terminal = lv_textarea_create(screen);
    lv_obj_set_width(s_terminal, LV_PCT(100));
    lv_obj_set_flex_grow(s_terminal, 1);
    lv_textarea_set_one_line(s_terminal, false);
    lv_textarea_set_cursor_click_pos(s_terminal, false);
    lv_obj_set_style_bg_color(s_terminal, lv_color_hex(0x050806), 0);
    lv_obj_set_style_bg_opa(s_terminal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_terminal, 0, 0);
    lv_obj_set_style_pad_all(s_terminal, 16, 0);
    lv_obj_set_style_text_color(s_terminal, lv_color_hex(0x8DFF96), 0);
    lv_obj_set_style_text_font(s_terminal, terminal_font, 0);
    lv_obj_set_scrollbar_mode(s_terminal, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(s_terminal, shell_textarea_event_cb, LV_EVENT_ALL, NULL);

    s_keyboard = lv_keyboard_create(screen);
    lv_obj_set_width(s_keyboard, LV_PCT(100));
    lv_obj_set_height(s_keyboard, SHELL_KEYBOARD_HEIGHT);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_keyboard, s_terminal);
    lv_obj_set_style_text_font(s_keyboard, terminal_font, 0);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x1D2625), 0);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);

    shell_history_reset();
    shell_history_append_text(SHELL_BOOT_MESSAGE);
    shell_history_append_text("\n");
    shell_render("");
}

void app_main(void)
{
    lv_display_t *display;

    // AI: preserve the BSP-driven LCD and touch startup path exactly as the original demo.
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

    display = bsp_display_start_with_config(&cfg);
    if (display == NULL) {
        ESP_LOGE(SHELL_TAG, "Display initialization failed");
        return;
    }

    bsp_display_backlight_on();

    // AI: create the LVGL shell surface after the BSP has started LVGL and touch input.
    bsp_display_lock(0);
    shell_build_ui();
    bsp_display_unlock();

    // AI: command parsing, prompt management, and built-in command handlers are event-driven from the textarea.
}
