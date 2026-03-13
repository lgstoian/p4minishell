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
#define SHELL_PROMPT "P4Shell> "
#define SHELL_TRANSCRIPT_BYTES 8192
#define SHELL_COMMAND_BYTES 256
#define SHELL_COMMAND_HISTORY_DEPTH 10
#define SHELL_KEYBOARD_HEIGHT 240
#define SHELL_INPUT_ROW_HEIGHT 52

static lv_obj_t *s_history_transcript;
static lv_obj_t *s_input_line;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_history_prev_button;
static lv_obj_t *s_history_next_button;
static char s_transcript[SHELL_TRANSCRIPT_BYTES];
static char s_command_history[SHELL_COMMAND_HISTORY_DEPTH][SHELL_COMMAND_BYTES];
static size_t s_command_history_count;
static int s_command_history_cursor = -1;
static char s_history_draft[SHELL_COMMAND_BYTES];

static void shell_input_line_reset(void);

static void shell_transcript_render(void)
{
    lv_textarea_set_text(s_history_transcript, s_transcript);
    lv_textarea_set_cursor_pos(s_history_transcript, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_update_layout(s_history_transcript);
    lv_obj_scroll_to_y(s_history_transcript, LV_COORD_MAX, LV_ANIM_OFF);
}

static void shell_transcript_reset(void)
{
    s_transcript[0] = '\0';
}

static void shell_transcript_append_text(const char *text)
{
    size_t current_len;
    size_t text_len;
    static const char truncation_marker[] = "\n[history truncated]\n";

    if (text == NULL || text[0] == '\0') {
        return;
    }

    current_len = strlen(s_transcript);
    text_len = strlen(text);

    if (text_len >= sizeof(s_transcript)) {
        text += text_len - (sizeof(s_transcript) - 1);
        text_len = strlen(text);
        shell_transcript_reset();
        current_len = 0;
    }

    if (current_len + text_len + 1 >= sizeof(s_transcript)) {
        size_t keep = sizeof(s_transcript) / 2;

        if (current_len > keep) {
            memmove(s_transcript, s_transcript + current_len - keep, keep);
            current_len = keep;
            s_transcript[current_len] = '\0';
        }

        if (current_len + sizeof(truncation_marker) < sizeof(s_transcript)) {
            memcpy(s_transcript + current_len, truncation_marker, sizeof(truncation_marker) - 1);
            current_len += sizeof(truncation_marker) - 1;
            s_transcript[current_len] = '\0';
        }
    }

    if (current_len + text_len + 1 >= sizeof(s_transcript)) {
        text_len = sizeof(s_transcript) - current_len - 1;
    }

    memcpy(s_transcript + current_len, text, text_len);
    s_transcript[current_len + text_len] = '\0';
}

static void shell_transcript_appendf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    shell_transcript_append_text(buffer);
}

