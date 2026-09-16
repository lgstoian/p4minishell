/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file netbench.h
 * @brief Minimal TCP/UDP throughput probe for the Wi-Fi link.
 *
 * Device side of `tools/wifi_bench.py`: a blocking send/receive of a fixed
 * byte count over a plain lwIP socket, timed to report Mbps. Used to measure
 * the ESP-Hosted SDIO transport (and thus the negotiated SDIO clock) with and
 * without the 40 MHz setting (see changelog v0.38.0). No protocol beyond a
 * raw byte stream / datagram flood, so the host endpoint is a few lines of
 * Python instead of a full iperf implementation.
 */
#ifndef NETBENCH_H
#define NETBENCH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Direction of the device-side transfer. */
typedef enum {
    NETBENCH_DIR_TX = 0, /**< Device sends `megabytes` to `host:port`. */
    NETBENCH_DIR_RX,     /**< Device listens on `port` and receives `megabytes`. */
} netbench_dir_t;

/** Probe parameters. */
typedef struct {
    netbench_dir_t direction;
    bool udp;              /**< UDP datagram flood instead of a TCP stream. */
    const char *host;      /**< Peer host/IP (TX only; RX binds all interfaces). */
    uint16_t port;         /**< TCP/UDP port. */
    uint32_t megabytes;    /**< Payload size to transfer. */
    uint32_t timeout_ms;   /**< Overall budget; abort with ESP_ERR_TIMEOUT past it. */
} netbench_config_t;

/** Probe outcome. */
typedef struct {
    uint64_t bytes;        /**< Bytes successfully transferred. */
    uint32_t elapsed_ms;   /**< Wall-clock duration. */
    double mbps;           /**< (bytes * 8) / elapsed, in megabits/second. */
    uint32_t datagrams;    /**< UDP: datagrams sent/received (0 for TCP). */
} netbench_result_t;

/**
 * Run a blocking throughput probe. Returns ESP_OK on a completed transfer
 * (result populated), ESP_ERR_TIMEOUT if the budget elapsed, or another
 * esp_err_t on a socket failure.
 */
esp_err_t netbench_run(const netbench_config_t *cfg, netbench_result_t *result);

#endif /* NETBENCH_H */
