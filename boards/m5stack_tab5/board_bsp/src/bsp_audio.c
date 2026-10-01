/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include "esp_err.h"
#include "esp_check.h"
#include "bsp_err_check.h"
#include "esp_codec_dev_defaults.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "M5Stack Tab5";

/* Can be used for i2s_std_gpio_config_t and/or i2s_std_config_t initialization */
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

/* This configuration is used by default in bsp_audio_init() */
#define BSP_I2S_DUPLEX_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL;  /* Codec data interface */

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    esp_err_t ret = ESP_FAIL;
    if (i2s_tx_chan && i2s_rx_chan) {
        /* Audio was initialized before */
        return ESP_OK;
    }

    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_CFG_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    BSP_ERROR_CHECK_RETURN_ERR(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_CFG(48000);
    const i2s_std_config_t *p_i2s_cfg = &std_cfg_default;
    if (i2s_config != NULL) {
        p_i2s_cfg = i2s_config;
    }
    if (i2s_tx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_chan, p_i2s_cfg), err, TAG, "I2S channel initialization failed");
    }
    if (i2s_rx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_chan, p_i2s_cfg), err, TAG, "I2S channel initialization failed");
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_CFG_I2S_PORT,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    BSP_NULL_CHECK_GOTO(i2s_data_if, err);

    return ESP_OK;

err:
    if (i2s_tx_chan) {
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = NULL;
    }
    if (i2s_rx_chan) {
        i2s_del_channel(i2s_rx_chan);
        i2s_rx_chan = NULL;
    }

    return ret;
}

