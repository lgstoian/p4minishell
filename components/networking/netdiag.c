/**
 * @file netdiag.c
 * @brief Network diagnostics: `netstat` and `ipconfig` companions.
 *
 * Reads lwIP state directly (netif_list, dns_getserver, and the TCP/UDP PCB
 * lists) so the shell can inspect the live network without reaching into the
 * driver. The PCB lists are external globals from the lwIP core; they are
 * traversed read-only and the report is capped by P4_CONFIG_NETSTAT_ROW_MAX.
 * This module is the only place that walks these lists, keeping the
 * networking component the sole owner of the lwIP surface.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"

#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/inet.h"

#include "ansi.h"
#include "ansi_palette.h"
#include "netdiag.h"
#include "networking.h"
#include "p4minishell_config.h"

#define NETDIAG_ROW_MAX         P4_CONFIG_NETSTAT_ROW_MAX
#define NETDIAG_TAG             P4_CONFIG_SHELL_TAG

/* The TCP PCB lists live in the lwIP private core; declare them here so the
 * module does not depend on the private header. */
extern struct tcp_pcb *tcp_active_pcbs;
extern struct tcp_pcb *tcp_tw_pcbs;
extern struct tcp_pcb_listen *tcp_listen_pcbs;

/* ---- TCP/IP core lock (no-op when core locking is disabled) ---- */
#if LWIP_TCPIP_CORE_LOCKING
#define NETDIAG_LOCK_CORE()    LOCK_TCPIP_CORE()
#define NETDIAG_UNLOCK_CORE()  UNLOCK_TCPIP_CORE()
#else
#define NETDIAG_LOCK_CORE()    do { } while (0)
#define NETDIAG_UNLOCK_CORE()  do { } while (0)
#endif

/* ---- Transcript output through the networking host ops ---- */

static const networking_host_ops_t *netdiag_ops(void)
{
    return networking_get_host_ops();
}

static void netdiag_appendf(const char *format, ...)
{
    const networking_host_ops_t *ops = netdiag_ops();
    char buffer[512];
    va_list args;

    if (ops == NULL || ops->transcript_append_ansi == NULL) {
        return;
    }
    va_start(args, format);
    ansi_vformat(buffer, sizeof(buffer), format, args);
    va_end(args);
    ops->transcript_append_ansi(buffer);
}

/* ---- Helpers ---- */

/** IPv4 text of an ip_addr_t, or "-" for non-IPv4. */
static const char *netdiag_ip_string(const ip_addr_t *ip, char *buf, size_t buf_size)
{
    if (ip == NULL || !IP_IS_V4(ip) || ip4_addr_isany_val(*ip_2_ip4(ip))) {
        snprintf(buf, buf_size, "-");
        return buf;
    }
    return ip4addr_ntoa_r(ip_2_ip4(ip), buf, buf_size);
}

/** Map a lwIP tcp_state to a short string. */
static const char *netdiag_tcp_state_string(u8_t state)
{
    switch (state) {
    case CLOSED:        return "CLOSED";
    case LISTEN:        return "LISTEN";
    case SYN_SENT:      return "SYN_SENT";
    case SYN_RCVD:      return "SYN_RCVD";
    case ESTABLISHED:   return "ESTABLISHED";
    case FIN_WAIT_1:    return "FIN_WAIT_1";
    case FIN_WAIT_2:    return "FIN_WAIT_2";
    case CLOSE_WAIT:    return "CLOSE_WAIT";
    case CLOSING:       return "CLOSING";
    case LAST_ACK:      return "LAST_ACK";
    case TIME_WAIT:     return "TIME_WAIT";
    default:            return "?";
    }
}

/** Format a MAC address as colon-separated hex. */
static void netdiag_mac_string(const struct netif *netif, char *buf, size_t buf_size)
{
    size_t i;
    size_t o = 0;
    size_t len = netif->hwaddr_len;

    if (len > 6) {
        len = 6;
    }
    for (i = 0; i < len; i++) {
        int written = snprintf(buf + o, buf_size - o, "%s%02x",
                               i > 0 ? ":" : "", netif->hwaddr[i]);
        if (written < 0 || (size_t)written >= buf_size - o) {
            break;
        }
        o += (size_t)written;
    }
}

