/**
 * @file periph_commands.c
 * @brief Peripheral toolkit verbs (gpio/pwm/freq/adc/i2c/spi/rgb/camera).
 *
 * Moved verbatim out of command.c in v0.35.4. Every toolkit command gates
 * its pins through shell_pin_is_reserved() so active board lines can never
 * be repurposed. The single dispatcher in command.c calls these, it never
 * implements them.
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "display.h"
#include "led.h"
#include "command.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/spi_common.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "soc/adc_channel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Compat aliases (moved with the toolkit in v0.35.4). */
#define SHELL_C6_HOST_RESET_GPIO        P4_CONFIG_C6_HOST_RESET_GPIO
#define SHELL_PWM_FREQ_MAX_HZ           P4_CONFIG_PWM_FREQ_MAX_HZ
#define SHELL_PWM_SRC_CLK_HZ            P4_CONFIG_PWM_SRC_CLK_HZ
#define SHELL_PWM_CLK_SOURCE            P4_CONFIG_PWM_CLK_SOURCE
#define SHELL_PWM_CHANNEL_MAX           P4_CONFIG_PWM_CHANNEL_MAX
#define SHELL_PWM_DUTY_DEFAULT_PCT      P4_CONFIG_PWM_DUTY_DEFAULT_PCT
#define SHELL_ADC_DEFAULT_SAMPLES       P4_CONFIG_ADC_DEFAULT_SAMPLES
#define SHELL_ADC_MAX_SAMPLES           P4_CONFIG_ADC_MAX_SAMPLES
#define SHELL_ADC_ATTEN                 P4_CONFIG_ADC_ATTEN
#define SHELL_I2C_TOOL_TIMEOUT_MS       P4_CONFIG_I2C_TOOL_TIMEOUT_MS
#define SHELL_I2C_SCAN_PROBE_TIMEOUT_MS P4_CONFIG_I2C_SCAN_PROBE_TIMEOUT_MS
#define SHELL_I2C_SCAN_FIRST_ADDR       P4_CONFIG_I2C_SCAN_FIRST_ADDR
#define SHELL_I2C_SCAN_LAST_ADDR        P4_CONFIG_I2C_SCAN_LAST_ADDR
#define SHELL_I2C_TOOL_CLK_HZ           P4_CONFIG_I2C_TOOL_CLK_HZ
#define SHELL_SPI_TOOL_CLK_HZ           P4_CONFIG_SPI_TOOL_CLK_HZ
#define SHELL_SPI_TOOL_TIMEOUT_MS       P4_CONFIG_SPI_TOOL_TIMEOUT_MS
#define SHELL_SPI_TOOL_BUFFER_BYTES     P4_CONFIG_SPI_TOOL_BUFFER_BYTES
#define SHELL_SPI_TOOL_HOST             P4_CONFIG_SPI_TOOL_HOST

/** One entry in the exposed board GPIO table. */
typedef struct {
    const char *name;
    gpio_num_t gpio;
    const char *role;
    bool allow_output;
    bool critical;
} shell_gpio_pin_desc_t;

/* ========================================================================
 * BOARD GPIO TABLE
 * ======================================================================== */

static const shell_gpio_pin_desc_t s_gpio_pins[] = {
    {"i2c_sda", BSP_I2C_SDA, "Shared control bus for GT911 touch and onboard peripherals", false, true},
    {"i2c_scl", BSP_I2C_SCL, "Shared control clock for GT911 touch and onboard peripherals", false, true},
    {"i2s_dout", BSP_I2S_DOUT, "Audio codec data from the ESP32-P4 to the speaker path", false, true},
    {"i2s_lclk", BSP_I2S_LCLK, "Audio codec word-select clock", false, true},
    {"i2s_dsin", BSP_I2S_DSIN, "Audio codec data into the ESP32-P4", false, true},
    {"i2s_sclk", BSP_I2S_SCLK, "Audio codec bit clock", false, true},
    {"i2s_mclk", BSP_I2S_MCLK, "Audio codec master clock", false, true},
    {"power_amp", BSP_POWER_AMP_IO, "Speaker amplifier enable line", true, false},
    {"backlight", BSP_LCD_BACKLIGHT, "JD9165 panel backlight control", false, true},
    {"lcd_reset", BSP_LCD_RST, "JD9165 panel hardware reset", false, true},
    {"battery_adc", BOARD_CFG_BATTERY_ADC_GPIO, "Battery divider sense input", false, true},
    {"hosted_sdio_d0", GPIO_NUM_14, "ESP32-C6 hosted SDIO data lane D0", false, true},
    {"hosted_sdio_d1", GPIO_NUM_15, "ESP32-C6 hosted SDIO data lane D1", false, true},
    {"hosted_sdio_d2", GPIO_NUM_16, "ESP32-C6 hosted SDIO data lane D2", false, true},
    {"hosted_sdio_d3", GPIO_NUM_17, "ESP32-C6 hosted SDIO data lane D3", false, true},
    {"hosted_sdio_clk", GPIO_NUM_18, "ESP32-C6 hosted SDIO clock", false, true},
    {"hosted_sdio_cmd", GPIO_NUM_19, "ESP32-C6 hosted SDIO command", false, true},
    {"c6_host_reset", (gpio_num_t)SHELL_C6_HOST_RESET_GPIO, "ESP32-C6 hosted reset or enable control", false, true},
    {"sd_d0", BSP_SD_D0, "MicroSD data lane D0", false, true},
    {"sd_d1", BSP_SD_D1, "MicroSD data lane D1", false, true},
    {"sd_d2", BSP_SD_D2, "MicroSD data lane D2", false, true},
    {"sd_d3", BSP_SD_D3, "MicroSD data lane D3", false, true},
    {"sd_clk", BSP_SD_CLK, "MicroSD clock", false, true},
    {"sd_cmd", BSP_SD_CMD, "MicroSD command", false, true},
    {"led_status", (gpio_num_t)BOARD_CFG_RGB_LED_GPIO, "WS2812 RGB status LED (LED1, back panel)", false, true},
}




;

static const shell_gpio_pin_desc_t *shell_find_gpio_pin(int gpio_num)
{
    size_t index;

    for (index = 0; index < sizeof(s_gpio_pins) / sizeof(s_gpio_pins[0]); index++) {
        if ((int)s_gpio_pins[index].gpio == gpio_num) {
            return &s_gpio_pins[index];
        }
    }

    return NULL;
}

/**
 * Pin-safety gate for the peripheral toolkit (pwm, freq, adc, i2c, spi).
 *
 * A pin is reserved when it is missing from the exposed table (untracked),
 * out of the GPIO range, or listed in the table as critical (active board
 * lines such as I2C, I2S, SDIO, display, SD, and the battery ADC). Every
 * toolkit command refuses reserved pins, so a peripheral never disturbs a
 * live board function.
 */
static bool shell_pin_is_reserved(int gpio_num)
{
    const shell_gpio_pin_desc_t *pin;

    if (gpio_num < 0 || gpio_num > GPIO_NUM_MAX || gpio_num == GPIO_NUM_NC) {
        return true;
    }
    pin = shell_find_gpio_pin(gpio_num);
    return pin != NULL && pin->critical;
}

/* ========================================================================
 * GPIO COMMANDS
 * ======================================================================== */

