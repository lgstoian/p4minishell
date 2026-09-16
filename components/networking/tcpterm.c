/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tcpterm.c
 * @brief `tcpterm` — one-shot TCP request/response terminal (modern Datacomm).
 *
 * The 95LX Comm app spoke RS-232/modem; this board has no RS-232 peer, so the
 * modern equivalent is TCP over hosted Wi-Fi, owned here alongside every
 * other socket surface. A session resolves the host, connects with a bounded
 * timeout, sends one request payload, half-closes, and prints the reply:
 *
 *   tcpterm <host> <port> [/t:secs] [text...]
 *
 * `text...` accepts `\r` `\n` `\t` `\\` escapes, so an HTTP probe reads
 * naturally (`tcpterm 127.0.0.1 80 "GET / HTTP/1.0\r\n\r\n"` hits the local
 * httpd). With no text and an active `< file` / pipe source the command
 * layer feeds that instead (assembled before the call; this file only ever
 * sees a byte buffer). Replies print sanitized like `httpget` bodies except
 * that ESC passes through, so remote SGR colours render on both surfaces;
 * cursor-motion sequences are not interpreted and show literally.
 *
 * Everything runs on the calling (worker) task with hard budgets: connect,
 * send, and an idle deadline that refreshes per received byte. No key waits,
 * no modal, fully background-safe.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "networking.h"
#include "p4minishell_config.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef P4_CONFIG_TCP_CONNECT_TIMEOUT_MS
#define P4_CONFIG_TCP_CONNECT_TIMEOUT_MS 10000
#endif

#ifndef P4_CONFIG_TCP_IDLE_TIMEOUT_MS
#define P4_CONFIG_TCP_IDLE_TIMEOUT_MS 5000
#endif

#ifndef P4_CONFIG_TCP_RX_MAX_BYTES
#define P4_CONFIG_TCP_RX_MAX_BYTES 65536
#endif

#ifndef P4_CONFIG_TCP_TX_MAX_BYTES
#define P4_CONFIG_TCP_TX_MAX_BYTES 65536
#endif

/* ------------------------------------------------------------------------
 * Host-render plumbing (mirrors netdiag.c: never touches LVGL directly)
 * ---------------------------------------------------------------------- */

static const networking_host_ops_t *tcpterm_ops(void)
{
    return networking_get_host_ops();
}

static void tcpterm_appendf(const char *format, ...)
{
    const networking_host_ops_t *ops = tcpterm_ops();
    char line[256];
    va_list args;

    if (ops == NULL || ops->transcript_append_text == NULL) {
        return;
    }
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ops->transcript_append_text(line);
}

/* ------------------------------------------------------------------------
 * Pure helpers (unit-tested, no sockets)
 * ---------------------------------------------------------------------- */

bool networking_tcp_parse_target(const char *host, const char *port_str, int *port_out)
{
    char *end = NULL;
    long port;

    if (port_out != NULL) {
        *port_out = 0;
    }
    if (host == NULL || host[0] == '\0' || port_str == NULL || port_out == NULL) {
        return false;
    }
    port = strtol(port_str, &end, 10);
    if (end == port_str || *end != '\0' || port < 1 || port > 65535) {
        return false;
    }
    *port_out = (int)port;
    return true;
}

size_t networking_tcp_unescape(const char *in, char *out, size_t out_size)
{
    size_t used = 0;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }
    while (*in != '\0' && used + 1 < out_size) {
        if (*in == '\\' && *(in + 1) != '\0') {
            char next = *(in + 1);
            char emit = 0;
            if (next == 'r') {
                emit = '\r';
            } else if (next == 'n') {
                emit = '\n';
            } else if (next == 't') {
                emit = '\t';
            } else if (next == '\\') {
                emit = '\\';
            }
            if (emit != 0) {
                out[used++] = emit;
                in += 2;
                continue;
            }
        }
        out[used++] = *in++;
    }
    out[used] = '\0';
    return used;
}

size_t networking_tcp_sanitize(const uint8_t *in, size_t len, char *out, size_t out_size)
{
    size_t used = 0;
    size_t i;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }
    for (i = 0; i < len && used + 1 < out_size; i++) {
        unsigned char ch = in[i];
        /* Printable ASCII plus the text separators plus ESC (remote SGR
         * colours render through the normal transcript path); every other
         * control byte (incl. NUL) becomes a harmless dot. */
        if (ch == 0x1B || ch == '\n' || ch == '\r' || ch == '\t' ||
            (ch >= 0x20 && ch < 0x7F)) {
            out[used++] = (char)ch;
        } else {
            out[used++] = '.';
        }
    }
    out[used] = '\0';
    return used;
}

/* ------------------------------------------------------------------------
 * Session
 * ---------------------------------------------------------------------- */