static void shell_history_transcript_scroll_to_end(void)
{
    shell_transcript_render();
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

static void shell_input_line_set_text(const char *command_text)
{
    char buffer[sizeof(SHELL_PROMPT) + SHELL_COMMAND_BYTES];
    const char *safe_command = command_text != NULL ? command_text : "";

    snprintf(buffer, sizeof(buffer), "%s%s", SHELL_PROMPT, safe_command);
    lv_textarea_set_text(s_input_line, buffer);
    lv_textarea_set_cursor_pos(s_input_line, LV_TEXTAREA_CURSOR_LAST);
}

static void shell_input_line_reset(void)
{
    shell_input_line_set_text("");
}

static void shell_extract_input_text(char *output, size_t output_size)
{
    const char *text = lv_textarea_get_text(s_input_line);

    if (strncmp(text, SHELL_PROMPT, strlen(SHELL_PROMPT)) == 0) {
        snprintf(output, output_size, "%s", text + strlen(SHELL_PROMPT));
    } else {
        snprintf(output, output_size, "%s", text);
    }

    shell_trim(output);
}

static void shell_store_command_history(const char *command)
{
    size_t index;

    if (command == NULL || command[0] == '\0') {
        return;
    }

    if (s_command_history_count > 0 &&
        strcmp(s_command_history[s_command_history_count - 1], command) == 0) {
        return;
    }

    if (s_command_history_count == SHELL_COMMAND_HISTORY_DEPTH) {
        for (index = 1; index < SHELL_COMMAND_HISTORY_DEPTH; index++) {
            snprintf(s_command_history[index - 1], SHELL_COMMAND_BYTES, "%s", s_command_history[index]);
        }
        s_command_history_count--;
    }

    snprintf(s_command_history[s_command_history_count], SHELL_COMMAND_BYTES, "%s", command);
    s_command_history_count++;
}

static void shell_recall_history(int direction)
{
    char current_input[SHELL_COMMAND_BYTES];

    if (s_command_history_count == 0) {
        return;
    }

    shell_extract_input_text(current_input, sizeof(current_input));

    if (s_command_history_cursor < 0) {
        snprintf(s_history_draft, sizeof(s_history_draft), "%s", current_input);
        if (direction < 0) {
            s_command_history_cursor = (int)s_command_history_count - 1;
        } else {
            return;
        }
    } else {
        s_command_history_cursor += direction;
        if (s_command_history_cursor < 0) {
            s_command_history_cursor = -1;
            shell_input_line_set_text(s_history_draft);
            return;
        }

        if (s_command_history_cursor >= (int)s_command_history_count) {
            s_command_history_cursor = -1;
            shell_input_line_set_text(s_history_draft);
            return;
        }
    }

    shell_input_line_set_text(s_command_history[s_command_history_cursor]);
}

static void shell_command_help(void)
{
    shell_transcript_append_text("Built-in commands:\n");
    shell_transcript_append_text("  help    Show available commands\n");
    shell_transcript_append_text("  sysinfo Show board/runtime information\n");
    shell_transcript_append_text("  clear   Clear the terminal history\n");
    shell_transcript_append_text("  reboot  Restart the board\n");
    shell_transcript_append_text("History recall: Prev/Next buttons above the keyboard\n");
}

static void shell_command_sysinfo(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#if CONFIG_SPIRAM
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif

    shell_transcript_appendf("board.requested_name: %s\n", SHELL_BOARD_REQUESTED);
    shell_transcript_appendf("board.detected_name: %s\n", SHELL_BOARD_DETECTED);
    shell_transcript_appendf("display: %d x %d, JD9165, reset GPIO %d, backlight GPIO %d\n",
                          BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_RST, BSP_LCD_BACKLIGHT);
    shell_transcript_appendf("display.timing: pclk=%dMHz, lanes=%d, bitrate=%dMbps, hsync=%d hbp=%d hfp=%d vsync=%d vbp=%d vfp=%d\n",
                          BSP_LCD_PIXEL_CLOCK_MHZ,
                          BSP_LCD_MIPI_DSI_LANE_NUM,
                          BOARD_CFG_LCD_DSI_BUS_LANE_BITRATE_MBPS_RUNTIME,
                          BSP_LCD_MIPI_DSI_LCD_HSYNC,
                          BSP_LCD_MIPI_DSI_LCD_HBP,
                          BSP_LCD_MIPI_DSI_LCD_HFP,
                          BSP_LCD_MIPI_DSI_LCD_VSYNC,
                          BSP_LCD_MIPI_DSI_LCD_VBP,
                          BSP_LCD_MIPI_DSI_LCD_VFP);
    shell_transcript_appendf("display.buffer: draw=%d, double=%d, dma=%d, spiram=%d, sw_rotate=%d\n",
                          BOARD_CFG_LCD_DRAW_BUFFER_SIZE,
                          BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE,
                          BOARD_CFG_APP_BUFFER_DMA,
                          BOARD_CFG_APP_BUFFER_SPIRAM,
                          BOARD_CFG_APP_SW_ROTATE);
    shell_transcript_appendf("touch: GT911 on I2C%d, SDA GPIO %d, SCL GPIO %d, %dHz, pullup=%d, swap_xy=%d, mirror_x=%d, mirror_y=%d\n",
                          BSP_I2C_NUM,
                          BSP_I2C_SDA,
                          BSP_I2C_SCL,
                          BOARD_CFG_I2C_CLK_SPEED_HZ,
                          BOARD_CFG_I2C_ENABLE_INTERNAL_PULLUP,
                          BOARD_CFG_TOUCH_SWAP_XY,
                          BOARD_CFG_TOUCH_MIRROR_X,
                          BOARD_CFG_TOUCH_MIRROR_Y);
    shell_transcript_appendf("storage: spiffs=%s, sd=%s\n", BSP_SPIFFS_MOUNT_POINT, BSP_SD_MOUNT_POINT);
    shell_transcript_appendf("idf: %s\n", esp_get_idf_version());
    shell_transcript_appendf("heap: free=%u bytes, internal_free=%u bytes\n",
                          (unsigned int)free_heap,
                          (unsigned int)free_internal);
#if CONFIG_SPIRAM
    shell_transcript_appendf("psram: enabled, total=%u bytes, free=%u bytes\n",
                          (unsigned int)total_psram,
                          (unsigned int)free_psram);
#else
    shell_transcript_append_text("psram: disabled\n");
#endif
    shell_transcript_append_text("wifi: unsupported on current esp32p4 target/config\n");
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
        shell_transcript_reset();
        return;
    }

    if (strcmp(trimmed, "reboot") == 0) {
        shell_transcript_append_text("Rebooting...\n");
        xTaskCreate(reboot_task, "reboot_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
        return;
    }

    shell_transcript_appendf("Unknown command: %s\n", trimmed);
}

static void shell_history_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    if (lv_event_get_target(event) == s_history_prev_button) {
        shell_recall_history(-1);
    } else if (lv_event_get_target(event) == s_history_next_button) {
        shell_recall_history(1);
    }
}