static const char *shell_gpio_access_label(const shell_gpio_pin_desc_t *pin)
{
    if (pin == NULL) {
        return "unknown";
    }

    if (pin->allow_output && !pin->critical) {
        return "shell-settable";
    }

    return "monitor-only";
}

static void shell_print_gpio_entry(const shell_gpio_pin_desc_t *pin)
{
    int level;

    if (pin == NULL || pin->gpio < 0) {
        return;
    }

    level = gpio_get_level(pin->gpio);
    shell_transcript_appendf_ansi(SH_LBL "gpio.list:" SH_RST " " SH_LBL "name=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "level=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "access=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "critical=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "role=" SH_RST SH_DESC "%s" SH_RST "\n",
                             pin->name,
                             pin->gpio,
                             level,
                             shell_gpio_access_label(pin),
                             pin->critical ? "yes" : "no",
                             pin->role != NULL ? pin->role : "unspecified");
}

static esp_err_t shell_gpio_set_safe_level(int gpio_num, int level)
{
    const shell_gpio_pin_desc_t *pin = shell_find_gpio_pin(gpio_num);
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (pin == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!pin->allow_output || pin->critical) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (gpio_config(&config) != ESP_OK) {
        return ESP_FAIL;
    }

    return gpio_set_level((gpio_num_t)gpio_num, level);
}

static void shell_command_gpio_status(void)
{
    size_t index;

    for (index = 0; index < sizeof(s_gpio_pins) / sizeof(s_gpio_pins[0]); index++) {
        const shell_gpio_pin_desc_t *pin = &s_gpio_pins[index];
        int level = gpio_get_level(pin->gpio);

        shell_transcript_appendf_ansi(SH_LBL "gpio.status:" SH_RST " " SH_LBL "name=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "level=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "access=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "role=" SH_RST SH_DESC "%s" SH_RST "\n",
                                 pin->name,
                                 pin->gpio,
                                 level,
                                 shell_gpio_access_label(pin),
                                 pin->role != NULL ? pin->role : "unspecified");
    }
}

void shell_execute_gpio_command(int argc, char **argv)
{
    size_t index;
    char *end = NULL;
    long gpio_num;
    long level;
    esp_err_t error;

    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "status"))) {
        shell_command_gpio_status();
        return;
    }

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "list")) {
        for (index = 0; index < sizeof(s_gpio_pins) / sizeof(s_gpio_pins[0]); index++) {
            shell_print_gpio_entry(&s_gpio_pins[index]);
        }
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "read")) {
        const shell_gpio_pin_desc_t *pin;

        gpio_num = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: gpio read <pin>");
            shell_record_warningf("gpio", "Usage error for gpio read command");
            return;
        }
        pin = shell_find_gpio_pin((int)gpio_num);
        if (pin != NULL) {
            shell_transcript_appendf("gpio read: name=%s pin=%ld level=%d role=%s\n",
                                     pin->name,
                                     gpio_num,
                                     gpio_get_level((gpio_num_t)gpio_num),
                                     pin->role != NULL ? pin->role : "unspecified");
        } else {
            shell_transcript_appendf("gpio read: pin=%ld level=%d\n", gpio_num, gpio_get_level((gpio_num_t)gpio_num));
        }
        return;
    }

    if (argc == 4 && shell_text_equals_ignore_case(argv[1], "set")) {
        gpio_num = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: gpio set <pin> <0|1>");
            shell_record_warningf("gpio", "Usage error for gpio set pin argument");
            return;
        }

        end = NULL;
        level = strtol(argv[3], &end, 10);
        if (end == NULL || *end != '\0' || (level != 0 && level != 1)) {
            shell_print_usage("Usage: gpio set <pin> <0|1>");
            shell_record_warningf("gpio", "Usage error for gpio set level argument");
            return;
        }

        error = shell_gpio_set_safe_level((int)gpio_num, (int)level);
        if (error == ESP_ERR_NOT_SUPPORTED) {
            shell_transcript_appendf("gpio set: pin %ld is reserved for active board functions and is read-only from the shell\n", gpio_num);
            shell_record_warningf("gpio", "Rejected unsafe gpio set on pin %ld", gpio_num);
            return;
        }
        if (error == ESP_ERR_NOT_FOUND) {
            shell_transcript_appendf("gpio set: pin %ld is not in the exposed board pin list\n", gpio_num);
            shell_record_warningf("gpio", "Unknown gpio set pin %ld", gpio_num);
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("gpio set: failed to drive pin %ld (%s)", gpio_num, esp_err_to_name(error));
            shell_record_errorf("gpio", error, "Failed to set gpio %ld", gpio_num);
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "gpio set:" SH_RST " " SH_LBL "pin=" SH_RST SH_NUM "%ld" SH_RST " " SH_LBL "level=" SH_RST SH_NUM "%ld" SH_RST "\n", gpio_num, level);
        return;
    }

    shell_print_usage("Usage: gpio list | gpio status | gpio read <pin> | gpio set <pin> <0|1>");
    shell_record_warningf("gpio", "Usage error for gpio command");
}

/* ========================================================================
 * PERIPHERAL TOOLKIT: pwm, freq, adc, i2c, spi
 * ========================================================================
 * A richer GPIO/peripheral surface built on the LEDC, ADC one-shot, I2C
 * master, and SPI master drivers. Every command gates its pins through
 * shell_pin_is_reserved() so active board lines (I2C, I2S, SDIO, display,
 * SD, battery ADC) can never be repurposed by a peripheral.
 */

/* ---- PWM and square-wave generation (LEDC) ----
 * The display backlight owns LEDC channel 1 / timer 1 on this board, so the
 * toolkit allocates channels and timers from the remaining set and never
 * touches the backlight path. */

#define SHELL_PWM_BACKLIGHT_CHANNEL      BOARD_CFG_DISPLAY_BRIGHTNESS_LEDC_CH
#define SHELL_PWM_BACKLIGHT_TIMER        BOARD_CFG_LCD_BACKLIGHT_PWM_TIMER

/** LEDC timers available to the toolkit (P4 has 4 timers, timer 1 is used). */
static const ledc_timer_t s_pwm_timers[] = {
    LEDC_TIMER_0, LEDC_TIMER_2, LEDC_TIMER_3
};

/** LEDC channels available to the toolkit (channel 1 is used by backlight). */
static const ledc_channel_t s_pwm_channels[] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_2, LEDC_CHANNEL_3,
    LEDC_CHANNEL_4, LEDC_CHANNEL_5, LEDC_CHANNEL_6, LEDC_CHANNEL_7
};

typedef struct {
    bool in_use;
    int gpio_num;
    ledc_channel_t channel;
    ledc_timer_t timer;
    uint32_t freq_hz;
    uint32_t duty_pct;
} shell_pwm_slot_t;

static shell_pwm_slot_t s_pwm_slots[SHELL_PWM_CHANNEL_MAX];

static shell_pwm_slot_t *shell_pwm_find_slot_by_gpio(int gpio_num)
{
    size_t index;

    for (index = 0; index < SHELL_PWM_CHANNEL_MAX; index++) {
        if (s_pwm_slots[index].in_use && s_pwm_slots[index].gpio_num == gpio_num) {
            return &s_pwm_slots[index];
        }
    }
    return NULL;
}

