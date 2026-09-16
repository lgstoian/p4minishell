/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file netbench.c
 * @brief Minimal TCP/UDP throughput probe implementation (see netbench.h).
 */

#include "netbench.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "p4minishell_config.h"

#define NETBENCH_TAG "netbench"

#define NETBENCH_CHUNK_BYTES  P4_CONFIG_WIFI_BENCH_CHUNK_BYTES

/** Fill a scratch buffer once; TX sends it repeatedly. */
static void netbench_fill(uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)(i & 0xFF);
    }
}

static void netbench_finish(netbench_result_t *result, uint64_t bytes,
                            int64_t start_us, uint32_t datagrams)
{
    int64_t elapsed_us = esp_timer_get_time() - start_us;

    if (elapsed_us <= 0) {
        elapsed_us = 1;
    }
    result->bytes = bytes;
    result->datagrams = datagrams;
    result->elapsed_ms = (uint32_t)(elapsed_us / 1000);
    result->mbps = ((double)bytes * 8.0) / ((double)elapsed_us / 1000000.0) / 1000000.0;
}

static esp_err_t netbench_tcp_tx(const netbench_config_t *cfg, netbench_result_t *result)
{
    struct sockaddr_in dst;
    struct timeval tv;
    uint8_t *buf;
    uint64_t target = (uint64_t)cfg->megabytes * 1024u * 1024u;
    uint64_t sent = 0;
    int64_t start_us;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return ESP_FAIL;
    }

    tv.tv_sec = cfg->timeout_ms / 1000;
    tv.tv_usec = (cfg->timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg->port);
    if (inet_pton(AF_INET, cfg->host, &dst.sin_addr) != 1) {
        close(fd);
        return ESP_ERR_INVALID_ARG;
    }

    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        ESP_LOGE(NETBENCH_TAG, "connect %s:%u failed: errno %d", cfg->host, cfg->port, errno);
        close(fd);
        return ESP_FAIL;
    }

    buf = malloc(NETBENCH_CHUNK_BYTES);
    if (buf == NULL) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    netbench_fill(buf, NETBENCH_CHUNK_BYTES);

    start_us = esp_timer_get_time();
    while (sent < target) {
        int n = send(fd, buf, NETBENCH_CHUNK_BYTES, 0);

        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break; /* send timeout: peer stalled */
            }
            break;
        }
        sent += (uint64_t)n;
        if ((esp_timer_get_time() - start_us) > (int64_t)cfg->timeout_ms * 1000) {
            break;
        }
    }

    free(buf);
    close(fd);
    netbench_finish(result, sent, start_us, 0);
    return (sent >= target) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t netbench_tcp_rx(const netbench_config_t *cfg, netbench_result_t *result)
{
    struct sockaddr_in addr;
    struct timeval tv;
    uint8_t *buf;
    uint64_t target = (uint64_t)cfg->megabytes * 1024u * 1024u;
    uint64_t got = 0;
    int64_t start_us;
    int fd;
    int client;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg->port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(NETBENCH_TAG, "bind :%u failed: errno %d", cfg->port, errno);
        close(fd);
        return ESP_FAIL;
    }
    if (listen(fd, 1) != 0) {
        close(fd);
        return ESP_FAIL;
    }

    tv.tv_sec = cfg->timeout_ms / 1000;
    tv.tv_usec = (cfg->timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    client = accept(fd, NULL, NULL);
    if (client < 0) {
        close(fd);
        return ESP_ERR_TIMEOUT;
    }
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    buf = malloc(NETBENCH_CHUNK_BYTES);
    if (buf == NULL) {
        close(client);
        close(fd);
        return ESP_ERR_NO_MEM;
    }

    start_us = esp_timer_get_time();
    while (got < target) {
        int n = recv(client, buf, NETBENCH_CHUNK_BYTES, 0);

        if (n <= 0) {
            break;
        }
        got += (uint64_t)n;
        if ((esp_timer_get_time() - start_us) > (int64_t)cfg->timeout_ms * 1000) {
            break;
        }
    }

    free(buf);
    close(client);
    close(fd);
    netbench_finish(result, got, start_us, 0);
    return (got >= target) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t netbench_udp_tx(const netbench_config_t *cfg, netbench_result_t *result)
{
    struct sockaddr_in dst;
    struct timeval tv;
    uint8_t *buf;
    uint64_t target = (uint64_t)cfg->megabytes * 1024u * 1024u;
    uint64_t sent = 0;
    uint32_t datagrams = 0;
    int64_t start_us;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return ESP_FAIL;
    }

    tv.tv_sec = cfg->timeout_ms / 1000;
    tv.tv_usec = (cfg->timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg->port);
    if (inet_pton(AF_INET, cfg->host, &dst.sin_addr) != 1) {
        close(fd);
        return ESP_ERR_INVALID_ARG;
    }

    buf = malloc(NETBENCH_CHUNK_BYTES);
    if (buf == NULL) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    netbench_fill(buf, NETBENCH_CHUNK_BYTES);

    start_us = esp_timer_get_time();
    while (sent < target) {
        int n = sendto(fd, buf, NETBENCH_CHUNK_BYTES, 0,
                       (struct sockaddr *)&dst, sizeof(dst));

        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            break;
        }
        sent += (uint64_t)n;
        datagrams++;
        if ((esp_timer_get_time() - start_us) > (int64_t)cfg->timeout_ms * 1000) {
            break;
        }
    }

    free(buf);
    close(fd);
    netbench_finish(result, sent, start_us, datagrams);
    return (sent >= target) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t netbench_udp_rx(const netbench_config_t *cfg, netbench_result_t *result)
{
    struct sockaddr_in addr;
    struct timeval tv;
    uint8_t *buf;
    uint64_t target = (uint64_t)cfg->megabytes * 1024u * 1024u;
    uint64_t got = 0;
    uint32_t datagrams = 0;
    int64_t start_us;
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg->port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(NETBENCH_TAG, "bind :%u failed: errno %d", cfg->port, errno);
        close(fd);
        return ESP_FAIL;
    }

    tv.tv_sec = cfg->timeout_ms / 1000;
    tv.tv_usec = (cfg->timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    buf = malloc(NETBENCH_CHUNK_BYTES);
    if (buf == NULL) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }

    start_us = esp_timer_get_time();
    while (got < target) {
        int n = recvfrom(fd, buf, NETBENCH_CHUNK_BYTES, 0, NULL, NULL);

        if (n <= 0) {
            break;
        }
        got += (uint64_t)n;
        datagrams++;
        if ((esp_timer_get_time() - start_us) > (int64_t)cfg->timeout_ms * 1000) {
            break;
        }
    }

    free(buf);
    close(fd);
    netbench_finish(result, got, start_us, datagrams);
    return (got >= target) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t netbench_run(const netbench_config_t *cfg, netbench_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Zero the result before validating cfg so the caller never prints
     * uninitialized counters on an early return. */
    memset(result, 0, sizeof(*result));
    if (cfg == NULL || cfg->megabytes == 0 ||
        cfg->timeout_ms == 0 || NETBENCH_CHUNK_BYTES == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->direction == NETBENCH_DIR_TX && (cfg->host == NULL || cfg->host[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->direction == NETBENCH_DIR_TX) {
        return cfg->udp ? netbench_udp_tx(cfg, result) : netbench_tcp_tx(cfg, result);
    }
    return cfg->udp ? netbench_udp_rx(cfg, result) : netbench_tcp_rx(cfg, result);
}
