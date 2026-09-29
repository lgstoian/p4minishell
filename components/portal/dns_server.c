/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file dns_server.c
 * @brief Minimal captive-portal DNS server.
 *
 * Responds to every DNS query with the device's AP IP address so that
 * captive-portal clients are redirected to the portal page.  The server
 * runs on a dedicated FreeRTOS task and uses a single UDP socket on port 53.
 */

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "p4minishell_config.h"
#include "portal_internal.h"

#define DNS_TAG "portal_dns"

/** Minimal DNS header (RFC 1035). */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

/** Standard DNS response flags: response, no error, recursion desired. */
#define DNS_FLAGS_RESPONSE  0x8180

/** A-pointer response: answer class IN, type A, TTL 60s. */
static const uint8_t dns_answer_prefix[] = {
    0xC0, 0x0C,     /* name pointer to the query name */
    0x00, 0x01,     /* type A */
    0x00, 0x01,     /* class IN */
    0x00, 0x00, 0x00, 0x3C, /* TTL = 60 s */
    0x00, 0x04,     /* RDLENGTH = 4 (IPv4) */
};

static TaskHandle_t s_dns_task;
static bool s_dns_running;
static uint32_t s_dns_server_ip;  /* AP IP in network byte order */

void dns_server_start(uint32_t ap_ip)
{
    if (s_dns_running) {
        return;
    }
    s_dns_server_ip = ap_ip;
    s_dns_running = true;
    if (xTaskCreate(dns_server_task, "dns_srv", 3072, NULL,
                    tskIDLE_PRIORITY + 2, &s_dns_task) != pdPASS) {
        ESP_LOGE(DNS_TAG, "failed to create DNS server task");
        s_dns_running = false;
    }
}

void dns_server_stop(void)
{
    if (!s_dns_running) {
        return;
    }
    s_dns_running = false;
    if (s_dns_task != NULL) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
}

void dns_server_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(DNS_TAG, "socket: errno %d", errno);
        s_dns_running = false;
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = s_dns_server_ip,
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(DNS_TAG, "bind: errno %d", errno);
        close(sock);
        s_dns_running = false;
        return;
    }

    ESP_LOGI(DNS_TAG, "DNS server started on %s:53",
             inet_ntoa(*(struct in_addr *)&s_dns_server_ip));

    while (s_dns_running) {
        uint8_t buf[512];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *hdr = (dns_header_t *)buf;

        /* Build the response: copy the query, set response flags, add an
         * A-record answer pointing to the device's AP IP. */
        uint8_t resp[512];
        int resp_len = 0;

        /* DNS header: keep the ID, set flags to "response, no error",
         * echo question count, set answer count = 1. */
        memcpy(resp, buf, sizeof(dns_header_t));
        dns_header_t *rhdr = (dns_header_t *)resp;
        rhdr->flags = htons(DNS_FLAGS_RESPONSE);
        rhdr->qdcount = hdr->qdcount;
        rhdr->ancount = htons(1);
        rhdr->nscount = 0;
        rhdr->arcount = 0;
        resp_len = sizeof(dns_header_t);

        /* Find the end of the question section (skip the QNAME + QTYPE + QCLASS). */
        int qname_end = sizeof(dns_header_t);
        while (qname_end < len && buf[qname_end] != 0) {
            qname_end += buf[qname_end] + 1;
        }
        qname_end += 5; /* +1 for the null terminator + 2 for QTYPE + 2 for QCLASS */

        if (qname_end > len) {
            /* Malformed query; just echo it back. */
            memcpy(resp + resp_len, buf + resp_len, len - resp_len);
            resp_len = len;
        } else {
            /* Copy the question section verbatim. */
            memcpy(resp + resp_len, buf + sizeof(dns_header_t),
                   qname_end - sizeof(dns_header_t));
            resp_len = qname_end;

            /* Append the A-record answer. */
            memcpy(resp + resp_len, dns_answer_prefix, sizeof(dns_answer_prefix));
            resp_len += sizeof(dns_answer_prefix);
            memcpy(resp + resp_len, &s_dns_server_ip, 4);
            resp_len += 4;
        }

        sendto(sock, resp, resp_len, 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    ESP_LOGI(DNS_TAG, "DNS server stopped");
}