static shell_pwm_slot_t *shell_pwm_find_free_slot(void)
{
    size_t index;

    for (index = 0; index < SHELL_PWM_CHANNEL_MAX; index++) {
        if (!s_pwm_slots[index].in_use) {
            return &s_pwm_slots[index];
        }
    }
    return NULL;
}

/** Claim a free timer and channel that no other active slot is using. */
static esp_err_t shell_pwm_claim(ledc_timer_t *timer_out, ledc_channel_t *channel_out)
{
    size_t i;
    size_t j;

    for (i = 0; i < sizeof(s_pwm_timers) / sizeof(s_pwm_timers[0]); i++) {
        bool used = false;
        for (j = 0; j < SHELL_PWM_CHANNEL_MAX; j++) {
            if (s_pwm_slots[j].in_use && s_pwm_slots[j].timer == s_pwm_timers[i]) {
                used = true;
                break;
            }
        }
        if (!used) {
            *timer_out = s_pwm_timers[i];
            break;
        }
    }
    if (i == sizeof(s_pwm_timers) / sizeof(s_pwm_timers[0])) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < sizeof(s_pwm_channels) / sizeof(s_pwm_channels[0]); i++) {
        bool used = false;
        for (j = 0; j < SHELL_PWM_CHANNEL_MAX; j++) {
            if (s_pwm_slots[j].in_use && s_pwm_slots[j].channel == s_pwm_channels[i]) {
                used = true;
                break;
            }
        }
        if (!used) {
            *channel_out = s_pwm_channels[i];
            break;
        }
    }
    if (i == sizeof(s_pwm_channels) / sizeof(s_pwm_channels[0])) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/**
 * Pick the LEDC duty resolution that can represent the requested frequency
 * given the assumed timer source clock, capped at the SOC timer width.
 */
static ledc_timer_bit_t shell_pwm_pick_resolution(uint32_t freq_hz)
{
    int bits = 20; /* SOC_LEDC_TIMER_BIT_WIDTH */

    while (bits > 1 && ((uint64_t)freq_hz << bits) > (uint64_t)SHELL_PWM_SRC_CLK_HZ) {
        bits--;
    }
    return (ledc_timer_bit_t)bits;
}

/** Configure or update an LEDC output for a pin. */
static esp_err_t shell_pwm_apply(int gpio_num, uint32_t freq_hz, uint32_t duty_pct)
{
    shell_pwm_slot_t *slot;
    ledc_timer_t timer;
    ledc_channel_t channel;
    ledc_timer_bit_t resolution;
    uint32_t max_duty;
    uint32_t duty_count;
    esp_err_t error;

    slot = shell_pwm_find_slot_by_gpio(gpio_num);
    if (slot == NULL) {
        error = shell_pwm_claim(&timer, &channel);
        if (error != ESP_OK) {
            return error;
        }
        slot = shell_pwm_find_free_slot();
        if (slot == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        slot->in_use = true;
        slot->gpio_num = gpio_num;
        slot->timer = timer;
        slot->channel = channel;
    }

    resolution = shell_pwm_pick_resolution(freq_hz);
    {
        ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = resolution,
            .timer_num = slot->timer,
            .freq_hz = freq_hz,
            .clk_cfg = (ledc_clk_cfg_t)SHELL_PWM_CLK_SOURCE,
        };
        error = ledc_timer_config(&timer_cfg);
        if (error != ESP_OK) {
            goto fail;
        }
    }

    max_duty = (1UL << (int)resolution) - 1;
    duty_count = (uint32_t)(((uint64_t)max_duty * duty_pct) / 100);
    {
        ledc_channel_config_t channel_cfg = {
            .gpio_num = gpio_num,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = slot->channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = slot->timer,
            .duty = duty_count,
            .hpoint = 0,
        };
        error = ledc_channel_config(&channel_cfg);
        if (error != ESP_OK) {
            goto fail;
        }
        error = ledc_update_duty(LEDC_LOW_SPEED_MODE, slot->channel);
        if (error != ESP_OK) {
            goto fail;
        }
    }

    slot->freq_hz = freq_hz;
    slot->duty_pct = duty_pct;
    return ESP_OK;

fail:
    ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0);
    gpio_reset_pin((gpio_num_t)gpio_num);
    slot->in_use = false;
    return error;
}

/** Stop and release the LEDC output on a pin. */
static esp_err_t shell_pwm_stop_pin(int gpio_num)
{
    shell_pwm_slot_t *slot = shell_pwm_find_slot_by_gpio(gpio_num);

    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0);
    gpio_reset_pin((gpio_num_t)gpio_num);
    slot->in_use = false;
    return ESP_OK;
}

/** Shared status listing for `pwm status` / `freq status`. */
static void shell_pwm_print_status(void)
{
    size_t index;
    bool any = false;

    for (index = 0; index < SHELL_PWM_CHANNEL_MAX; index++) {
        if (!s_pwm_slots[index].in_use) {
            continue;
        }
        any = true;
        shell_transcript_appendf_ansi(SH_LBL "pwm.status:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST
                                 " " SH_LBL "freq=" SH_RST SH_NUM "%lu Hz" SH_RST " " SH_LBL "duty=" SH_RST SH_NUM "%lu%%" SH_RST "\n",
                                 s_pwm_slots[index].gpio_num,
                                 (unsigned long)s_pwm_slots[index].freq_hz,
                                 (unsigned long)s_pwm_slots[index].duty_pct);
    }
    if (!any) {
        shell_transcript_appendf_ansi(SH_LBL "pwm.status:" SH_RST " " SH_MUTE "no active outputs" SH_RST "\n");
    }
}

/** Shared `stop <pin>` handling for `pwm` and `freq`. */
static void shell_pwm_cmd_stop(const char *label, const char *pin_text)
{
    char *end = NULL;
    long gpio_num = strtol(pin_text, &end, 10);

    if (end == NULL || *end != '\0') {
        shell_print_usage("Usage: %s stop <pin>", label);
        shell_record_warningf(label, "Usage error for %s stop", label);
        return;
    }
    if (shell_pin_is_reserved((int)gpio_num)) {
        shell_print_warning("%s: pin %ld is reserved for active board functions", label, gpio_num);
        shell_record_warningf(label, "Rejected reserved pin %ld", gpio_num);
        return;
    }
    if (shell_pwm_stop_pin((int)gpio_num) == ESP_ERR_NOT_FOUND) {
        shell_transcript_appendf("%s: no active output on pin %ld\n", label, gpio_num);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "%s stop:" SH_RST " " SH_LBL "pin=" SH_RST SH_NUM "%ld" SH_RST " " SH_OK "stopped" SH_RST "\n",
                             label, gpio_num);
}

/** Parse an optional duty argument; defaults apply when omitted. */
static bool shell_pwm_parse_duty_arg(const char *text, long *duty_out)
{
    char *end = NULL;
    long value;

    if (text == NULL) {
        *duty_out = SHELL_PWM_DUTY_DEFAULT_PCT;
        return true;
    }
    value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0' || value < 0 || value > 100) {
        return false;
    }
    *duty_out = value;
    return true;
}

