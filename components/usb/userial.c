/**
 * @file userial.c
 * @brief USB-Host CDC-ACM serial driver (`usb userial`, verbs in command/).
 *
 * The wire side of the palmtop Comm story (`tcpterm` is the Wi-Fi side):
 * GPS pucks, microcontrollers, scopes, and serial consoles that present a
 * CDC-ACM / virtual-COM interface. This file is a LEAF driver only — it owns
 * the class-driver install, one open device, and a mutex-guarded RX ring.
 * All verbs, key-queue pumping, and SD input sourcing live in
 * `components/command/userial_commands.c`, which reaches this byte API
 * through usb.h (never the reverse).
 *
 * One open device at a time (the normal bench shape; hub-fanout enumeration
 * is out of scope). RX lands in the ring from the driver's data callback;
 * `userial_read` drains it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "usb/cdc_acm_host.h"
#include "usb.h"
#include "p4minishell_config.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

typedef struct {
    bool driver_installed;
    bool open;
    bool link_lost;
    cdc_acm_dev_hdl_t handle;
    uint16_t vid;
    uint16_t pid;
    userial_coding_t coding;
    uint8_t ring[P4_CONFIG_USERIAL_RING_BYTES];
    size_t head;
    size_t tail;
    uint32_t dropped;
    SemaphoreHandle_t lock;
} userial_state_t;

static userial_state_t s_userial;

/* ========================================================================
 * Driver install (idempotent stage, called from usb_install_host_stack)
 * ======================================================================== */

