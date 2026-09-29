/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file portal_internal.h
 * @brief Internal declarations shared by the portal sub-modules.
 */

#ifndef PORTAL_INTERNAL_H
#define PORTAL_INTERNAL_H

#include <stdint.h>

/** DNS server task entry point. */
void dns_server_task(void *arg);

/** Start/stop the DNS server. */
void dns_server_start(uint32_t ap_ip);
void dns_server_stop(void);

/** Start/stop the captive-portal HTTP server. */
void portal_http_start(void);
void portal_http_stop(void);

#endif /* PORTAL_INTERNAL_H */