void shell_execute_pwm_command(int argc, char **argv)
{
    char *end = NULL;
    long gpio_num;
    long freq;
    long duty;
    esp_err_t error;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        shell_pwm_print_status();
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "stop")) {
        shell_pwm_cmd_stop("pwm", argv[2]);
        return;
    }

    if (argc == 4) {
        gpio_num = strtol(argv[1], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: pwm <pin> <freq_hz> <duty_pct>");
            shell_record_warningf("pwm", "Usage error for pwm pin argument");
            return;
        }
        end = NULL;
        freq = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || freq <= 0 || freq > SHELL_PWM_FREQ_MAX_HZ) {
            shell_print_usage("Usage: pwm <pin> <freq_hz> <duty_pct>  (freq 1..%d Hz)",
                              (int)SHELL_PWM_FREQ_MAX_HZ);
            shell_record_warningf("pwm", "Invalid pwm frequency argument");
            return;
        }
        if (!shell_pwm_parse_duty_arg(argv[3], &duty)) {
            shell_print_usage("Usage: pwm <pin> <freq_hz> <duty_pct>  (duty 0..100)");
            shell_record_warningf("pwm", "Invalid pwm duty argument");
            return;
        }

        if (shell_pin_is_reserved((int)gpio_num)) {
            shell_print_warning("pwm: pin %ld is reserved for active board functions", gpio_num);
            shell_record_warningf("pwm", "Rejected reserved pin %ld", gpio_num);
            return;
        }

        error = shell_pwm_apply((int)gpio_num, (uint32_t)freq, (uint32_t)duty);
        if (error == ESP_ERR_INVALID_STATE) {
            shell_print_error("pwm: no free PWM channel (stop one first)");
            shell_record_errorf("pwm", error, "PWM channel pool exhausted");
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("pwm: failed to configure LEDC on pin %ld (%s)", gpio_num, esp_err_to_name(error));
            shell_record_errorf("pwm", error, "LEDC config failed on pin %ld", gpio_num);
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "pwm set:" SH_RST " " SH_LBL "pin=" SH_RST SH_NUM "%ld" SH_RST
                                 " " SH_LBL "freq=" SH_RST SH_NUM "%ld Hz" SH_RST " " SH_LBL "duty=" SH_RST SH_NUM "%ld%%" SH_RST "\n",
                                 gpio_num, freq, duty);
        return;
    }

    shell_print_usage("Usage: pwm status | pwm stop <pin> | pwm <pin> <freq_hz> <duty_pct>");
    shell_record_warningf("pwm", "Usage error for pwm command");
}

void shell_execute_freq_command(int argc, char **argv)
{
    char *end = NULL;
    long gpio_num;
    long freq;
    esp_err_t error;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        shell_pwm_print_status();
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "stop")) {
        shell_pwm_cmd_stop("freq", argv[2]);
        return;
    }

    if (argc == 3) {
        gpio_num = strtol(argv[1], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: freq <pin> <hz>");
            shell_record_warningf("freq", "Usage error for freq pin argument");
            return;
        }
        end = NULL;
        freq = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || freq <= 0 || freq > SHELL_PWM_FREQ_MAX_HZ) {
            shell_print_usage("Usage: freq <pin> <hz>  (1..%d Hz)", (int)SHELL_PWM_FREQ_MAX_HZ);
            shell_record_warningf("freq", "Invalid freq argument");
            return;
        }

        if (shell_pin_is_reserved((int)gpio_num)) {
            shell_print_warning("freq: pin %ld is reserved for active board functions", gpio_num);
            shell_record_warningf("freq", "Rejected reserved pin %ld", gpio_num);
            return;
        }

        /* A square wave is PWM at 50% duty; reuse the single LEDC engine. */
        error = shell_pwm_apply((int)gpio_num, (uint32_t)freq, SHELL_PWM_DUTY_DEFAULT_PCT);
        if (error == ESP_ERR_INVALID_STATE) {
            shell_print_error("freq: no free PWM channel (stop one first)");
            shell_record_errorf("freq", error, "PWM channel pool exhausted");
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("freq: failed to generate %ld Hz on pin %ld (%s)",
                              freq, gpio_num, esp_err_to_name(error));
            shell_record_errorf("freq", error, "Square wave config failed on pin %ld", gpio_num);
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "freq:" SH_RST " square wave on " SH_LBL "pin=" SH_RST SH_NUM "%ld" SH_RST
                                 " at " SH_NUM "%ld Hz" SH_RST "\n", gpio_num, freq);
        return;
    }

    shell_print_usage("Usage: freq status | freq stop <pin> | freq <pin> <hz>");
    shell_record_warningf("freq", "Usage error for freq command");
}

/* ---- ADC reads on arbitrary pins ---- */

static void shell_adc_delete_cali(adc_cali_handle_t cali)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(cali);
#else
    (void)cali;
#endif
}

/**
 * Read an arbitrary ADC-capable GPIO with a fresh one-shot unit. The unit
 * and any calibration handle are deleted before returning, so repeated
 * reads never leak ADC resources.
 */
static esp_err_t shell_adc_read_pin(int gpio_num, int samples, int *mv_out, int *raw_out)
{
    esp_err_t error;
    adc_unit_t unit_id;
    adc_channel_t channel;
    adc_oneshot_unit_init_cfg_t unit_cfg = {0};
    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = (adc_atten_t)SHELL_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_unit_handle_t unit = NULL;
    adc_cali_handle_t cali = NULL;
    int64_t raw_sum = 0;
    int i;

    error = adc_oneshot_io_to_channel(gpio_num, &unit_id, &channel);
    if (error != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    unit_cfg.unit_id = unit_id;
    error = adc_oneshot_new_unit(&unit_cfg, &unit);
    if (error != ESP_OK) {
        return error;
    }

    error = adc_oneshot_config_channel(unit, channel, &channel_cfg);
    if (error != ESP_OK) {
        adc_oneshot_del_unit(unit);
        return error;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = unit_id,
            .chan = channel,
            .atten = (adc_atten_t)SHELL_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        (void)adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali);
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = unit_id,
            .atten = (adc_atten_t)SHELL_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        (void)adc_cali_create_scheme_line_fitting(&cali_cfg, &cali);
    }
#endif

    if (samples < 1) {
        samples = 1;
    }
    for (i = 0; i < samples; i++) {
        int raw = 0;
        error = adc_oneshot_read(unit, channel, &raw);
        if (error != ESP_OK) {
            break;
        }
        raw_sum += raw;
    }

    if (error != ESP_OK) {
        if (cali != NULL) {
            shell_adc_delete_cali(cali);
        }
        adc_oneshot_del_unit(unit);
        return error;
    }

    *raw_out = (int)(raw_sum / samples);
    if (cali != NULL) {
        if (adc_cali_raw_to_voltage(cali, *raw_out, mv_out) != ESP_OK) {
            *mv_out = (int)(((int64_t)*raw_out * 3300) / 4095);
        }
        shell_adc_delete_cali(cali);
    } else {
        *mv_out = (int)(((int64_t)*raw_out * 3300) / 4095);
    }

    adc_oneshot_del_unit(unit);
    return ESP_OK;
}