static void shell_transcript_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_keyboard, s_input_line);
        lv_obj_add_state(s_input_line, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(s_input_line, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void shell_input_line_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(s_keyboard, s_input_line);
        lv_textarea_set_cursor_pos(s_input_line, LV_TEXTAREA_CURSOR_LAST);
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        const char *text = lv_textarea_get_text(s_input_line);

        if (strncmp(text, SHELL_PROMPT, strlen(SHELL_PROMPT)) != 0) {
            char repaired[SHELL_COMMAND_BYTES];

            snprintf(repaired, sizeof(repaired), "%s", text);
            shell_trim(repaired);
            shell_input_line_set_text(repaired);
        }
        return;
    }

    if (code == LV_EVENT_READY) {
        char command[SHELL_COMMAND_BYTES];

        shell_extract_input_text(command, sizeof(command));
        shell_transcript_appendf("%s%s\n", SHELL_PROMPT, command);
        shell_store_command_history(command);
        s_command_history_cursor = -1;
        s_history_draft[0] = '\0';
        shell_input_line_reset();
        shell_execute_command(command);
        shell_history_transcript_scroll_to_end();
        return;
    }

    if (code == LV_EVENT_KEY) {
        uint32_t key = *((const uint32_t *)lv_event_get_param(event));

        if (key == LV_KEY_UP) {
            shell_recall_history(-1);
        } else if (key == LV_KEY_DOWN) {
            shell_recall_history(1);
        }
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
    lv_obj_t *input_row;
    lv_obj_t *screen = lv_screen_active();
    const lv_font_t *terminal_font = shell_select_font();

    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B0F10), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_layout(screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_row(screen, 0, 0);

    // AI: the transcript and input line are separated so command submission does not mutate the visible history.
    s_history_transcript = lv_textarea_create(screen);
    lv_obj_set_width(s_history_transcript, LV_PCT(100));
    lv_obj_set_flex_grow(s_history_transcript, 1);
    lv_textarea_set_one_line(s_history_transcript, false);
    lv_textarea_set_cursor_click_pos(s_history_transcript, false);
    lv_obj_set_style_bg_color(s_history_transcript, lv_color_hex(0x050806), 0);
    lv_obj_set_style_bg_opa(s_history_transcript, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_history_transcript, 0, 0);
    lv_obj_set_style_pad_all(s_history_transcript, 16, 0);
    lv_obj_set_style_text_color(s_history_transcript, lv_color_hex(0x8DFF96), 0);
    lv_obj_set_style_text_font(s_history_transcript, terminal_font, 0);
    lv_obj_set_scrollbar_mode(s_history_transcript, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(s_history_transcript, shell_transcript_event_cb, LV_EVENT_ALL, NULL);

    input_row = lv_obj_create(screen);
    lv_obj_set_width(input_row, LV_PCT(100));
    lv_obj_set_height(input_row, SHELL_INPUT_ROW_HEIGHT);
    lv_obj_set_layout(input_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(input_row, 8, 0);
    lv_obj_set_style_pad_ver(input_row, 6, 0);
    lv_obj_set_style_pad_column(input_row, 8, 0);
    lv_obj_set_style_bg_color(input_row, lv_color_hex(0x111816), 0);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);

    s_history_prev_button = lv_button_create(input_row);
    lv_obj_set_size(s_history_prev_button, 82, LV_PCT(100));
    lv_obj_add_event_cb(s_history_prev_button, shell_history_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_label = lv_label_create(s_history_prev_button);
    lv_label_set_text(prev_label, "Prev");
    lv_obj_center(prev_label);

    s_history_next_button = lv_button_create(input_row);
    lv_obj_set_size(s_history_next_button, 82, LV_PCT(100));
    lv_obj_add_event_cb(s_history_next_button, shell_history_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_label = lv_label_create(s_history_next_button);
    lv_label_set_text(next_label, "Next");
    lv_obj_center(next_label);

    s_input_line = lv_textarea_create(input_row);
    lv_obj_set_flex_grow(s_input_line, 1);
    lv_obj_set_height(s_input_line, LV_PCT(100));
    lv_textarea_set_one_line(s_input_line, true);
    lv_textarea_set_cursor_click_pos(s_input_line, false);
    lv_obj_set_style_bg_color(s_input_line, lv_color_hex(0x050806), 0);
    lv_obj_set_style_bg_opa(s_input_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_input_line, 0, 0);
    lv_obj_set_style_pad_hor(s_input_line, 12, 0);
    lv_obj_set_style_pad_ver(s_input_line, 10, 0);
    lv_obj_set_style_text_color(s_input_line, lv_color_hex(0x8DFF96), 0);
    lv_obj_set_style_text_font(s_input_line, terminal_font, 0);
    lv_obj_add_event_cb(s_input_line, shell_input_line_event_cb, LV_EVENT_ALL, NULL);

    s_keyboard = lv_keyboard_create(screen);
    lv_obj_set_width(s_keyboard, LV_PCT(100));
    lv_obj_set_height(s_keyboard, SHELL_KEYBOARD_HEIGHT);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_keyboard, s_input_line);
    lv_obj_set_style_text_font(s_keyboard, terminal_font, 0);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x1D2625), 0);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);

    shell_transcript_reset();
    shell_transcript_append_text(SHELL_BOOT_MESSAGE);
    shell_transcript_append_text("\n");
    shell_history_transcript_scroll_to_end();
    shell_input_line_reset();
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

    // AI: command parsing, prompt management, transcript updates, and history recall are event-driven from the dedicated input line.
}
