// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file netsvc.c
 * @brief Persistent MQTT service: one task, one session, offline-first.
 *
 * The task loops CONNECTED/BACKOFF/IDLE: it dials the configured broker with
 * an MQTT CONNECT, resubscribes the RAM table, flushes the SD outbox
 * oldest-first, then pumps frames (keepalive PING, PUBACK tracking, inbound
 * PUBLISH dispatch). It only runs while Wi-Fi is associated and no OTA is
 * active (checked every iteration before any socket/heap/SD touch, so a C6
 * flash or a sleep teardown strands nothing: the loop just waits).
 *
 * Every publish is journal-first (write-through outbox): the record hits SD,
 * then the wire; the file is removed on PUBACK. Offline publishes only
 * journal. Inbound `$pim/...` payloads merge newer-wins through
 * components/pim (the single merger), gated on the lock hook. Delivery to
 * batch is the stored `/onmsg` line via the registered async hook plus the
 * shared last-message slot (`net msg`); status surfaces are the existing
 * header/LED notifies owned by networking's host ops.
 */
#include "netsvc.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mqtt_codec.h"
#include "networking.h"
#include "p4minishell_config.h"
#include "pim.h"
#include "storage.h"

#define NETSVC_TAG "netsvc"
#define NETSVC_MAX_SUBS 8
#define NETSVC_PROFILE_BROKER "broker"
#define NETSVC_PROFILE_PORT "port"
#define NETSVC_PROFILE_USER "user"
#define NETSVC_PROFILE_PASS "pass"
#define NETSVC_PROFILE_CLIENTID "clientid"
#define NETSVC_PIM_DB_PREFIX "$pim/db/"
#define NETSVC_PIM_ALARMS "$pim/alarms"

/* Service state, owned by the task; verbs touch it under the lock. */
typedef struct {
    TaskHandle_t task;
    SemaphoreHandle_t lock;
    bool enabled;
    bool connected;
    bool ota_paused;
    int sock;
    unsigned attempt;
    uint16_t pkt_id;
    char subs[NETSVC_MAX_SUBS][P4_CONFIG_NET_TOPIC_BYTES];
    int sub_count;
    char onmsg[P4_CONFIG_NET_ONMSG_BYTES];
    char last_topic[P4_CONFIG_NET_TOPIC_BYTES];
    uint8_t *last_payload;
    size_t last_len;
    uint64_t outbox_seq;
    int64_t last_io_us;
} netsvc_state_t;

static netsvc_state_t s_net;
static netsvc_host_ops_t s_host_ops;
static bool s_inited = false;

/* ========================================================================
 * Pure helpers (unit-tested through netsvc.h)
 * ======================================================================== */

uint32_t netsvc_backoff_ms(unsigned attempt, uint32_t base_ms, uint32_t cap_ms)
{
    uint64_t wait = (uint64_t)base_ms;
    unsigned i;
    if (cap_ms == 0) {
        return 0;
    }
    if (base_ms == 0) {
        base_ms = 1;
    }
    wait = base_ms;
    for (i = 0; i < attempt && wait < cap_ms; i++) {
        wait *= 2u;
        if (wait > cap_ms) {
            wait = cap_ms;
        }
    }
    if (wait > cap_ms) {
        wait = cap_ms;
    }
    return (uint32_t)wait;
}

int netsvc_outbox_encode(const char *topic, const uint8_t *payload, size_t len,
                         uint8_t qos, char *out, size_t out_size)
{
    int n;
    if (topic == NULL || topic[0] == '\0' || out == NULL || out_size == 0 ||
        (len > 0 && payload == NULL) || qos > 1) {
        return -1;
    }
    n = snprintf(out, out_size, "N1\n%s\n%u\n%u\n%u\n",
                 topic, (unsigned)qos, 0u, (unsigned)len);
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    if ((size_t)n + len + 1 > out_size) {
        return -1;
    }
    if (len > 0) {
        memcpy(out + n, payload, len);
    }
    out[n + len] = '\n';
    return (int)(n + len + 1);
}

int netsvc_outbox_decode(const char *rec, size_t rec_len,
                         char *topic_out, size_t topic_size,
                         const uint8_t **payload_out, size_t *payload_len_out,
                         uint8_t *qos_out)
{
    /* Layout: "N1\n" topic "\n" qos "\n" retain "\n" len "\n" payload "\n". */
    size_t pos = 0;
    size_t eol;
    size_t tlen;
    unsigned long qos = 0;
    unsigned long paylen = 0;
    if (rec == NULL || rec_len < 8 || topic_out == NULL || topic_size == 0 ||
        payload_out == NULL || payload_len_out == NULL) {
        return -1;
    }
    if (memcmp(rec, "N1\n", 3) != 0) {
        return -1;
    }
    pos = 3;
    eol = pos;
    while (eol < rec_len && rec[eol] != '\n') {
        eol++;
    }
    if (eol >= rec_len || eol == pos) {
        return -1;
    }
    tlen = eol - pos;
    if (tlen + 1 > topic_size) {
        return -1;
    }
    memcpy(topic_out, rec + pos, tlen);
    topic_out[tlen] = '\0';
    pos = eol + 1;
    /* qos line */
    eol = pos;
    while (eol < rec_len && rec[eol] != '\n') {
        eol++;
    }
    if (eol >= rec_len || eol == pos || eol - pos > 1) {
        return -1;
    }
    if (rec[pos] < '0' || rec[pos] > '1') {
        return -1;
    }
    qos = (unsigned long)(rec[pos] - '0');
    pos = eol + 1;
    /* retain line (accepted, currently always 0) */
    eol = pos;
    while (eol < rec_len && rec[eol] != '\n') {
        eol++;
    }
    if (eol >= rec_len || eol == pos || eol - pos > 1) {
        return -1;
    }
    pos = eol + 1;
    /* length line */
    eol = pos;
    while (eol < rec_len && rec[eol] != '\n') {
        if (rec[eol] < '0' || rec[eol] > '9') {
            return -1;
        }
        eol++;
    }
    if (eol >= rec_len || eol == pos || eol - pos > 10) {
        return -1;
    }
    for (size_t i = pos; i < eol; i++) {
        paylen = paylen * 10u + (unsigned long)(rec[i] - '0');
    }
    pos = eol + 1;
    if (pos + paylen + 1 > rec_len || rec[pos + paylen] != '\n') {
        return -1;
    }
    *payload_out = (const uint8_t *)(rec + pos);
    *payload_len_out = paylen;
    if (qos_out != NULL) {
        *qos_out = (uint8_t)qos;
    }
    return 0;
}

