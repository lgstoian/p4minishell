#ifndef P4MINISHELL_NETDIAG_H
#define P4MINISHELL_NETDIAG_H

/**
 * @file netdiag.h
 * @brief Network diagnostics companions: `netstat` and `ipconfig`.
 *
 * Both commands live in components/networking because they read lwIP state
 * (the interface list, the DNS servers, and the TCP/UDP protocol control
 * blocks). `netstat` lists the network interfaces plus the active TCP
 * connections and UDP endpoints; `ipconfig` reports the full per-interface
 * configuration (state, MAC, IPv4, MTU, default-route flag, DNS servers).
 *
 * Output goes through the transcript appenders, so both commands are
 * redirectable and pipable like every other command. Reading the lwIP PCB
 * lists is best-effort diagnostics: the lists are traversed read-only under
 * the TCP/IP core lock when available, and a configurable row cap
 * (P4_CONFIG_NETSTAT_ROW_MAX) keeps the report bounded.
 */

#include "esp_err.h"

/** Print a `netstat` report (interfaces, TCP connections, UDP endpoints). */
void networking_netstat(void);

/** Print a rich `ipconfig` report (all interfaces + DNS servers). */
void networking_ipconfig(void);

#endif /* P4MINISHELL_NETDIAG_H */