/** Print one network interface row. */
static void netdiag_print_interface(const struct netif *netif, bool is_default)
{
    char ip[16];
    char mask[16];
    char gw[16];
    char mac[24];

    netdiag_ip_string(&netif->ip_addr, ip, sizeof(ip));
    netdiag_ip_string(&netif->netmask, mask, sizeof(mask));
    netdiag_ip_string(&netif->gw, gw, sizeof(gw));
    netdiag_mac_string(netif, mac, sizeof(mac));

    netdiag_appendf("  " SH_LBL "%c%c%d" SH_RST " ",
                    (netif->name[0] != '\0' ? netif->name[0] : '?'),
                    (netif->name[1] != '\0' ? netif->name[1] : '?'),
                    (int)netif->num);
    if (is_default) {
        netdiag_appendf(SH_OK "*" SH_RST " ");
    } else {
        netdiag_appendf(SH_MUTE "-" SH_RST " ");
    }
    netdiag_appendf(SH_VAL "%-9s" SH_RST " " SH_VAL "%-15s" SH_RST " "
                    SH_NUM "%-15s" SH_RST " " SH_NUM "%-15s" SH_RST " " SH_NUM "%5u" SH_RST " "
                    SH_MUTE "%s" SH_RST "\n",
                    netif_is_up(netif) ? "up" : "down",
                    ip,
                    mask,
                    gw,
                    (unsigned int)netif->mtu,
                    mac);
}

/* ---- Public commands ---- */

void networking_netstat(void)
{
    struct netif *netif;
    struct tcp_pcb *pcb;
    struct tcp_pcb_listen *lpcb;
    struct udp_pcb *upcb;
    char local[48];
    char remote[48];
    size_t rows = 0;

    NETDIAG_LOCK_CORE();

    netdiag_appendf(SH_HEAD "Network Interfaces" SH_RST "\n");
    netdiag_appendf("  " SH_MUTE "IF   State      IPv4            Netmask         Gateway          MTU   MAC" SH_RST "\n");
    for (netif = netif_list; netif != NULL && rows < NETDIAG_ROW_MAX; netif = netif->next) {
        netdiag_print_interface(netif, netif == netif_default);
        rows++;
    }

    netdiag_appendf(SH_HEAD "Active TCP Connections" SH_RST "\n");
    netdiag_appendf("  " SH_MUTE "Local                     Remote                    State" SH_RST "\n");
    rows = 0;
    for (pcb = tcp_active_pcbs; pcb != NULL && rows < NETDIAG_ROW_MAX; pcb = pcb->next) {
        snprintf(local, sizeof(local), "%s:%u",
                 netdiag_ip_string(&pcb->local_ip, local, sizeof(local)),
                 (unsigned int)pcb->local_port);
        snprintf(remote, sizeof(remote), "%s:%u",
                 netdiag_ip_string(&pcb->remote_ip, remote, sizeof(remote)),
                 (unsigned int)pcb->remote_port);
        netdiag_appendf("  " SH_NUM "%-25s" SH_RST " " SH_NUM "%-25s" SH_RST " " SH_VAL "%s" SH_RST "\n",
                        local, remote, netdiag_tcp_state_string(pcb->state));
        rows++;
    }
    for (lpcb = tcp_listen_pcbs; lpcb != NULL && rows < NETDIAG_ROW_MAX; lpcb = lpcb->next) {
        snprintf(local, sizeof(local), "*:%u", (unsigned int)lpcb->local_port);
        netdiag_appendf("  " SH_NUM "%-25s" SH_RST " " SH_MUTE "%-25s" SH_RST " " SH_VAL "%-10s" SH_RST "\n",
                        local, "-", "LISTEN");
        rows++;
    }
    for (pcb = tcp_tw_pcbs; pcb != NULL && rows < NETDIAG_ROW_MAX; pcb = pcb->next) {
        snprintf(local, sizeof(local), "%s:%u",
                 netdiag_ip_string(&pcb->local_ip, local, sizeof(local)),
                 (unsigned int)pcb->local_port);
        snprintf(remote, sizeof(remote), "%s:%u",
                 netdiag_ip_string(&pcb->remote_ip, remote, sizeof(remote)),
                 (unsigned int)pcb->remote_port);
        netdiag_appendf("  " SH_NUM "%-25s" SH_RST " " SH_NUM "%-25s" SH_RST " " SH_VAL "%s" SH_RST "\n",
                        local, remote, netdiag_tcp_state_string(pcb->state));
        rows++;
    }
    if (rows == 0) {
        netdiag_appendf("  " SH_MUTE "no active connections" SH_RST "\n");
    }

    netdiag_appendf(SH_HEAD "UDP Endpoints" SH_RST "\n");
    netdiag_appendf("  " SH_MUTE "Local                     Remote" SH_RST "\n");
    rows = 0;
    for (upcb = udp_pcbs; upcb != NULL && rows < NETDIAG_ROW_MAX; upcb = upcb->next) {
        snprintf(local, sizeof(local), "%s:%u",
                 netdiag_ip_string(&upcb->local_ip, local, sizeof(local)),
                 (unsigned int)upcb->local_port);
        if (upcb->remote_port != 0) {
            snprintf(remote, sizeof(remote), "%s:%u",
                     netdiag_ip_string(&upcb->remote_ip, remote, sizeof(remote)),
                     (unsigned int)upcb->remote_port);
        } else {
            snprintf(remote, sizeof(remote), "-");
        }
        netdiag_appendf("  " SH_NUM "%-25s" SH_RST " " SH_NUM "%-25s" SH_RST "\n", local, remote);
        rows++;
    }
    if (rows == 0) {
        netdiag_appendf("  " SH_MUTE "no active endpoints" SH_RST "\n");
    }

    NETDIAG_UNLOCK_CORE();
}