/* ========================================================================
 * Internal plumbing
 * ======================================================================== */

static const networking_host_ops_t *netsvc_ops(void)
{
    return networking_get_host_ops();
}

static void netsvc_note(const char *text)
{
    const networking_host_ops_t *ops = netsvc_ops();
    if (ops != NULL && ops->notify_header != NULL && text != NULL) {
        ops->notify_header(text, 2500);
    }
}

static bool netsvc_can_sync(void)
{
    if (s_host_ops.can_reveal_private == NULL) {
        return false;
    }
    return s_host_ops.can_reveal_private();
}

static bool netsvc_ota_active(void)
{
    if (s_host_ops.ota_active == NULL) {
        return false;
    }
    return s_host_ops.ota_active();
}

static void netsvc_lock(void)
{
    if (s_net.lock != NULL) {
        xSemaphoreTake(s_net.lock, portMAX_DELAY);
    }
}

static void netsvc_unlock(void)
{
    if (s_net.lock != NULL) {
        xSemaphoreGive(s_net.lock);
    }
}

/* DB names from remote topics stay filesystem-safe (mirror the app rule). */
static bool netsvc_db_name_ok(const char *name)
{
    size_t i;
    if (name == NULL || name[0] == '\0' || strlen(name) > 64) {
        return false;
    }
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if ((c < 'A' || c > 'Z') && (c < 'a' || c > 'z') &&
            (c < '0' || c > '9') && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

static void netsvc_close_socket(void)
{
    if (s_net.sock >= 0) {
        close(s_net.sock);
        s_net.sock = -1;
    }
    s_net.connected = false;
}

/* -- profile (NET.INI through the single INI core) ---------------------- */

static esp_err_t netsvc_profile_get(const char *key, char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (storage_ini_file_get(P4_CONFIG_NET_PROFILE_FILE, key, out, size) != ESP_OK) {
        out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* -- outbox journal ------------------------------------------------------ */

static void netsvc_outbox_path(char *out, size_t size, const char *id)
{
    snprintf(out, size, "%s/%s.MSG", P4_CONFIG_NET_OUTBOX_DIR, id);
}

static void netsvc_mint_id(char *out, size_t size)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    uint32_t rnd = esp_random();
    s_net.outbox_seq++;
    snprintf(out, size, "%010llx%04x%04x",
             (unsigned long long)(now & 0xFFFFFFFFFFull),
             (unsigned)(rnd & 0xFFFFu),
             (unsigned)(s_net.outbox_seq & 0xFFFFu));
}

static int netsvc_outbox_usage(size_t *bytes_out)
{
    DIR *d;
    struct dirent *e;
    int count = 0;
    size_t bytes = 0;
    shell_sd_session_t session;
    if (shell_sd_begin(&session) != ESP_OK) {
        return -1;
    }
    d = opendir(P4_CONFIG_NET_OUTBOX_DIR);
    if (d == NULL) {
        shell_sd_end(&session, "net-outbox");
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        if (nlen < 5 || strcmp(e->d_name + nlen - 4, ".MSG") != 0) {
            continue;
        }
        {
            char full[P4_CONFIG_SD_PATH_BYTES + P4_CONFIG_LFN_BYTES];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", P4_CONFIG_NET_OUTBOX_DIR, e->d_name);
            if (stat(full, &st) == 0) {
                bytes += (size_t)st.st_size;
            }
        }
        count++;
    }
    closedir(d);
    shell_sd_end(&session, "net-outbox");
    if (bytes_out != NULL) {
        *bytes_out = bytes;
    }
    return count;
}

/* Journal one record; caller holds no lock (opens its own SD session). */
static esp_err_t netsvc_outbox_append(const char *topic,
                                      const uint8_t *payload, size_t len)
{
    size_t framing;
    size_t total;
    size_t used = 0;
    int count;
    char id[32];
    char path[P4_CONFIG_SD_PATH_BYTES];
    char *rec = NULL;
    FILE *f = NULL;
    shell_sd_session_t session;
    esp_err_t err = ESP_OK;
    if (topic == NULL || (len > 0 && payload == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    framing = strlen(topic) + 32;
    total = framing + len + 2;
    count = netsvc_outbox_usage(&used);
    if (count < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count >= P4_CONFIG_NET_OUTBOX_MAX ||
        used + total > P4_CONFIG_NET_OUTBOX_MAX_BYTES) {
        return ESP_ERR_NO_MEM;
    }
    if (!storage_check_free_space((uint64_t)total + 4096u, 0, "net")) {
        return ESP_ERR_NO_MEM;
    }
    rec = malloc(total);
    if (rec == NULL) {
        return ESP_ERR_NO_MEM;
    }
    {
        int wlen = netsvc_outbox_encode(topic, payload, len, 1, rec, total);
        if (wlen < 0) {
            free(rec);
            return ESP_ERR_INVALID_SIZE;
        }
        if (shell_sd_begin(&session) != ESP_OK) {
            free(rec);
            return ESP_ERR_INVALID_STATE;
        }
        if (storage_mkdir_p(P4_CONFIG_NET_OUTBOX_DIR) != ESP_OK) {
            shell_sd_end(&session, "net-outbox");
            free(rec);
            return ESP_FAIL;
        }
        netsvc_mint_id(id, sizeof(id));
        netsvc_outbox_path(path, sizeof(path), id);
        f = fopen(path, "wb");
        if (f == NULL) {
            err = ESP_FAIL;
        } else {
            if (fwrite(rec, 1, (size_t)wlen, f) != (size_t)wlen) {
                err = ESP_FAIL;
            }
            fclose(f);
            if (err != ESP_OK) {
                remove(path);
            }
        }
        shell_sd_end(&session, "net-outbox");
        free(rec);
        return err;
    }
}

/* ========================================================================
 * Socket engine (lwIP, same patterns as tcpterm: bounded everything)
 * ======================================================================== */

static int netsvc_sock_set_timeout(int fd, uint32_t timeout_ms)
{
    struct timeval tv;
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    return 0;
}

/* Resolve + connect with a bounded wait. Returns the fd or -1. */
static int netsvc_tcp_dial(const char *host, int port, uint32_t timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    struct addrinfo *ai;
    char port_str[8];
    int fd = -1;
    if (host == NULL || host[0] == '\0' || port <= 0 || port > 65535) {
        return -1;
    }
    snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &list) != 0 || list == NULL) {
        return -1;
    }
    for (ai = list; ai != NULL; ai = ai->ai_next) {
        int flags;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break; /* instant (loopback) connect */
        }
        if (errno == EINPROGRESS) {
            fd_set wfds;
            struct timeval tv;
            int soerr = 0;
            socklen_t olen = sizeof(soerr);
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = (long)(timeout_ms / 1000u);
            tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
            if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &olen) == 0 &&
                soerr == 0) {
                break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        if (netsvc_sock_set_timeout(fd, P4_CONFIG_NET_CONNECT_TIMEOUT_MS) != 0) {
            close(fd);
            fd = -1;
        }
    }
    return fd;
}

static int netsvc_send_all(int fd, const uint8_t *buf, size_t len,
                           uint32_t timeout_ms)
{
    int64_t end_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t sent = 0;
    while (sent < len) {
        fd_set wfds;
        struct timeval tv;
        ssize_t n;
        int64_t left_us = end_us - esp_timer_get_time();
        if (left_us <= 0) {
            return -1;
        }
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = (long)(left_us / 1000000);
        tv.tv_usec = (long)(left_us % 1000000);
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            return -1;
        }
        n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* Frame pump state: linear buffer, parsed prefix-first. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t used;
} netsvc_rx_t;

/* Forward declaration for the dispatch below. */
static void netsvc_on_frame(uint8_t type, uint8_t flags,
                            const uint8_t *body, size_t body_len);

/* Feed bytes; returns 0 ok, -1 on protocol error. */
static int netsvc_rx_feed(netsvc_rx_t *rx, const uint8_t *data, size_t len)
{
    if (rx->used + len > rx->cap) {
        return -1;
    }
    memcpy(rx->buf + rx->used, data, len);
    rx->used += len;
    for (;;) {
        uint8_t type = 0;
        uint8_t flags = 0;
        uint32_t rem = 0;
        size_t hdrlen = 0;
        int rc = mqtt_decode_header(rx->buf, rx->used, &type, &flags,
                                    &rem, &hdrlen);
        if (rc == 0) {
            return 0; /* need more */
        }
        if (rc < 0) {
            return -1;
        }
        if (rem > P4_CONFIG_NET_PUBLISH_MAX_BYTES + 512u) {
            return -1;
        }
        if (hdrlen + rem > rx->used) {
            if (hdrlen + rem > rx->cap) {
                return -1;
            }
            return 0; /* need more */
        }
        netsvc_on_frame(type, flags, rx->buf + hdrlen, rem);
        {
            size_t consumed = hdrlen + rem;
            memmove(rx->buf, rx->buf + consumed, rx->used - consumed);
            rx->used -= consumed;
        }
    }
}

/* ========================================================================
 * Inbound dispatch (task context, lock held briefly per update)
 * ======================================================================== */

static void netsvc_store_last(const char *topic, size_t tlen,
                              const uint8_t *payload, size_t plen)
{
    size_t keep;
    uint8_t *slot;
    if (tlen + 1 > sizeof(s_net.last_topic)) {
        return;
    }
    keep = plen;
    if (keep > P4_CONFIG_NET_LASTMSG_BYTES) {
        keep = P4_CONFIG_NET_LASTMSG_BYTES;
    }
    slot = malloc(keep > 0 ? keep : 1);
    if (slot == NULL) {
        return;
    }
    if (keep > 0) {
        memcpy(slot, payload, keep);
    }
    netsvc_lock();
    memcpy(s_net.last_topic, topic, tlen);
    s_net.last_topic[tlen] = '\0';
    free(s_net.last_payload);
    s_net.last_payload = slot;
    s_net.last_len = keep;
    netsvc_unlock();
}

static bool netsvc_merge_pim_topic(const char *topic, size_t tlen,
                                   const uint8_t *payload, size_t plen)
{
    char dbname[72];
    bool alarms = false;
    char tmp[P4_CONFIG_SD_PATH_BYTES];
    FILE *f = NULL;
    shell_sd_session_t session;
    pim_count_t count = {0, 0};
    int rc = -1;
    if (tlen == strlen(NETSVC_PIM_ALARMS) &&
        memcmp(topic, NETSVC_PIM_ALARMS, tlen) == 0) {
        alarms = true;
    } else if (tlen > strlen(NETSVC_PIM_DB_PREFIX) &&
               memcmp(topic, NETSVC_PIM_DB_PREFIX,
                      strlen(NETSVC_PIM_DB_PREFIX)) == 0) {
        size_t nlen = tlen - strlen(NETSVC_PIM_DB_PREFIX);
        if (nlen >= sizeof(dbname)) {
            return false;
        }
        memcpy(dbname, topic + strlen(NETSVC_PIM_DB_PREFIX), nlen);
        dbname[nlen] = '\0';
        if (!netsvc_db_name_ok(dbname)) {
            return false;
        }
    } else {
        return false;
    }
    if (!netsvc_can_sync()) {
        return false;
    }
    if (plen > P4_CONFIG_PIM_RX_MAX_BYTES) {
        return false;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    if (!storage_check_free_space((uint64_t)plen + 4096u, 0, "net")) {
        shell_sd_end(&session, "net");
        return false;
    }
    if (storage_temp_path(tmp, sizeof(tmp), "mqtt") != ESP_OK) {
        shell_sd_end(&session, "net");
        return false;
    }
    f = fopen(tmp, "wb");
    if (f == NULL) {
        shell_sd_end(&session, "net");
        return false;
    }
    if (plen > 0 && fwrite(payload, 1, plen, f) != plen) {
        fclose(f);
        remove(tmp);
        shell_sd_end(&session, "net");
        return false;
    }
    fclose(f);
    f = fopen(tmp, "rb");
    if (f == NULL) {
        remove(tmp);
        shell_sd_end(&session, "net");
        return false;
    }
    if (alarms) {
        rc = pim_ics_merge_stream(f, &count);
    } else {
        rc = pim_vcf_merge_stream(f, dbname, &count);
    }
    fclose(f);
    remove(tmp);
    shell_sd_end(&session, "net");
    return rc == 0 && count.ok > 0;
}

static void netsvc_deliver(const char *topic, size_t tlen,
                           const uint8_t *payload, size_t plen)
{
    char ftopic[P4_CONFIG_NET_TOPIC_BYTES];
    char note[160];
    char hook[P4_CONFIG_NET_ONMSG_BYTES];
    bool hooked = false;
    if (tlen + 1 > sizeof(ftopic)) {
        return;
    }
    memcpy(ftopic, topic, tlen);
    ftopic[tlen] = '\0';
    netsvc_store_last(ftopic, tlen, payload, plen);
    if (s_host_ops.can_reveal_private != NULL && !netsvc_can_sync()) {
        /* Locked: still acked at the protocol layer, but never surfaced. */
        return;
    }
    netsvc_merge_pim_topic(ftopic, tlen, payload, plen);
    netsvc_lock();
    if (s_net.onmsg[0] != '\0' && s_host_ops.execute_async != NULL) {
        snprintf(hook, sizeof(hook), "%s", s_net.onmsg);
        hooked = true;
    }
    netsvc_unlock();
    if (hooked) {
        if (s_host_ops.set_env != NULL) {
            char lenbuf[24];
            snprintf(lenbuf, sizeof(lenbuf), "%u", (unsigned)plen);
            s_host_ops.set_env("NET_TOPIC", ftopic);
            s_host_ops.set_env("NET_LEN", lenbuf);
        }
        s_host_ops.execute_async(hook);
    } else {
        snprintf(note, sizeof(note), "net: %s", ftopic);
        netsvc_note(note);
    }
}

static void netsvc_on_frame(uint8_t type, uint8_t flags,
                            const uint8_t *body, size_t body_len)
{
    if (type == MQTT_PKT_PUBLISH) {
        mqtt_view_t topic = {NULL, 0};
        mqtt_view_t payload = {NULL, 0};
        uint16_t pkt_id = 0;
        if (mqtt_decode_publish(body, body_len, flags, &topic, &pkt_id,
                                &payload) == 0 && topic.data != NULL) {
            uint8_t qos = (uint8_t)((flags >> 1) & 0x03u);
            if (qos == 1 && s_net.sock >= 0) {
                uint8_t ack[4];
                if (mqtt_encode_puback(ack, sizeof(ack), pkt_id) == 4) {
                    (void)netsvc_send_all(s_net.sock, ack, 4, 5000);
                }
            }
            /* Views alias the RX buffer: copy the topic for delivery. */
            {
                char tcopy[P4_CONFIG_NET_TOPIC_BYTES];
                size_t tlen = topic.len;
                if (tlen < sizeof(tcopy)) {
                    memcpy(tcopy, topic.data, tlen);
                    netsvc_deliver(tcopy, tlen, payload.data, payload.len);
                }
            }
        }
    }
    /* CONNACK/SUBACK/PUBACK/PINGRESP are consumed synchronously by the
     * session driver below (out-of-band signals, not dispatched here). */
}

/* ========================================================================
 * Session driver (blocking socket calls, task context only)
 * ======================================================================== */

/* Await one frame of @p want_type. Returns body length or -1. */
static int netsvc_await_frame(int fd, netsvc_rx_t *rx, uint8_t want_type,
                              uint8_t *flags_out, uint32_t timeout_ms,
                              size_t *body_off_out)
{
    int64_t end_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        uint8_t type = 0;
        uint8_t flags = 0;
        uint32_t rem = 0;
        size_t hdrlen = 0;
        int rc;
        fd_set rfds;
        struct timeval tv;
        uint8_t chunk[512];
        ssize_t n;
        int64_t left_us = end_us - esp_timer_get_time();
        if (left_us <= 0) {
            return -1;
        }
        /* Drain complete frames already buffered. */
        rc = mqtt_decode_header(rx->buf, rx->used, &type, &flags, &rem, &hdrlen);
        if (rc == 1 && hdrlen + rem <= rx->used) {
            if (type != want_type) {
                /* Stray frame (e.g. PUBLISH racing SUBACK): dispatch it and
                 * keep waiting rather than desyncing the session. */
                netsvc_on_frame(type, flags, rx->buf + hdrlen, rem);
                memmove(rx->buf, rx->buf + hdrlen + rem, rx->used - hdrlen - rem);
                rx->used -= hdrlen + rem;
                continue;
            }
            if (flags_out != NULL) {
                *flags_out = flags;
            }
            if (body_off_out != NULL) {
                *body_off_out = hdrlen;
            }
            return (int)rem;
        }
        if (rc < 0 || rem > P4_CONFIG_NET_PUBLISH_MAX_BYTES + 512u) {
            return -1;
        }
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = (long)(left_us / 1000000);
        tv.tv_usec = (long)(left_us % 1000000);
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            return -1;
        }
        n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return -1;
        }
        if (netsvc_rx_feed(rx, chunk, (size_t)n) != 0) {
            return -1;
        }
    }
}