/**
 * List the ADC-capable, non-reserved GPIOs that currently read back a value.
 *
 * The candidate pins come from the SOC channel map (no io_to_channel probing
 * of arbitrary GPIOs, which would log spurious errors), and each candidate is
 * actually sampled once so a busy ADC unit or a pin claimed by another driver
 * is reported as unavailable instead of listed as usable.
 */
static void shell_adc_print_capable_pins(void)
{
    size_t index;
    int count = 0;

#if SOC_ADC_SUPPORTED
    static const int adc1_gpios[] = {
        ADC1_CHANNEL_0_GPIO_NUM, ADC1_CHANNEL_1_GPIO_NUM, ADC1_CHANNEL_2_GPIO_NUM,
        ADC1_CHANNEL_3_GPIO_NUM, ADC1_CHANNEL_4_GPIO_NUM, ADC1_CHANNEL_5_GPIO_NUM,
        ADC1_CHANNEL_6_GPIO_NUM, ADC1_CHANNEL_7_GPIO_NUM,
    };
    static const int adc2_gpios[] = {
        ADC2_CHANNEL_0_GPIO_NUM, ADC2_CHANNEL_1_GPIO_NUM, ADC2_CHANNEL_2_GPIO_NUM,
        ADC2_CHANNEL_3_GPIO_NUM, ADC2_CHANNEL_4_GPIO_NUM, ADC2_CHANNEL_5_GPIO_NUM,
    };

    for (index = 0; index < sizeof(adc1_gpios) / sizeof(adc1_gpios[0]); index++) {
        int mv;
        int raw;
        int gpio = adc1_gpios[index];
        if (shell_pin_is_reserved(gpio)) {
            continue;
        }
        if (shell_adc_read_pin(gpio, 1, &mv, &raw) == ESP_OK) {
            count++;
            shell_transcript_appendf_ansi(SH_LBL "adc.status:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST
                                     " " SH_LBL "unit=" SH_RST SH_NUM "1" SH_RST " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST
                                     " " SH_LBL "voltage=" SH_RST SH_NUM "%d.%03d V" SH_RST "\n",
                                     gpio, raw, mv / 1000, mv % 1000);
        }
    }
    for (index = 0; index < sizeof(adc2_gpios) / sizeof(adc2_gpios[0]); index++) {
        int mv;
        int raw;
        int gpio = adc2_gpios[index];
        if (shell_pin_is_reserved(gpio)) {
            continue;
        }
        if (shell_adc_read_pin(gpio, 1, &mv, &raw) == ESP_OK) {
            count++;
            shell_transcript_appendf_ansi(SH_LBL "adc.status:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST
                                     " " SH_LBL "unit=" SH_RST SH_NUM "2" SH_RST " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST
                                     " " SH_LBL "voltage=" SH_RST SH_NUM "%d.%03d V" SH_RST "\n",
                                     gpio, raw, mv / 1000, mv % 1000);
        }
    }
#endif

    if (count == 0) {
        shell_transcript_appendf_ansi(SH_LBL "adc.status:" SH_RST " " SH_MUTE "no usable ADC pins" SH_RST "\n");
    }
}

void shell_execute_adc_command(int argc, char **argv)
{
    char *end = NULL;
    long gpio_num;
    long samples = SHELL_ADC_DEFAULT_SAMPLES;
    int mv;
    int raw;
    esp_err_t error;

    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "status"))) {
        shell_adc_print_capable_pins();
        return;
    }

    if (argc == 2 || argc == 3) {
        gpio_num = strtol(argv[1], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: adc <pin> [samples]");
            shell_record_warningf("adc", "Usage error for adc pin argument");
            return;
        }
        if (argc == 3) {
            end = NULL;
            samples = strtol(argv[2], &end, 10);
            if (end == NULL || *end != '\0' || samples < 1 || samples > SHELL_ADC_MAX_SAMPLES) {
                shell_print_usage("Usage: adc <pin> [samples]  (samples 1..%d)", (int)SHELL_ADC_MAX_SAMPLES);
                shell_record_warningf("adc", "Invalid adc samples argument");
                return;
            }
        }

        if (shell_pin_is_reserved((int)gpio_num)) {
            shell_print_warning("adc: pin %ld is reserved for active board functions", gpio_num);
            shell_record_warningf("adc", "Rejected reserved pin %ld", gpio_num);
            return;
        }

        error = shell_adc_read_pin((int)gpio_num, (int)samples, &mv, &raw);
        if (error == ESP_ERR_NOT_SUPPORTED) {
            shell_print_error("adc: pin %ld is not an ADC-capable GPIO", gpio_num);
            shell_record_warningf("adc", "Pin %ld is not ADC-capable", gpio_num);
            return;
        }
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("adc: the ADC unit for pin %ld is in use by another driver", gpio_num);
            shell_record_warningf("adc", "ADC unit for pin %ld is busy", gpio_num);
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("adc: read failed on pin %ld (%s)", gpio_num, esp_err_to_name(error));
            shell_record_errorf("adc", error, "ADC read failed on pin %ld", gpio_num);
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "adc:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%ld" SH_RST
                                 " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "voltage=" SH_RST SH_NUM "%d.%03d V" SH_RST "\n",
                                 gpio_num, raw, mv / 1000, mv % 1000);
        return;
    }

    shell_print_usage("Usage: adc status | adc <pin> [samples]");
    shell_record_warningf("adc", "Usage error for adc command");
}

/* ---- I2C scan / peek / poke (i2c_master driver) ---- */

typedef struct {
    bool using_shared_bus;
    uint32_t clk_hz;
    i2c_master_bus_handle_t bus;
} shell_i2c_session_t;

/**
 * Open an I2C session. The board's shared bus (pins 7/8) is reused through
 * the BSP handle so a scan never conflicts with the touch controller; any
 * other pin pair gets a temporary master bus on a free port.
 */
static esp_err_t shell_i2c_open(int sda, int scl, shell_i2c_session_t *session)
{
    i2c_master_bus_config_t cfg;

    if (sda == (int)BSP_I2C_SDA && scl == (int)BSP_I2C_SCL) {
        session->bus = bsp_i2c_get_handle();
        if (session->bus != NULL) {
            session->using_shared_bus = true;
            session->clk_hz = (uint32_t)BOARD_CFG_I2C_CLK_SPEED_HZ;
            return ESP_OK;
        }
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)sda;
    cfg.scl_io_num = (gpio_num_t)scl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.trans_queue_depth = 1;
    cfg.flags.enable_internal_pullup = true;

    session->using_shared_bus = false;
    session->clk_hz = SHELL_I2C_TOOL_CLK_HZ;
    return i2c_new_master_bus(&cfg, &session->bus);
}

static void shell_i2c_close(shell_i2c_session_t *session)
{
    if (session->bus != NULL && !session->using_shared_bus) {
        i2c_del_master_bus(session->bus);
    }
    session->bus = NULL;
}