void networking_ipconfig(void)
{
    struct netif *netif;
    char ip[16];
    char mask[16];
    char gw[16];
    char mac[24];
    int dns_index;

    NETDIAG_LOCK_CORE();

    netdiag_appendf(SH_HEAD "IP Configuration" SH_RST "\n");
    for (netif = netif_list; netif != NULL; netif = netif->next) {
        netdiag_ip_string(&netif->ip_addr, ip, sizeof(ip));
        netdiag_ip_string(&netif->netmask, mask, sizeof(mask));
        netdiag_ip_string(&netif->gw, gw, sizeof(gw));
        netdiag_mac_string(netif, mac, sizeof(mac));

        netdiag_appendf("  " SH_LBL "interface %c%c%d" SH_RST "%s" SH_RST "\n",
                        (netif->name[0] != '\0' ? netif->name[0] : '?'),
                        (netif->name[1] != '\0' ? netif->name[1] : '?'),
                        (int)netif->num,
                        netif == netif_default ? " (default route)" : "");
        if (netif_is_up(netif)) {
            netdiag_appendf("    " SH_LBL "state:" SH_RST " " SH_OK "up" SH_RST "\n");
        } else {
            netdiag_appendf("    " SH_LBL "state:" SH_RST " " SH_MUTE "down" SH_RST "\n");
        }
        netdiag_appendf("    " SH_LBL "mac:" SH_RST " " SH_VAL "%s" SH_RST "\n", mac);
        netdiag_appendf("    " SH_LBL "ipv4:" SH_RST " " SH_VAL "%s" SH_RST "\n", ip);
        netdiag_appendf("    " SH_LBL "netmask:" SH_RST " " SH_VAL "%s" SH_RST "\n", mask);
        netdiag_appendf("    " SH_LBL "gateway:" SH_RST " " SH_VAL "%s" SH_RST "\n", gw);
        netdiag_appendf("    " SH_LBL "mtu:" SH_RST " " SH_NUM "%u" SH_RST "\n", (unsigned int)netif->mtu);
    }

    netdiag_appendf(SH_LBL "DNS servers:" SH_RST " ");
    {
        bool any = false;
        for (dns_index = 0; dns_index < DNS_MAX_SERVERS; dns_index++) {
            const ip_addr_t *server = dns_getserver((u8_t)dns_index);
            if (server != NULL && !ip_addr_isany(server)) {
                netdiag_appendf("%s" SH_VAL "%s" SH_RST,
                                any ? ", " : "",
                                netdiag_ip_string(server, ip, sizeof(ip)));
                any = true;
            }
        }
        if (!any) {
            netdiag_appendf(SH_MUTE "none" SH_RST);
        }
        netdiag_appendf("\n");
    }

    NETDIAG_UNLOCK_CORE();
}