/* Consume one buffered frame body after netsvc_await_frame(). */
static void netsvc_consume_frame(netsvc_rx_t *rx, size_t body_off, size_t body_len)
{
    size_t total = body_off + body_len;
    if (total <= rx->used) {
        memmove(rx->buf, rx->buf + total, rx->used - total);
        rx->used -= total;
    }
}

static bool netsvc_load_profile(char *host, size_t host_size, int *port_out,
                                char *user, size_t user_size,
                                char *pass, size_t pass_size,
                                char *cid, size_t cid_size)
{
    char portbuf[8];
    if (netsvc_profile_get(NETSVC_PROFILE_BROKER, host, host_size) != ESP_OK ||
        host[0] == '\0') {
        return false;
    }
    *port_out = 1883;
    if (netsvc_profile_get(NETSVC_PROFILE_PORT, portbuf, sizeof(portbuf)) == ESP_OK &&
        portbuf[0] != '\0') {
        long p = strtol(portbuf, NULL, 10);
        if (p > 0 && p <= 65535) {
            *port_out = (int)p;
        }
    }
    netsvc_profile_get(NETSVC_PROFILE_USER, user, user_size);
    netsvc_profile_get(NETSVC_PROFILE_PASS, pass, pass_size);
    if (netsvc_profile_get(NETSVC_PROFILE_CLIENTID, cid, cid_size) != ESP_OK ||
        cid[0] == '\0') {
        uint32_t rnd = esp_random();
        snprintf(cid, cid_size, "p4mini-%04x", (unsigned)(rnd & 0xFFFFu));
        storage_ini_file_set(P4_CONFIG_NET_PROFILE_FILE, NETSVC_PROFILE_CLIENTID, cid);
    }
    return true;
}