esp_err_t userial_install_driver(void)
{
#if SOC_USB_OTG_SUPPORTED
    cdc_acm_host_driver_config_t config = {
        .driver_task_stack_size = P4_CONFIG_USB_DRIVER_TASK_STACK,
        .driver_task_priority = 5,
        .xCoreID = 0,
        .new_dev_cb = NULL,
    };
    esp_err_t error;

    if (s_userial.driver_installed) {
        return ESP_OK;
    }
    error = cdc_acm_host_install(&config);
    if (error != ESP_OK) {
        return error;
    }
    if (s_userial.lock == NULL) {
        s_userial.lock = xSemaphoreCreateMutex();
        if (s_userial.lock == NULL) {
            (void)cdc_acm_host_uninstall();
            return ESP_ERR_NO_MEM;
        }
    }
    s_userial.driver_installed = true;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ========================================================================
 * Pure parsers (unit-tested, no hardware)
 * ======================================================================== */

bool userial_parse_id(const char *text, uint16_t *vid_out, uint16_t *pid_out)
{
    char *end = NULL;
    unsigned long vid;
    unsigned long pid;
    const char *colon;

    if (vid_out != NULL) {
        *vid_out = 0;
    }
    if (pid_out != NULL) {
        *pid_out = 0;
    }
    if (text == NULL || vid_out == NULL || pid_out == NULL) {
        return false;
    }
    colon = strchr(text, ':');
    if (colon == NULL || colon == text || *(colon + 1) == '\0' ||
        strchr(colon + 1, ':') != NULL) {
        return false;
    }
    vid = strtoul(text, &end, 16);
    if (end != colon || vid > 0xFFFF) {
        return false;
    }
    pid = strtoul(colon + 1, &end, 16);
    if (end == colon + 1 || *end != '\0' || pid > 0xFFFF) {
        return false;
    }
    *vid_out = (uint16_t)vid;
    *pid_out = (uint16_t)pid;
    return true;
}

bool userial_parse_coding(const char *baud_str, const char *data_str,
                          const char *parity_str, const char *stop_str,
                          userial_coding_t *out)
{
    char *end = NULL;
    unsigned long baud;
    unsigned long data;
    unsigned long stop;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->baud = 115200;
    out->data_bits = 8;
    out->stop_bits = 1;

    if (baud_str != NULL) {
        baud = strtoul(baud_str, &end, 10);
        if (end == baud_str || *end != '\0' || baud < 300 || baud > 3000000) {
            return false;
        }
        out->baud = (uint32_t)baud;
    }
    if (data_str != NULL) {
        data = strtoul(data_str, &end, 10);
        if (end == data_str || *end != '\0' || data < 5 || data > 8) {
            return false;
        }
        out->data_bits = (uint8_t)data;
    }
    if (parity_str != NULL) {
        if (parity_str[0] == '\0' || parity_str[1] != '\0') {
            return false;
        }
        switch (toupper((unsigned char)parity_str[0])) {
        case 'N':
            out->parity = 0;
            break;
        case 'O':
            out->parity = 1;
            break;
        case 'E':
            out->parity = 2;
            break;
        default:
            return false;
        }
    }
    if (stop_str != NULL) {
        stop = strtoul(stop_str, &end, 10);
        if (end == stop_str || *end != '\0' || (stop != 1 && stop != 2)) {
            return false;
        }
        out->stop_bits = (uint8_t)stop;
    }
    return true;
}

/** Map the driver-free coding into the CDC driver's line-coding struct. */
static void userial_coding_to_cdc(const userial_coding_t *in, cdc_acm_line_coding_t *out)
{
    out->dwDTERate = in->baud;
    out->bDataBits = in->data_bits;
    out->bParityType = in->parity;
    out->bCharFormat = (in->stop_bits == 1) ? 0 : 2;
}

/* ========================================================================
 * RX ring (driver callback context -> verb context)
 * ======================================================================== */

static bool userial_data_cb(const uint8_t *data, size_t data_len, void *user_arg)
{
    size_t i;

    (void)user_arg;
    if (data == NULL || data_len == 0 || s_userial.lock == NULL) {
        return true;
    }
    if (xSemaphoreTake(s_userial.lock, 0) != pdTRUE) {
        /* Ring busy: ask the driver to keep the buffer so it is not lost. */
        return false;
    }
    for (i = 0; i < data_len; i++) {
        size_t next = (s_userial.head + 1) % sizeof(s_userial.ring);
        if (next == s_userial.tail) {
            s_userial.dropped++;
            continue;
        }
        s_userial.ring[s_userial.head] = data[i];
        s_userial.head = next;
    }
    xSemaphoreGive(s_userial.lock);
    /* Returning true lets the driver flush its buffer (we copied). */
    return true;
}

static void userial_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event != NULL && event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED &&
        s_userial.lock != NULL) {
        if (xSemaphoreTake(s_userial.lock, 0) == pdTRUE) {
            s_userial.link_lost = true;
            xSemaphoreGive(s_userial.lock);
        }
    }
}

/* ========================================================================
 * Byte API (called from components/command/userial_commands.c)
 * ======================================================================== */