/** Parse optional `sda=` / `scl=` pin tokens; defaults to the board bus. */
static bool shell_i2c_parse_pins(int argc, char **argv, int *sda, int *scl)
{
    int i;

    *sda = (int)BSP_I2C_SDA;
    *scl = (int)BSP_I2C_SCL;

    for (i = 0; i < argc; i++) {
        if (argv[i] == NULL) {
            continue;
        }
        if (strncasecmp(argv[i], "sda=", 4) == 0) {
            char *end = NULL;
            long value = strtol(argv[i] + 4, &end, 10);
            if (end == NULL || *end != '\0' || value < 0) {
                return false;
            }
            *sda = (int)value;
        } else if (strncasecmp(argv[i], "scl=", 4) == 0) {
            char *end = NULL;
            long value = strtol(argv[i] + 4, &end, 10);
            if (end == NULL || *end != '\0' || value < 0) {
                return false;
            }
            *scl = (int)value;
        } else {
            return false;
        }
    }

    return *sda >= 0 && *scl >= 0;
}

/** The board's own I2C pins are the intended scan target; any other pair
 *  must be made of free (non-reserved) GPIOs. */
static bool shell_i2c_pins_allowed(int sda, int scl)
{
    bool is_shared = (sda == (int)BSP_I2C_SDA && scl == (int)BSP_I2C_SCL);

    return is_shared || (!shell_pin_is_reserved(sda) && !shell_pin_is_reserved(scl));
}

/**
 * Probe one 7-bit address for an ACK using a normal device transaction.
 *
 * Unlike i2c_master_probe(), this path never touches the shared controller's
 * bus timing or interrupt mask, so a scan cannot disrupt the GT911 touch that
 * runs on the same bus. A one-byte write to a device that NACKs the address
 * fails immediately; a device that ACKs the address replies with a NACK only
 * on the data byte, which the driver surfaces as a transaction error too, so
 * only addresses that fully ACK count as present.
 */
static bool shell_i2c_probe_address(const shell_i2c_session_t *session, int addr)
{
    i2c_master_dev_handle_t dev = NULL;
    uint8_t dummy = 0;
    esp_err_t error;

    error = i2c_master_bus_add_device(session->bus,
                                      &(i2c_device_config_t){
                                          .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                          .device_address = (uint16_t)addr,
                                          .scl_speed_hz = session->clk_hz,
                                      },
                                      &dev);
    if (error != ESP_OK) {
        return false;
    }

    error = i2c_master_transmit(dev, &dummy, 1, SHELL_I2C_SCAN_PROBE_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);
    return error == ESP_OK;
}

static void shell_i2c_print_scan(const shell_i2c_session_t *session)
{
    int addr;
    int found = 0;

    shell_transcript_appendf_ansi(SH_LBL "i2c.scan:" SH_RST " probing " SH_NUM "0x%02X..0x%02X" SH_RST "\n",
                             SHELL_I2C_SCAN_FIRST_ADDR, SHELL_I2C_SCAN_LAST_ADDR);
    for (addr = SHELL_I2C_SCAN_FIRST_ADDR; addr <= SHELL_I2C_SCAN_LAST_ADDR; addr++) {
        if (shell_i2c_probe_address(session, addr)) {
            found++;
            shell_transcript_appendf_ansi("  " SH_NUM "0x%02X" SH_RST " " SH_MUTE "(0x%02X with R/W bit)" SH_RST "\n",
                                     addr, (addr << 1));
        }
    }
    if (found == 0) {
        shell_transcript_appendf_ansi(SH_LBL "i2c.scan:" SH_RST " " SH_MUTE "no devices found" SH_RST "\n");
    } else {
        shell_transcript_appendf_ansi(SH_LBL "i2c.scan:" SH_RST " " SH_NUM "%d" SH_RST " " SH_LBL "device(s) found" SH_RST "\n", found);
    }
}

static esp_err_t shell_i2c_add_device(const shell_i2c_session_t *session, int addr,
                                      i2c_master_dev_handle_t *dev_out)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = session->clk_hz,
    };

    return i2c_master_bus_add_device(session->bus, &dev_cfg, dev_out);
}

static bool shell_parse_hex_long(const char *text, long *value_out)
{
    char *end = NULL;
    long value;

    if (text == NULL) {
        return false;
    }
    value = strtol(text, &end, 0);
    if (end == NULL || *end != '\0' || value < 0 || value > 0xFFFF) {
        return false;
    }
    *value_out = value;
    return true;
}