/* Send SUBSCRIBE for every RAM-table filter. Returns false on transport loss. */
static bool netsvc_resubscribe(int fd, netsvc_rx_t *rx, uint8_t *tx, size_t txcap,
                               uint16_t *pkt_id)
{
    char filters[NETSVC_MAX_SUBS][P4_CONFIG_NET_TOPIC_BYTES];
    int count = 0;
    int i;
    netsvc_lock();
    count = s_net.sub_count;
    for (i = 0; i < count && i < NETSVC_MAX_SUBS; i++) {
        snprintf(filters[i], sizeof(filters[i]), "%s", s_net.subs[i]);
    }
    netsvc_unlock();
    for (i = 0; i < count; i++) {
        uint8_t flags = 0;
        size_t body_off = 0;
        uint16_t expect;
        int n;
        int bodylen;
        (*pkt_id)++;
        if (*pkt_id == 0) {
            (*pkt_id)++;
        }
        expect = *pkt_id;
        n = mqtt_encode_subscribe(tx, txcap, expect, filters[i], 1);
        if (n < 0 || netsvc_send_all(fd, tx, (size_t)n, 10000) != 0) {
            return false;
        }
        bodylen = netsvc_await_frame(fd, rx, MQTT_PKT_SUBACK, &flags, 10000, &body_off);
        if (bodylen < 3) {
            return false;
        }
        {
            uint16_t got = 0;
            uint8_t granted[4];
            int g = mqtt_decode_suback(rx->buf + body_off, (size_t)bodylen,
                                       &got, granted, sizeof(granted));
            netsvc_consume_frame(rx, body_off, (size_t)bodylen);
            if (g <= 0 || got != expect || granted[0] > 1) {
                return false;
            }
        }
    }
    return true;
}

