/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "bsp_err_check.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_interface.h"

#include "bsp/m5stack_tab5.h"

static const char *TAG = "M5Stack Tab5";
static sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL; //SD power control handle
static esp_ldo_channel_handle_t sd_ldo_chan = NULL; //SD rail (LDO_VO4)
static sdmmc_card_t *bsp_sdcard = NULL;    // Global uSD card handler
static bool spi_sd_initialized = false;

/*
 * microSD rail power control over on-chip LDO channel 4 (LDO_VO4 -> SD_VDD).
 *
 * The stock `sd_pwr_ctrl_new_on_chip_ldo()` acquires the channel with voltage
 * 0 and adjustable=true, which makes the LDO driver log "The voltage value 0 is
 * out of the recommended range [500, 2700]" on every boot before the SDMMC
 * stack sets the real card IO voltage. To avoid that spurious boot warning the
 * BSP owns the channel directly: it acquires LDO_VO4 once at the card's 3.3 V
 * IO level and exposes the standard `sd_pwr_ctrl_drv_t` interface, so the SDMMC
 * stack still switches IO voltage through `set_io_voltage` (SDR50/SDR104) but no
 * zero-voltage acquire ever happens.
 */
typedef struct {
    esp_ldo_channel_handle_t chan;
    int voltage_mv;
} bsp_sd_ldo_ctx_t;

static bsp_sd_ldo_ctx_t sd_ldo_ctx;

static esp_err_t bsp_sd_ldo_set_voltage(void *arg, int voltage_mv)
{
    bsp_sd_ldo_ctx_t *ctx = (bsp_sd_ldo_ctx_t *)arg;

    ESP_RETURN_ON_FALSE(ctx != NULL, ESP_ERR_INVALID_ARG, TAG, "null SD LDO ctx");
    ESP_RETURN_ON_ERROR(esp_ldo_channel_adjust_voltage(ctx->chan, voltage_mv), TAG,
                        "failed to set SD LDO voltage");
    ctx->voltage_mv = voltage_mv;
    return ESP_OK;
}

/* Acquire LDO_VO4 at the given IO voltage and return the SD power-control
 * handle. The caller must NOT also call sd_pwr_ctrl_new_on_chip_ldo(). */
static esp_err_t bsp_sd_ldo_pwr_ctrl_new(int voltage_mv, sd_pwr_ctrl_handle_t *out)
{
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "null out");

    if (pwr_ctrl_handle != NULL) {
        *out = pwr_ctrl_handle;
        return ESP_OK;
    }
    if (sd_ldo_chan == NULL) {
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = BOARD_CFG_SD_PWR_LDO_CHAN,
            .voltage_mv = voltage_mv,
            .flags.adjustable = true,
        };
        ret = esp_ldo_acquire_channel(&ldo_cfg, &sd_ldo_chan);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to acquire SD LDO channel");
            return ret;
        }
        sd_ldo_ctx.chan = sd_ldo_chan;
        sd_ldo_ctx.voltage_mv = voltage_mv;
    }
    pwr_ctrl_handle = (sd_pwr_ctrl_handle_t)calloc(1, sizeof(sd_pwr_ctrl_drv_t));
    ESP_RETURN_ON_FALSE(pwr_ctrl_handle != NULL, ESP_ERR_NO_MEM, TAG, "no mem for SD pwr ctrl");
    pwr_ctrl_handle->set_io_voltage = bsp_sd_ldo_set_voltage;
    pwr_ctrl_handle->ctx = &sd_ldo_ctx;
    *out = pwr_ctrl_handle;
    return ESP_OK;
}

static void bsp_sd_ldo_pwr_ctrl_del(void)
{
    if (pwr_ctrl_handle != NULL) {
        free(pwr_ctrl_handle);
        pwr_ctrl_handle = NULL;
    }
    if (sd_ldo_chan != NULL) {
        esp_ldo_release_channel(sd_ldo_chan);
        sd_ldo_chan = NULL;
    }
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BOARD_CFG_SPIFFS_MOUNT_POINT,
        .partition_label = BOARD_CFG_SPIFFS_PARTITION_LABEL,
        .max_files = BOARD_CFG_SPIFFS_MAX_FILES,
#if BOARD_CFG_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(BOARD_CFG_SPIFFS_PARTITION_LABEL);
}

sdmmc_card_t *bsp_sdcard_get_handle(void)
{
    return bsp_sdcard;
}

void bsp_sdcard_get_sdmmc_host(const int slot, sdmmc_host_t *config)
{
    assert(config);
    sdmmc_host_t host_config = SDMMC_HOST_DEFAULT();
    host_config.slot = slot;
    host_config.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    memcpy(config, &host_config, sizeof(sdmmc_host_t));
}