const audio_codec_data_if_t *bsp_audio_get_codec_itf(void)
{
    return i2s_data_if;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    const audio_codec_data_if_t *i2s_data_if = bsp_audio_get_codec_itf();
    if (i2s_data_if == NULL) {

        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
        i2s_data_if = bsp_audio_get_codec_itf();
    }
    assert(i2s_data_if);
    /* The NS4150B amp (SPK_EN on 0x43 P1) stays OFF here by design. The audio
     * layer gates it around the playback window only (enable after
     * esp_codec_dev_open, mute before close): latching it on at creation
     * amplified the clockless ES8388 idle into constant white noise from
     * boot (v1.3.0 regression). See components/audio/audio.c. */

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8388_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8388_codec_cfg_t codec_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *dev = es8388_codec_new(&codec_cfg);
    BSP_NULL_CHECK(dev, NULL);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

/**
 * @brief Read the 3.5 mm headphone-detect line.
 *
 * HP_DET lives on PI4IOE5V6408 #1 (0x43) pin P7 (BOARD_CFG_HP_DET_EXP_PIN),
 * active-high when a plug is inserted. Read at command/playback time through
 * the managed first-expander handle; the pin is set to input with no internal
 * pull (M5 drives it externally, so this disturbs nothing else on the chip).
 * Never touches the 0x44 expander (see the F6 rule).
 */
esp_err_t bsp_audio_headphone_detected(bool *inserted_out)
{
    esp_io_expander_handle_t io;
    uint32_t level = 0;
    esp_err_t err;

    if (inserted_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *inserted_out = false;
#if !BOARD_CFG_HP_DET_PRESENT
    return ESP_ERR_NOT_SUPPORTED;
#else
    io = bsp_io_expander_init();
    if (io == NULL) {
        return ESP_FAIL;
    }
    err = esp_io_expander_set_dir(io, (1u << BOARD_CFG_HP_DET_EXP_PIN), IO_EXPANDER_INPUT);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_io_expander_get_level(io, (1u << BOARD_CFG_HP_DET_EXP_PIN), &level);
    if (err != ESP_OK) {
        return err;
    }
    *inserted_out = ((level >> BOARD_CFG_HP_DET_EXP_PIN) & 1u) != 0;
    return ESP_OK;
#endif
}

/**
 * @brief Enable/disable the speaker power amplifier.
 *
 * Thin wrapper over the existing BSP_FEATURE_SPEAKER path (NS4150B SPK_EN on
 * 0x43 P1); kept as a named entry point so components/audio never drives
 * expander pins directly.
 */
esp_err_t bsp_audio_speaker_enable(bool enable)
{
    return bsp_feature_enable(BSP_FEATURE_SPEAKER, enable);
}

/* ES8388 DAC bring-up (F28). The vendored codec driver leaves this Tab5's DAC
 * silent, so after every open the registers are programmed with M5Unified's
 * proven Tab5 speaker sequence (reg: M5 value): reset dance on CONTROL1,
 * CONTROL2, CHIPPOWER, ADCPOWER, DACPOWER, VSEL, slave mode, I2S format, DAC
 * unmute, DAC volumes, click-free, mixer selects, DAC mixers, separate LRCK,
 * VROI and the LOUT volumes. The single deliberate difference is DACCONTROL2,
 * kept at the driver's 256 MCLK ratio to match this firmware's I2S clocks
 * (M5 runs its own clock tree at 128). Written through a cached I2C control
 * handle: this driver's codec interface exposes no set_reg/get_reg, so
 * esp_codec_dev_write_reg() cannot reach these registers. */
static const audio_codec_ctrl_if_t *s_es8388_fixup_ctrl;

static esp_err_t bsp_es8388_fixup_ctrl_ensure(void)
{
    if (s_es8388_fixup_ctrl == NULL) {
        audio_codec_i2c_cfg_t i2c_cfg = {
            .port = BSP_I2C_NUM,
            .addr = ES8388_CODEC_DEFAULT_ADDR,
            .bus_handle = bsp_i2c_get_handle(),
        };
        s_es8388_fixup_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
        if (s_es8388_fixup_ctrl == NULL) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t bsp_es8388_fixup_write(uint8_t reg, uint8_t val)
{
    int ret;

    if (bsp_es8388_fixup_ctrl_ensure() != ESP_OK) {
        return ESP_FAIL;
    }
    ret = s_es8388_fixup_ctrl->write_reg(s_es8388_fixup_ctrl, reg, 1,
                                         &val, 1);
    return (ret == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}

/** Dump ES8388 registers [0x00..0x31] into @p out (50 bytes). */
esp_err_t bsp_audio_codec_speaker_dump_regs(uint8_t out[50])
{
    uint8_t reg;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bsp_es8388_fixup_ctrl_ensure() != ESP_OK) {
        return ESP_FAIL;
    }
    for (reg = 0; reg < 50; reg++) {
        uint8_t val = 0;

        if (s_es8388_fixup_ctrl->read_reg(s_es8388_fixup_ctrl, reg, 1,
                                          &val, 1) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
        out[reg] = val;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_codec_speaker_post_open(esp_codec_dev_handle_t dev)
{
    /* M5Unified Tab5 speaker sequence (reg, value); DACCONTROL2 stays 0x02
     * (256x, matching our I2S MCLK) instead of M5's 0x00 (128x). */
    static const uint8_t seq[][2] = {
        { 0x00, 0x80 }, { 0x00, 0x00 }, { 0x00, 0x0e },
        { 0x01, 0x00 }, { 0x02, 0x0a }, { 0x03, 0xff }, { 0x04, 0x3c },
        { 0x07, 0x7c }, { 0x08, 0x00 }, { 0x17, 0x18 }, { 0x18, 0x02 },
        { 0x19, 0x20 }, { 0x1a, 0x00 }, { 0x1b, 0x00 }, { 0x1c, 0x08 },
        { 0x1d, 0x00 }, { 0x26, 0x00 }, { 0x27, 0xb8 }, { 0x2a, 0xb8 },
        { 0x2b, 0x08 }, { 0x2d, 0x00 }, { 0x2e, 0x21 }, { 0x2f, 0x21 },
        { 0x30, 0x21 }, { 0x31, 0x21 },
    };
    uint8_t readback = 0xff;
    size_t i;
    (void)dev;

    for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        if (bsp_es8388_fixup_write(seq[i][0], seq[i][1]) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    /* Read back the clock select so the board log proves the fixup stuck. */
    if (s_es8388_fixup_ctrl->read_reg(s_es8388_fixup_ctrl, 0x2b, 1,
                                      &readback, 1) != ESP_CODEC_DEV_OK ||
        readback != 0x08) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    const audio_codec_data_if_t *i2s_data_if = bsp_audio_get_codec_itf();
    if (i2s_data_if == NULL) {

        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
        i2s_data_if = bsp_audio_get_codec_itf();
    }
    assert(i2s_data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    es7210_codec_cfg_t codec_cfg = {
        .ctrl_if = i2c_ctrl_if,
    };
    const audio_codec_if_t *dev = es7210_codec_new(&codec_cfg);
    BSP_NULL_CHECK(dev, NULL);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}