/* Publish one journal record file; removes it on PUBACK. */
static bool netsvc_flush_one(int fd, netsvc_rx_t *rx, uint8_t *tx, size_t txcap,
                             const char *path, uint16_t *pkt_id)
{
    FILE *f = NULL;
    long fsize = 0;
    char *rec = NULL;
    char topic[P4_CONFIG_NET_TOPIC_BYTES];
    const uint8_t *payload = NULL;
    size_t paylen = 0;
    uint8_t qos = 1;
    shell_sd_session_t session;
    bool ok = false;
    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        shell_sd_end(&session, "net-flush");
        return true; /* vanished: treat as done */
    }
    {
        /* Size from stat: the FATFS VFS does not report fseek+ftell sizes. */
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size <= 0) {
            fclose(f);
            shell_sd_end(&session, "net-flush");
            return false;
        }
        if (st.st_size > (long)(P4_CONFIG_NET_PUBLISH_MAX_BYTES + 512u)) {
            /* Poison record: drop it rather than wedging the queue. */
            fclose(f);
            remove(path);
            shell_sd_end(&session, "net-flush");
            return true;
        }
        fsize = st.st_size;
    }
    rec = malloc((size_t)fsize);
    if (rec == NULL) {
        fclose(f);
        shell_sd_end(&session, "net-flush");
        return false;
    }
    if (fread(rec, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f);
        free(rec);
        shell_sd_end(&session, "net-flush");
        return false;
    }
    fclose(f);
    if (netsvc_outbox_decode(rec, (size_t)fsize, topic, sizeof(topic),
                             &payload, &paylen, &qos) != 0) {
        free(rec);
        shell_sd_end(&session, "net-flush");
        remove(path); /* damaged record: drop, do not wedge the queue */
        return true;
    }
    (*pkt_id)++;
    if (*pkt_id == 0) {
        (*pkt_id)++;
    }
    {
        int n = mqtt_encode_publish(tx, txcap, topic, payload, paylen, 1, *pkt_id, false);
        uint16_t expect = *pkt_id;
        uint8_t flags = 0;
        size_t body_off = 0;
        int bodylen;
        if (n < 0 || netsvc_send_all(fd, tx, (size_t)n, 10000) != 0) {
            free(rec);
            shell_sd_end(&session, "net-flush");
            return false;
        }
        bodylen = netsvc_await_frame(fd, rx, MQTT_PKT_PUBACK, &flags, 10000, &body_off);
        if (bodylen != 2) {
            free(rec);
            shell_sd_end(&session, "net-flush");
            return false;
        }
        {
            uint16_t got = (uint16_t)(((uint16_t)rx->buf[body_off] << 8) |
                                      rx->buf[body_off + 1]);
            netsvc_consume_frame(rx, body_off, (size_t)bodylen);
            if (got != expect) {
                free(rec);
                shell_sd_end(&session, "net-flush");
                return false;
            }
        }
    }
    free(rec);
    remove(path);
    shell_sd_end(&session, "net-flush");
    ok = true;
    return ok;
}