esp_err_t networking_tcp_term(const char *host, int port,
                              const uint8_t *tx, size_t tx_len,
                              int idle_timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct sockaddr_in addr;
    struct timeval tv;
    uint8_t *rx = NULL;
    int fd = -1;
    int flags;
    int64_t deadline_us;
    size_t received = 0;
    size_t printed = 0;
    esp_err_t result = ESP_FAIL;

    if (host == NULL || host[0] == '\0' || port < 1 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx == NULL && tx_len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx_len > (size_t)P4_CONFIG_TCP_TX_MAX_BYTES) {
        tcpterm_appendf("tcpterm: request exceeds %d bytes\n", P4_CONFIG_TCP_TX_MAX_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!networking_wifi_is_connected()) {
        tcpterm_appendf("tcpterm: no active connection; run wifi connect first\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (idle_timeout_ms <= 0) {
        idle_timeout_ms = P4_CONFIG_TCP_IDLE_TIMEOUT_MS;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL || res->ai_addr == NULL) {
        tcpterm_appendf("tcpterm: cannot resolve host %s\n", host);
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(&addr, res->ai_addr, sizeof(addr));
    addr.sin_port = htons((uint16_t)port);
    freeaddrinfo(res);

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        tcpterm_appendf("tcpterm: socket failed\n");
        return ESP_FAIL;
    }
    /* Non-blocking connect + select gives a hard connect budget (a blocking
     * connect on lwIP can stall far past any socket timeout). */
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        tcpterm_appendf("tcpterm: cannot set non-blocking mode\n");
        close(fd);
        return ESP_FAIL;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 && errno != EINPROGRESS) {
        tcpterm_appendf("tcpterm: connect to %s:%d failed\n", host, port);
        close(fd);
        return ESP_FAIL;
    }
    {
        fd_set writers;
        struct timeval ctv;
        int selected;
        int ok = 0;

        FD_ZERO(&writers);
        FD_SET(fd, &writers);
        ctv.tv_sec = P4_CONFIG_TCP_CONNECT_TIMEOUT_MS / 1000;
        ctv.tv_usec = (P4_CONFIG_TCP_CONNECT_TIMEOUT_MS % 1000) * 1000;
        selected = select(fd + 1, NULL, &writers, NULL, &ctv);
        if (selected > 0 && FD_ISSET(fd, &writers)) {
            int sockerr = 0;
            socklen_t sockerr_len = sizeof(sockerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &sockerr_len) == 0 &&
                sockerr == 0) {
                ok = 1;
            } else {
                tcpterm_appendf("tcpterm: connect to %s:%d refused\n", host, port);
                close(fd);
                return ESP_FAIL;
            }
        }
        if (!ok) {
            tcpterm_appendf("tcpterm: connect to %s:%d timed out\n", host, port);
            close(fd);
            return ESP_ERR_TIMEOUT;
        }
    }
    (void)fcntl(fd, F_SETFL, flags);
    tv.tv_sec = idle_timeout_ms / 1000;
    tv.tv_usec = (idle_timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    rx = malloc(P4_CONFIG_TCP_RX_MAX_BYTES);
    if (rx == NULL) {
        tcpterm_appendf("tcpterm: out of memory\n");
        close(fd);
        return ESP_ERR_NO_MEM;
    }

    /* Send the request, then half-close: most line protocols (HTTP, echo,
     * status banners) answer once the request is complete. */
    {
        size_t sent = 0;
        while (sent < tx_len) {
            int n = send(fd, tx + sent, tx_len - sent, 0);
            if (n <= 0) {
                tcpterm_appendf("tcpterm: send failed\n");
                goto done;
            }
            sent += (size_t)n;
        }
    }
    (void)shutdown(fd, SHUT_WR);

    /* Read until the peer closes or a full idle window passes with no data.
     * The deadline refreshes per received byte, so a slow-but-live peer is
     * never cut off early while a dead one always terminates. */
    deadline_us = esp_timer_get_time() + (int64_t)idle_timeout_ms * 1000;
    for (;;) {
        int n = recv(fd, rx + received, P4_CONFIG_TCP_RX_MAX_BYTES - received, 0);
        if (n > 0) {
            received += (size_t)n;
            deadline_us = esp_timer_get_time() + (int64_t)idle_timeout_ms * 1000;
            if (received >= P4_CONFIG_TCP_RX_MAX_BYTES) {
                break;
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (esp_timer_get_time() >= deadline_us) {
            break;
        }
        /* Brief nap keeps a tight EAGAIN spin from starving the worker. */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* Sanitized print in bounded chunks (never holds the whole reply twice). */
    while (printed < received) {
        char chunk[256];
        size_t step = received - printed;
        if (step > sizeof(chunk) - 1) {
            step = sizeof(chunk) - 1;
        }
        networking_tcp_sanitize(rx + printed, step, chunk, sizeof(chunk));
        tcpterm_appendf("%s", chunk);
        printed += step;
    }
    tcpterm_appendf("\n[tcpterm: %s:%d closed, %u byte(s) in %u out]\n", host, port,
                    (unsigned)received, (unsigned)tx_len);
    result = ESP_OK;

done:
    free(rx);
    close(fd);
    return result;
}