void bsp_sdcard_get_sdspi_host(const int slot, sdmmc_host_t *config)
{
    assert(config);
    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = slot;

    memcpy(config, &host_config, sizeof(sdmmc_host_t));
}

void bsp_sdcard_sdmmc_get_slot(const int slot, sdmmc_slot_config_t *config)
{
    assert(config);
    memset(config, 0, sizeof(sdmmc_slot_config_t));
    /* SD card is connected to Slot 0 pins. Slot 0 uses IO MUX, so not specifying the pins here */
    config->cd = SDMMC_SLOT_NO_CD;
    config->wp = SDMMC_SLOT_NO_WP;
    config->cmd = BSP_SD_CMD;
    config->clk = BSP_SD_CLK;
    config->d0 = BSP_SD_D0;
    config->d1 = BSP_SD_D1;
    config->d2 = BSP_SD_D2;
    config->d3 = BSP_SD_D3;
    config->width = 4;
    config->flags = 0;
}

void bsp_sdcard_sdspi_get_slot(const spi_host_device_t spi_host, sdspi_device_config_t *config)
{
    assert(config);
    memset(config, 0, sizeof(sdspi_device_config_t));
    config->gpio_cs   = BSP_SD_SPI_CS;
    config->gpio_cd   = SDSPI_SLOT_NO_CD;
    config->gpio_wp   = SDSPI_SLOT_NO_WP;
    config->gpio_int  = GPIO_NUM_NC;
    config->host_id = spi_host;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    config->gpio_wp_polarity = SDSPI_IO_ACTIVE_LOW;
#endif
}

esp_err_t bsp_sdcard_sdmmc_mount(bsp_sdcard_cfg_t *cfg)
{
    sdmmc_host_t sdhost = {0};
    sdmmc_slot_config_t sdslot = {0};
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#if BOARD_CFG_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    assert(cfg);

    if (!cfg->mount) {
        cfg->mount = &mount_config;
    }

    if (!cfg->host) {
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &sdhost);
        cfg->host = &sdhost;
    }

    if (!cfg->slot.sdmmc) {
        bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &sdslot);
        cfg->slot.sdmmc = &sdslot;
    }

    esp_err_t ret = bsp_sd_ldo_pwr_ctrl_new(BOARD_CFG_SD_PWR_LDO_VOLTAGE_MV, &cfg->host->pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create the SD power control driver");
        return ret;
    }

#if defined(CONFIG_FATFS_LFN_NONE)
    ESP_LOGW(TAG, "Warning: Long filenames on SD card are disabled in menuconfig!");
#endif

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, cfg->host, cfg->slot.sdmmc, cfg->mount, &bsp_sdcard);
}

esp_err_t bsp_sdcard_sdspi_mount(bsp_sdcard_cfg_t *cfg)
{
    sdmmc_host_t sdhost = {0};
    sdspi_device_config_t sdslot = {0};
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#if BOARD_CFG_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    assert(cfg);
    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num     = BSP_SD_SPI_CLK,
        .mosi_io_num     = BSP_SD_SPI_MOSI,
        .miso_io_num     = BSP_SD_SPI_MISO,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = 4000,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_SDSPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");
    spi_sd_initialized = true;

    if (!cfg->mount) {
        cfg->mount = &mount_config;
    }

    if (!cfg->host) {
        bsp_sdcard_get_sdspi_host(SDMMC_HOST_SLOT_0, &sdhost);
        cfg->host = &sdhost;
    }

    if (!cfg->slot.sdspi) {
        bsp_sdcard_sdspi_get_slot(BSP_SDSPI_HOST, &sdslot);
        cfg->slot.sdspi = &sdslot;
    }

    esp_err_t ret = bsp_sd_ldo_pwr_ctrl_new(BOARD_CFG_SD_PWR_LDO_VOLTAGE_MV, &cfg->host->pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create the SD power control driver");
        return ret;
    }

#if defined(CONFIG_FATFS_LFN_NONE)
    ESP_LOGW(TAG, "Warning: Long filenames on SD card are disabled in menuconfig!");
#endif

    return esp_vfs_fat_sdspi_mount(BSP_SD_MOUNT_POINT, cfg->host, cfg->slot.sdspi, cfg->mount, &bsp_sdcard);
}

esp_err_t bsp_sdcard_mount(void)
{
    bsp_sdcard_cfg_t cfg = {0};
    return bsp_sdcard_sdmmc_mount(&cfg);
}

esp_err_t bsp_sdcard_unmount(void)
{
    esp_err_t ret = ESP_OK;

    /* Releases our power-control shim and the LDO channel it owns. */
    bsp_sd_ldo_pwr_ctrl_del();

    ret |= esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
    bsp_sdcard = NULL;
    if (spi_sd_initialized) {
        ret |= spi_bus_free(BSP_SDSPI_HOST);
        spi_sd_initialized = false;
    }

    return ret;
}