static int netsvc_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Flush the journal oldest-first (lexicographic ids are chronological). */
static bool netsvc_flush_outbox(int fd, netsvc_rx_t *rx, uint8_t *tx,
                                size_t txcap, uint16_t *pkt_id)
{
    DIR *d = NULL;
    shell_sd_session_t session;
    char (*names)[64] = NULL;
    int count = 0;
    int i;
    bool ok = true;
    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    d = opendir(P4_CONFIG_NET_OUTBOX_DIR);
    if (d == NULL) {
        shell_sd_end(&session, "net-flush");
        return true;
    }
    names = malloc(sizeof(*names) * (P4_CONFIG_NET_OUTBOX_MAX + 1));
    if (names == NULL) {
        closedir(d);
        shell_sd_end(&session, "net-flush");
        return false;
    }
    {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && count <= P4_CONFIG_NET_OUTBOX_MAX) {
            size_t nlen = strlen(e->d_name);
            if (nlen < 5 || nlen >= sizeof(*names) ||
                strcmp(e->d_name + nlen - 4, ".MSG") != 0) {
                continue;
            }
            snprintf(names[count], sizeof(*names), "%.*s",
                     (int)(sizeof(*names) - 1), e->d_name);
            count++;
        }
    }
    closedir(d);
    shell_sd_end(&session, "net-flush");
    qsort(names, (size_t)count, sizeof(*names), netsvc_name_cmp);
    for (i = 0; i < count; i++) {
        char path[P4_CONFIG_SD_PATH_BYTES];
        snprintf(path, sizeof(path), "%s/%s", P4_CONFIG_NET_OUTBOX_DIR, names[i]);
        if (!netsvc_flush_one(fd, rx, tx, txcap, path, pkt_id)) {
            ok = false;
            break;
        }
    }
    free(names);
    return ok;
}

/* CONNECT handshake: returns the fd or -1. */
static int netsvc_session_open(const char *host, int port,
                               const char *user, const char *pass,
                               const char *cid, uint16_t keepalive,
                               netsvc_rx_t *rx, uint8_t *tx, size_t txcap,
                               uint16_t *pkt_id)
{
    mqtt_connect_args_t args;
    uint8_t flags = 0;
    size_t body_off = 0;
    int bodylen;
    uint8_t rc = 0;
    int fd;
    int n;
    memset(&args, 0, sizeof(args));
    args.client_id = cid;
    args.keepalive_s = keepalive;
    args.clean_session = true;
    args.username = (user != NULL && user[0] != '\0') ? user : NULL;
    args.password = (pass != NULL && pass[0] != '\0') ? pass : NULL;
    fd = netsvc_tcp_dial(host, port, P4_CONFIG_NET_CONNECT_TIMEOUT_MS);
    if (fd < 0) {
        return -1;
    }
    rx->used = 0;
    n = mqtt_encode_connect(tx, txcap, &args);
    if (n < 0 || netsvc_send_all(fd, tx, (size_t)n, 10000) != 0) {
        close(fd);
        return -1;
    }
    bodylen = netsvc_await_frame(fd, rx, MQTT_PKT_CONNACK, &flags, 10000, &body_off);
    if (bodylen != 2 ||
        mqtt_decode_connack(rx->buf + body_off, (size_t)bodylen, &rc) != 0 ||
        rc != MQTT_RC_ACCEPTED) {
        close(fd);
        return -1;
    }
    netsvc_consume_frame(rx, body_off, (size_t)bodylen);
    *pkt_id = (uint16_t)(1 + (esp_random() % 60000u));
    if (!netsvc_resubscribe(fd, rx, tx, txcap, pkt_id)) {
        close(fd);
        return -1;
    }
    if (!netsvc_flush_outbox(fd, rx, tx, txcap, pkt_id)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Connected pump: frames, keepalive, out-of-band acks. Runs until loss. */
static void netsvc_session_pump(int fd, netsvc_rx_t *rx, uint16_t keepalive_s)
{
    for (;;) {
        fd_set rfds;
        struct timeval tv;
        uint8_t chunk[512];
        ssize_t n;
        int r;
        int64_t now_us = esp_timer_get_time();
        int64_t idle_us = now_us - s_net.last_io_us;
        if (!networking_wifi_is_connected() || netsvc_ota_active()) {
            return;
        }
        if (idle_us > (int64_t)keepalive_s * 1000000 / 2) {
            uint8_t ping[2];
            if (mqtt_encode_pingreq(ping, sizeof(ping)) != 2 ||
                netsvc_send_all(fd, ping, 2, 5000) != 0) {
                return;
            }
            s_net.last_io_us = now_us;
        }
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            return;
        }
        if (r == 0) {
            continue; /* tick: re-evaluate the gates above */
        }
        n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return;
        }
        s_net.last_io_us = esp_timer_get_time();
        if (netsvc_rx_feed(rx, chunk, (size_t)n) != 0) {
            return;
        }
    }
}