bool userial_is_open(void)
{
    bool open = false;

    if (s_userial.lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_userial.lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    open = s_userial.open && !s_userial.link_lost;
    xSemaphoreGive(s_userial.lock);
    return open;
}

bool userial_link_lost(void)
{
    bool lost = false;

    if (s_userial.lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_userial.lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    lost = s_userial.link_lost;
    xSemaphoreGive(s_userial.lock);
    return lost;
}

bool userial_get_status(userial_status_t *out)
{
    if (out == NULL || s_userial.lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_userial.lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    out->open = s_userial.open;
    out->link_lost = s_userial.link_lost;
    out->vid = s_userial.vid;
    out->pid = s_userial.pid;
    out->coding = s_userial.coding;
    out->dropped = s_userial.dropped;
    if (s_userial.head >= s_userial.tail) {
        out->waiting = s_userial.head - s_userial.tail;
    } else {
        out->waiting = sizeof(s_userial.ring) - s_userial.tail + s_userial.head;
    }
    xSemaphoreGive(s_userial.lock);
    return true;
}

int userial_open(uint16_t vid, uint16_t pid, const userial_coding_t *coding)
{
    cdc_acm_line_coding_t cdc_coding;
    cdc_acm_host_device_config_t dev_config;
    cdc_acm_dev_hdl_t handle = NULL;
    esp_err_t error;

    if (coding == NULL) {
        return 1;
    }
    /* Lazy install: the CDC class driver is brought up on first open so boot
     * never pays for it (see the note in usb_install_host_stack). */
    if (!s_userial.driver_installed) {
        if (userial_install_driver() != ESP_OK) {
            return 1;
        }
    }
    if (s_userial.lock == NULL) {
        return 1;
    }
    if (xSemaphoreTake(s_userial.lock, portMAX_DELAY) != pdTRUE) {
        return 1;
    }
    if (s_userial.open) {
        xSemaphoreGive(s_userial.lock);
        return 1;
    }
    xSemaphoreGive(s_userial.lock);

    userial_coding_to_cdc(coding, &cdc_coding);
    /* The open blocks until a matching device appears or the connection
     * timeout expires, so the worker is never hung. */
    memset(&dev_config, 0, sizeof(dev_config));
    dev_config.connection_timeout_ms = P4_CONFIG_USERIAL_OPEN_TIMEOUT_MS;
    dev_config.out_buffer_size = P4_CONFIG_USERIAL_CHUNK_BYTES;
    dev_config.in_buffer_size = P4_CONFIG_USERIAL_CHUNK_BYTES;
    dev_config.event_cb = userial_event_cb;
    dev_config.data_cb = userial_data_cb;
    error = cdc_acm_host_open(vid, pid, 0, &dev_config, &handle);
    if (error != ESP_OK || handle == NULL) {
        return 1;
    }
    if (cdc_acm_host_line_coding_set(handle, &cdc_coding) != ESP_OK ||
        cdc_acm_host_set_control_line_state(handle, true, true) != ESP_OK) {
        (void)cdc_acm_host_close(handle);
        return 1;
    }
    if (xSemaphoreTake(s_userial.lock, portMAX_DELAY) != pdTRUE) {
        (void)cdc_acm_host_close(handle);
        return 1;
    }
    s_userial.open = true;
    s_userial.handle = handle;
    s_userial.vid = vid;
    s_userial.pid = pid;
    s_userial.coding = *coding;
    s_userial.link_lost = false;
    s_userial.head = 0;
    s_userial.tail = 0;
    s_userial.dropped = 0;
    xSemaphoreGive(s_userial.lock);
    return 0;
}

void userial_close(void)
{
    if (s_userial.lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_userial.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_userial.open) {
        (void)cdc_acm_host_close(s_userial.handle);
        s_userial.open = false;
    }
    s_userial.link_lost = false;
    s_userial.head = 0;
    s_userial.tail = 0;
    xSemaphoreGive(s_userial.lock);
}

int userial_write(const uint8_t *data, size_t len)
{
    cdc_acm_dev_hdl_t handle;
    int rc;

    if (data == NULL || len == 0 || s_userial.lock == NULL) {
        return 1;
    }
    if (xSemaphoreTake(s_userial.lock, portMAX_DELAY) != pdTRUE) {
        return 1;
    }
    if (!s_userial.open || s_userial.link_lost) {
        xSemaphoreGive(s_userial.lock);
        return 1;
    }
    handle = s_userial.handle;
    xSemaphoreGive(s_userial.lock);
    rc = (cdc_acm_host_data_tx_blocking(handle, data, len,
                                        P4_CONFIG_USERIAL_OP_TIMEOUT_MS) == ESP_OK) ? 0 : 1;
    return rc;
}

size_t userial_read(uint8_t *out, size_t max)
{
    size_t n = 0;

    if (out == NULL || max == 0 || s_userial.lock == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_userial.lock, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    while (n < max && s_userial.tail != s_userial.head) {
        out[n++] = s_userial.ring[s_userial.tail];
        s_userial.tail = (s_userial.tail + 1) % sizeof(s_userial.ring);
    }
    xSemaphoreGive(s_userial.lock);
    return n;
}