void shell_execute_i2c_command(int argc, char **argv)
{
    shell_i2c_session_t session = {0};
    esp_err_t error;
    int sda;
    int scl;
    long addr;
    long reg;
    long value;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        shell_transcript_appendf_ansi(SH_LBL "i2c.status:" SH_RST " " SH_LBL "shared bus" SH_RST " "
                                 SH_LBL "sda=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "scl=" SH_RST SH_NUM "%d" SH_RST
                                 " " SH_LBL "clk=" SH_RST SH_NUM "%u Hz" SH_RST " " SH_LBL "timeout=" SH_RST SH_NUM "%d ms" SH_RST "\n",
                                 (int)BSP_I2C_SDA, (int)BSP_I2C_SCL,
                                 (unsigned)BOARD_CFG_I2C_CLK_SPEED_HZ, (int)SHELL_I2C_TOOL_TIMEOUT_MS);
        return;
    }

    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "scan")) {
        if (!shell_i2c_parse_pins(argc - 2, argv + 2, &sda, &scl)) {
            shell_print_usage("Usage: i2c scan [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c scan");
            return;
        }
        if (!shell_i2c_pins_allowed(sda, scl)) {
            shell_print_warning("i2c: pin pair %d/%d includes a reserved board line", sda, scl);
            shell_record_warningf("i2c", "Rejected reserved scan pins %d/%d", sda, scl);
            return;
        }
        error = shell_i2c_open(sda, scl, &session);
        if (error != ESP_OK) {
            shell_print_error("i2c: bus open failed on %d/%d (%s)", sda, scl, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C bus open failed");
            return;
        }
        shell_i2c_print_scan(&session);
        shell_i2c_close(&session);
        return;
    }

    if (argc >= 4 && shell_text_equals_ignore_case(argv[1], "peek")) {
        i2c_master_dev_handle_t dev = NULL;
        uint8_t reg_byte;
        uint8_t data = 0;

        if (!shell_parse_hex_long(argv[2], &addr) || !shell_parse_hex_long(argv[3], &reg)) {
            shell_print_usage("Usage: i2c peek <addr> <reg> [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c peek");
            return;
        }
        if (addr < 0 || addr > 0x7F) {
            shell_print_usage("Usage: i2c peek <addr> <reg>  (addr 0x03..0x77)");
            shell_record_warningf("i2c", "i2c peek address out of range");
            return;
        }
        if (!shell_i2c_parse_pins(argc - 4, argv + 4, &sda, &scl) || !shell_i2c_pins_allowed(sda, scl)) {
            shell_print_usage("Usage: i2c peek <addr> <reg> [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c peek pins");
            return;
        }
        error = shell_i2c_open(sda, scl, &session);
        if (error != ESP_OK) {
            shell_print_error("i2c: bus open failed on %d/%d (%s)", sda, scl, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C bus open failed");
            return;
        }
        error = shell_i2c_add_device(&session, (int)addr, &dev);
        if (error == ESP_OK) {
            reg_byte = (uint8_t)reg;
            error = i2c_master_transmit_receive(dev, &reg_byte, 1, &data, 1, SHELL_I2C_TOOL_TIMEOUT_MS);
            i2c_master_bus_rm_device(dev);
        }
        shell_i2c_close(&session);

        if (error != ESP_OK) {
            shell_print_error("i2c peek: read failed at addr 0x%02lX reg 0x%02lX (%s)",
                              addr, reg, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C peek failed");
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "i2c.peek:" SH_RST " " SH_LBL "addr=" SH_RST SH_NUM "0x%02lX" SH_RST
                                 " " SH_LBL "reg=" SH_RST SH_NUM "0x%02lX" SH_RST " " SH_LBL "value=" SH_RST SH_NUM "0x%02X" SH_RST "\n",
                                 addr, reg, data);
        return;
    }

    if (argc >= 5 && shell_text_equals_ignore_case(argv[1], "poke")) {
        i2c_master_dev_handle_t dev = NULL;
        uint8_t buf[2];

        if (!shell_parse_hex_long(argv[2], &addr) || !shell_parse_hex_long(argv[3], &reg) ||
            !shell_parse_hex_long(argv[4], &value)) {
            shell_print_usage("Usage: i2c poke <addr> <reg> <value> [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c poke");
            return;
        }
        if (addr < 0 || addr > 0x7F || value > 0xFF) {
            shell_print_usage("Usage: i2c poke <addr> <reg> <value>  (addr 0x03..0x77, value 0x00..0xFF)");
            shell_record_warningf("i2c", "i2c poke argument out of range");
            return;
        }
        if (!shell_i2c_parse_pins(argc - 5, argv + 5, &sda, &scl) || !shell_i2c_pins_allowed(sda, scl)) {
            shell_print_usage("Usage: i2c poke <addr> <reg> <value> [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c poke pins");
            return;
        }
        error = shell_i2c_open(sda, scl, &session);
        if (error != ESP_OK) {
            shell_print_error("i2c: bus open failed on %d/%d (%s)", sda, scl, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C bus open failed");
            return;
        }
        error = shell_i2c_add_device(&session, (int)addr, &dev);
        if (error == ESP_OK) {
            buf[0] = (uint8_t)reg;
            buf[1] = (uint8_t)value;
            error = i2c_master_transmit(dev, buf, 2, SHELL_I2C_TOOL_TIMEOUT_MS);
            i2c_master_bus_rm_device(dev);
        }
        shell_i2c_close(&session);

        if (error != ESP_OK) {
            shell_print_error("i2c poke: write failed at addr 0x%02lX reg 0x%02lX (%s)",
                              addr, reg, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C poke failed");
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "i2c.poke:" SH_RST " " SH_LBL "addr=" SH_RST SH_NUM "0x%02lX" SH_RST
                                 " " SH_LBL "reg=" SH_RST SH_NUM "0x%02lX" SH_RST " " SH_LBL "value=" SH_RST SH_NUM "0x%02lX" SH_RST " " SH_OK "written" SH_RST "\n",
                                 addr, reg, value);
        return;
    }

    shell_print_usage("Usage: i2c status | i2c scan [sda=.. scl=..] | i2c peek <addr> <reg> [sda=.. scl=..] | i2c poke <addr> <reg> <value> [sda=.. scl=..]");
    shell_record_warningf("i2c", "Usage error for i2c command");
}

/* ---- SPI status (transactions unavailable on this board) ----
 * `spi status` reports the toolkit's SPI configuration. The loopback / peek /
 * poke verbs are deliberately NOT wired to the SPI master driver: initializing
 * the SPI host on this P4 with the ESP-Hosted SDIO link active stalls the chip
 * and drops USB-Serial-JTAG off the bus, so any SPI transaction command would
 * freeze the shell. They fail with an honest message instead, following the
 * rgb/camera unsupported-hardware pattern.
 */

static void shell_spi_print_unavailable(const char *verb)
{
    shell_print_error("spi: %s is unavailable on this board - initializing the SPI host "
                      "here stalls the chip (clock-domain conflict with the ESP-Hosted "
                      "SDIO link). `spi status` reports the toolkit configuration.",
                      verb);
    shell_record_warningf("spi", "SPI %s requested but SPI host init stalls this board", verb);
}

void shell_execute_spi_command(int argc, char **argv)
{
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        shell_transcript_appendf_ansi(SH_LBL "spi.status:" SH_RST " " SH_LBL "host" SH_RST " " SH_NUM "SPI3" SH_RST
                                 " " SH_LBL "mode" SH_RST " " SH_NUM "0" SH_RST " " SH_LBL "clk=" SH_RST SH_NUM "%u Hz" SH_RST
                                 " " SH_LBL "timeout=" SH_RST SH_NUM "%d ms" SH_RST "\n",
                                 (unsigned)SHELL_SPI_TOOL_CLK_HZ, (int)SHELL_SPI_TOOL_TIMEOUT_MS);
        shell_transcript_appendf_ansi(SH_MUTE "spi.transactions: " SH_ERR "unavailable on this board" SH_RST
                                 " - SPI host init stalls the chip with the ESP-Hosted SDIO link active.\n");
        return;
    }

    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "loopback")) {
        shell_spi_print_unavailable("loopback");
        return;
    }
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "peek")) {
        shell_spi_print_unavailable("peek");
        return;
    }
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "poke")) {
        shell_spi_print_unavailable("poke");
        return;
    }

    shell_print_usage("Usage: spi status | spi loopback <sclk> <mosi> <miso> | spi peek <sclk> <mosi> <miso> <cs> <reg> | spi poke <sclk> <mosi> <miso> <cs> <reg> <value>");
    shell_record_warningf("spi", "Usage error for spi command");
}


/* ========================================================================
 * UNSUPPORTED HARDWARE COMMAND (camera)
 * ======================================================================== */

/** Parse a hex colour "#RRGGBB" or "RRGGBB" into 0xRRGGBB. */
static bool shell_rgb_parse_hex(const char *text, uint32_t *rgb_out)
{
    const char *cursor = text;
    uint32_t value = 0;
    size_t digits = 0;

    if (text == NULL || rgb_out == NULL) {
        return false;
    }
    if (*cursor == '#') {
        cursor++;
    }
    while (digits < 6 && cursor[digits] != '\0') {
        char c = cursor[digits];
        int nibble;
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10;
        } else {
            return false;
        }
        value = (value << 4) | (uint32_t)nibble;
        digits++;
    }
    if (digits != 6 || cursor[digits] != '\0') {
        return false;
    }
    *rgb_out = value;
    return true;
}

/**
 * `rgb` � control the WS2812 status LED (LED1, GPIO26).
 *
 * Usage:
 *   rgb status                  Show state (mode, colour, effect, brightness)
 *   rgb off                     Turn the LED off
 *   rgb <r> <g> <b>             Solid colour, each channel 0-255
 *   rgb #RRGGBB                 Solid colour from a hex value
 *   rgb <effect> [speed]        rainbow | breath | pulse | blink (speed 1..10)
 *   rgb auto <on|off>           Enable/disable the status-driven colour layer
 *
 * ERRORLEVEL: 0 success, 1 failure, 2 usage. Works in batch files and is
 * redirectable/pipable like every other command.
 */