static void netsvc_task(void *arg)
{
    uint8_t *rxbuf = NULL;
    uint8_t *txbuf = NULL;
    netsvc_rx_t rx;
    unsigned attempt = 0;
    uint16_t pkt_id = 1;
    (void)arg;
    rxbuf = malloc(P4_CONFIG_NET_PUBLISH_MAX_BYTES + 1024u);
    txbuf = malloc(P4_CONFIG_NET_PUBLISH_MAX_BYTES + 1024u);
    if (rxbuf == NULL || txbuf == NULL) {
        free(rxbuf);
        free(txbuf);
        vTaskDelete(NULL);
        return;
    }
    rx.buf = rxbuf;
    rx.cap = P4_CONFIG_NET_PUBLISH_MAX_BYTES + 1024u;
    rx.used = 0;
    for (;;) {
        char host[128];
        char user[64];
        char pass[P4_CONFIG_NET_PASSWORD_BYTES];
        char cid[48];
        int port = 1883;
        bool enabled;
        netsvc_lock();
        enabled = s_net.enabled;
        netsvc_unlock();
        /* Gate every iteration before any socket/heap/SD touch: Wi-Fi down
         * (sleep teardown), OTA flash (PSRAM off-limits), or disabled. */
        if (!enabled || !networking_wifi_is_connected() || netsvc_ota_active()) {
            netsvc_lock();
            netsvc_close_socket();
            s_net.ota_paused = netsvc_ota_active();
            netsvc_unlock();
            vTaskDelay(pdMS_TO_TICKS(1000));
            attempt = 0;
            continue;
        }
        {
            char pbuf[sizeof(pass)];
            memset(pbuf, 0, sizeof(pbuf));
            if (!netsvc_load_profile(host, sizeof(host), &port,
                                     user, sizeof(user), pbuf, sizeof(pbuf),
                                     cid, sizeof(cid))) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            s_net.sock = netsvc_session_open(host, port, user, pbuf, cid,
                                             P4_CONFIG_NET_KEEPALIVE_S,
                                             &rx, txbuf,
                                             P4_CONFIG_NET_PUBLISH_MAX_BYTES + 1024u,
                                             &pkt_id);
            memset(pbuf, 0, sizeof(pbuf));
        }
        if (s_net.sock < 0) {
            uint32_t wait = netsvc_backoff_ms(attempt, P4_CONFIG_NET_BACKOFF_BASE_MS,
                                              P4_CONFIG_NET_BACKOFF_CAP_MS);
            attempt++;
            vTaskDelay(pdMS_TO_TICKS(wait));
            continue;
        }
        attempt = 0;
        netsvc_lock();
        s_net.connected = true;
        s_net.ota_paused = false;
        s_net.last_io_us = esp_timer_get_time();
        netsvc_unlock();
        netsvc_note("net: connected");
        (void)netsvc_session_pump(s_net.sock, &rx, P4_CONFIG_NET_KEEPALIVE_S);
        netsvc_lock();
        netsvc_close_socket();
        netsvc_unlock();
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void netsvc_register_host_ops(const netsvc_host_ops_t *ops)
{
    if (ops == NULL) {
        memset(&s_host_ops, 0, sizeof(s_host_ops));
    } else {
        s_host_ops = *ops;
    }
}

void netsvc_ensure_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;
    memset(&s_net, 0, sizeof(s_net));
    s_net.sock = -1;
    s_net.lock = xSemaphoreCreateMutex();
    if (s_net.lock == NULL) {
        s_inited = false;
        return;
    }
    if (xTaskCreate(netsvc_task, "netsvc", P4_CONFIG_NET_TASK_STACK, NULL,
                    tskIDLE_PRIORITY + 1, &s_net.task) != pdPASS) {
        vSemaphoreDelete(s_net.lock);
        s_net.lock = NULL;
        s_net.task = NULL;
        s_inited = false;
    }
}

bool netsvc_is_connected(void)
{
    bool up = false;
    netsvc_lock();
    up = s_net.connected && s_net.sock >= 0;
    netsvc_unlock();
    return up;
}

bool netsvc_is_active(void)
{
    bool active = false;
    netsvc_lock();
    active = s_inited && s_net.enabled;
    netsvc_unlock();
    return active;
}

const char *netsvc_state_string(void)
{
    if (!s_inited) {
        return "idle";
    }
    if (s_net.ota_paused) {
        return "paused-ota";
    }
    if (!s_net.enabled) {
        return "disabled";
    }
    if (s_net.connected) {
        return "connected";
    }
    if (!networking_wifi_is_connected()) {
        return "waiting-wifi";
    }
    return "backoff";
}

esp_err_t netsvc_set_broker(const char *host, int port, const char *user)
{
    char portbuf[8];
    if (host == NULL || host[0] == '\0' || port <= 0 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    if (storage_ini_file_set(P4_CONFIG_NET_PROFILE_FILE,
                             NETSVC_PROFILE_BROKER, host) != ESP_OK) {
        return ESP_FAIL;
    }
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    if (storage_ini_file_set(P4_CONFIG_NET_PROFILE_FILE,
                             NETSVC_PROFILE_PORT, portbuf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (storage_ini_file_set(P4_CONFIG_NET_PROFILE_FILE, NETSVC_PROFILE_USER,
                             (user != NULL) ? user : "") != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t netsvc_get_broker(char *host, size_t host_size, int *port_out,
                            char *user, size_t user_size)
{
    char portbuf[8];
    esp_err_t have_host;

    if (host != NULL && host_size > 0) {
        host[0] = '\0';
    }
    if (user != NULL && user_size > 0) {
        user[0] = '\0';
    }
    if (port_out != NULL) {
        *port_out = 1883;
    }
    if (host == NULL || host_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    have_host = netsvc_profile_get(NETSVC_PROFILE_BROKER, host, host_size);
    if (user != NULL && user_size > 0) {
        (void)netsvc_profile_get(NETSVC_PROFILE_USER, user, user_size);
    }
    if (port_out != NULL &&
        netsvc_profile_get(NETSVC_PROFILE_PORT, portbuf, sizeof(portbuf)) == ESP_OK &&
        portbuf[0] != '\0') {
        long p = strtol(portbuf, NULL, 10);
        if (p > 0 && p <= 65535) {
            *port_out = (int)p;
        }
    }
    return (have_host == ESP_OK && host[0] != '\0') ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t netsvc_set_password(const char *password)
{
    if (password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(password) >= P4_CONFIG_NET_PASSWORD_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (storage_ini_file_set(P4_CONFIG_NET_PROFILE_FILE, NETSVC_PROFILE_PASS,
                             password) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t netsvc_connect(void)
{
    netsvc_ensure_init();
    if (!s_inited) {
        return ESP_FAIL;
    }
    netsvc_lock();
    s_net.enabled = true;
    s_net.attempt = 0;
    netsvc_unlock();
    return ESP_OK;
}

esp_err_t netsvc_disconnect(void)
{
    netsvc_lock();
    s_net.enabled = false;
    if (s_net.sock >= 0) {
        shutdown(s_net.sock, SHUT_RDWR);
    }
    netsvc_unlock();
    return ESP_OK;
}

esp_err_t netsvc_subscribe(const char *filter)
{
    size_t i;
    if (filter == NULL || filter[0] == '\0' ||
        strlen(filter) >= P4_CONFIG_NET_TOPIC_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strchr(filter, '#') != NULL &&
        (strchr(filter, '#') != filter + strlen(filter) - 1)) {
        /* `#` is only meaningful as the final level; keep the table clean. */
        return ESP_ERR_INVALID_ARG;
    }
    netsvc_lock();
    for (i = 0; i < (size_t)s_net.sub_count && i < NETSVC_MAX_SUBS; i++) {
        if (strcmp(s_net.subs[i], filter) == 0) {
            netsvc_unlock();
            return ESP_OK;
        }
    }
    if (s_net.sub_count >= NETSVC_MAX_SUBS) {
        netsvc_unlock();
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_net.subs[s_net.sub_count], sizeof(s_net.subs[0]), "%s", filter);
    s_net.sub_count++;
    netsvc_unlock();
    return ESP_OK;
}

esp_err_t netsvc_unsubscribe(const char *filter)
{
    int i;
    int j;
    if (filter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    netsvc_lock();
    for (i = 0; i < s_net.sub_count; i++) {
        if (strcmp(s_net.subs[i], filter) == 0) {
            for (j = i; j + 1 < s_net.sub_count; j++) {
                memmove(s_net.subs[j], s_net.subs[j + 1],
                        sizeof(s_net.subs[j]));
            }
            s_net.sub_count--;
            netsvc_unlock();
            return ESP_OK;
        }
    }
    netsvc_unlock();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t netsvc_publish(const char *topic, const uint8_t *payload, size_t len)
{
    if (topic == NULL || topic[0] == '\0' ||
        strlen(topic) >= P4_CONFIG_NET_TOPIC_BYTES ||
        len > P4_CONFIG_NET_PUBLISH_MAX_BYTES ||
        (len > 0 && payload == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Journal-first (write-through outbox): the record hits SD, then the
     * wire on flush; offline publishes only journal. */
    return netsvc_outbox_append(topic, payload, len);
}

bool netsvc_last_msg(char *topic_out, size_t topic_size,
                     uint8_t *payload_out, size_t *payload_len_inout,
                     size_t payload_cap)
{
    bool have = false;
    if (topic_out == NULL || topic_size == 0 || payload_len_inout == NULL) {
        return false;
    }
    netsvc_lock();
    if (s_net.last_topic[0] != '\0') {
        size_t copy = s_net.last_len;
        snprintf(topic_out, topic_size, "%s", s_net.last_topic);
        if (payload_out != NULL && payload_cap > 0) {
            if (copy > payload_cap) {
                copy = payload_cap;
            }
            if (copy > 0 && s_net.last_payload != NULL) {
                memcpy(payload_out, s_net.last_payload, copy);
            }
        } else {
            copy = 0;
        }
        *payload_len_inout = copy;
        have = true;
    }
    netsvc_unlock();
    return have;
}

esp_err_t netsvc_set_onmsg(const char *line)
{
    netsvc_lock();
    if (line == NULL || line[0] == '\0') {
        s_net.onmsg[0] = '\0';
    } else {
        snprintf(s_net.onmsg, sizeof(s_net.onmsg), "%s", line);
    }
    netsvc_unlock();
    return ESP_OK;
}

int netsvc_outbox_count(void)
{
    return netsvc_outbox_usage(NULL);
}

esp_err_t netsvc_outbox_purge(void)
{
    DIR *d = NULL;
    shell_sd_session_t session;
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    d = opendir(P4_CONFIG_NET_OUTBOX_DIR);
    if (d == NULL) {
        shell_sd_end(&session, "net-purge");
        return ESP_OK;
    }
    {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t nlen = strlen(e->d_name);
            char full[P4_CONFIG_SD_PATH_BYTES + P4_CONFIG_LFN_BYTES];
            if (nlen < 5 || strcmp(e->d_name + nlen - 4, ".MSG") != 0) {
                continue;
            }
            snprintf(full, sizeof(full), "%s/%s", P4_CONFIG_NET_OUTBOX_DIR, e->d_name);
            remove(full);
        }
    }
    closedir(d);
    shell_sd_end(&session, "net-purge");
    return ESP_OK;
}

int netsvc_sub_count(void)
{
    int n = 0;
    netsvc_lock();
    n = s_net.sub_count;
    netsvc_unlock();
    return n;
}

bool netsvc_sub_get(int index, char *out, size_t out_size)
{
    bool ok = false;
    if (out == NULL || out_size == 0) {
        return false;
    }
    netsvc_lock();
    if (index >= 0 && index < s_net.sub_count) {
        snprintf(out, out_size, "%s", s_net.subs[index]);
        ok = true;
    }
    netsvc_unlock();
    return ok;
}