void shell_execute_rgb_command(int argc, char **argv)
{
    esp_err_t error = ESP_OK;

    /* rgb / rgb status */
    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "status"))) {
        led_state_t state;

        if (!led_is_initialized()) {
            shell_print_error("rgb: WS2812 LED driver is not initialized (check GPIO%d)",
                              (int)BOARD_CFG_RGB_LED_GPIO);
            shell_record_warningf("rgb", "RGB LED driver not initialized");
            batch_set_errorlevel(1);
            return;
        }
        led_get_state(&state);
        shell_transcript_appendf_ansi(SH_HEAD "RGB LED" SH_RST "\n");
        shell_transcript_appendf_ansi("  " SH_LBL "driver:" SH_RST " WS2812 on " SH_NUM "GPIO%d" SH_RST "\n",
                                      (int)BOARD_CFG_RGB_LED_GPIO);
        if (state.auto_status) {
            shell_transcript_appendf_ansi("  " SH_LBL "mode:" SH_RST " " SH_OK "auto status" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi("  " SH_LBL "mode:" SH_RST " " SH_VAL "manual" SH_RST "\n");
        }
        shell_transcript_appendf_ansi("  " SH_LBL "effect:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                      led_effect_name(state.effect));
        shell_transcript_appendf_ansi("  " SH_LBL "colour:" SH_RST " " SH_NUM "#%02X%02X%02X" SH_RST "\n",
                                      state.red, state.green, state.blue);
        shell_transcript_appendf_ansi("  " SH_LBL "speed:" SH_RST " " SH_NUM "%u" SH_RST " " SH_MUTE "(1..10)" SH_RST "\n",
                                      (unsigned int)state.speed);
        shell_transcript_appendf_ansi("  " SH_LBL "brightness:" SH_RST " " SH_NUM "%u%%" SH_RST "\n",
                                      (unsigned int)state.brightness_pct);
        batch_set_errorlevel(0);
        return;
    }

    /* rgb off */
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "off")) {
        error = led_off();
        if (error == ESP_OK) {
            shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " LED " SH_ERR "off" SH_RST "\n");
        }
        goto done;
    }

    /* rgb auto <on|off> */
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "auto")) {
        if (shell_text_equals_ignore_case(argv[2], "on")) {
            error = led_set_auto_status(true);
            if (error == ESP_OK) {
                shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " auto status " SH_OK "enabled" SH_RST "\n");
            }
            goto done;
        }
        if (shell_text_equals_ignore_case(argv[2], "off")) {
            error = led_set_auto_status(false);
            if (error == ESP_OK) {
                shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " auto status " SH_MUTE "disabled" SH_RST "\n");
            }
            goto done;
        }
        shell_print_usage("Usage: rgb auto <on|off>");
        shell_record_warningf("rgb", "Invalid rgb auto argument");
        batch_set_errorlevel(2);
        return;
    }

    /* rgb #RRGGBB */
    if (argc == 2 && argv[1][0] == '#') {
        uint32_t rgb;

        if (!shell_rgb_parse_hex(argv[1], &rgb)) {
            shell_print_usage("Usage: rgb #RRGGBB");
            shell_record_warningf("rgb", "Invalid rgb hex colour");
            batch_set_errorlevel(2);
            return;
        }
        error = led_set_hex(rgb);
        if (error == ESP_OK) {
            shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " colour set to " SH_NUM "#%06lX" SH_RST "\n",
                                          (unsigned long)rgb);
        }
        goto done;
    }

    /* rgb <r> <g> <b> */
    if (argc == 4) {
        char *end = NULL;
        long red = strtol(argv[1], &end, 10);
        if (end == NULL || *end != '\0' || red < 0 || red > 255) {
            shell_print_usage("Usage: rgb <r> <g> <b>  (0-255 each)");
            shell_record_warningf("rgb", "Invalid rgb red argument");
            batch_set_errorlevel(2);
            return;
        }
        end = NULL;
        long green = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || green < 0 || green > 255) {
            shell_print_usage("Usage: rgb <r> <g> <b>  (0-255 each)");
            shell_record_warningf("rgb", "Invalid rgb green argument");
            batch_set_errorlevel(2);
            return;
        }
        end = NULL;
        long blue = strtol(argv[3], &end, 10);
        if (end == NULL || *end != '\0' || blue < 0 || blue > 255) {
            shell_print_usage("Usage: rgb <r> <g> <b>  (0-255 each)");
            shell_record_warningf("rgb", "Invalid rgb blue argument");
            batch_set_errorlevel(2);
            return;
        }
        error = led_set_color((uint8_t)red, (uint8_t)green, (uint8_t)blue);
        if (error == ESP_OK) {
            shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " colour set to " SH_NUM "r=%ld g=%ld b=%ld" SH_RST "\n",
                                          red, green, blue);
        }
        goto done;
    }

    /* rgb <effect> [speed] */
    if (argc == 2 || argc == 3) {
        led_effect_t effect = led_effect_from_name(argv[1]);
        uint8_t speed = 0;

        if (effect == LED_EFFECT_SOLID && !shell_text_equals_ignore_case(argv[1], "solid")) {
            shell_print_usage("Usage: rgb <effect> [speed]  (rainbow|breath|pulse|blink|solid)");
            shell_record_warningf("rgb", "Unknown rgb effect %s", argv[1]);
            batch_set_errorlevel(2);
            return;
        }
        if (argc == 3) {
            char *end = NULL;
            long parsed = strtol(argv[2], &end, 10);
            if (end == NULL || *end != '\0' || parsed < 1 || parsed > 10) {
                shell_print_usage("Usage: rgb <effect> [speed]  (speed 1..10)");
                shell_record_warningf("rgb", "Invalid rgb speed argument");
                batch_set_errorlevel(2);
                return;
            }
            speed = (uint8_t)parsed;
        }
        error = led_set_effect(effect, speed);
        if (error == ESP_OK) {
            shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " effect " SH_NUM "%s" SH_RST
                                          " at speed " SH_NUM "%u" SH_RST "\n",
                                          led_effect_name(effect),
                                          (unsigned int)(speed ? speed : P4_CONFIG_LED_EFFECT_SPEED_DEFAULT));
        }
        goto done;
    }

    shell_print_usage("Usage: rgb status | rgb off | rgb <r> <g> <b> | rgb #RRGGBB | rgb <effect> [speed] | rgb auto <on|off>");
    shell_record_warningf("rgb", "Usage error for rgb command");
    batch_set_errorlevel(2);
    return;

done:
    if (error != ESP_OK) {
        shell_print_error("rgb: LED control failed (%s)", esp_err_to_name(error));
        shell_record_errorf("rgb", error, "RGB LED control failed");
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
}

void shell_execute_camera_command(int argc, char **argv)
{
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "init")) {
        shell_print_error("camera: unsupported because the JC1060 reference repo shows a camera add-on path, but this workspace still does not declare the sensor, CSI pin map, or local esp_video camera stack needed to initialize it");
        shell_record_warningf("camera", "Camera init requested without camera metadata in the workspace");
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "snap")) {
        shell_transcript_appendf("camera: snap unavailable for %s because the workspace still lacks the declared sensor, CSI pin map, and local camera stack that the JC1060 examples depend on\n",
                                 argv[2]);
        shell_record_warningf("camera", "Camera snap requested without camera metadata in the workspace");
        return;
    }

    shell_print_usage("Usage: camera init | camera snap <filename>");
    shell_record_warningf("camera", "Usage error for camera command");
}
