/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file event_bus.h
 * @brief Lightweight pub/sub event bus.
 *
 * Fixed-capacity storage is allocated once, lazily, from PSRAM (internal RAM
 * as a fallback) rather than BSS, keeping the scarce boot-time internal RAM
 * free. ISR-safe via critical sections. Topic matching is exact string
 * compare. Subscribers get a small queue; events are dropped (oldest first)
 * when a subscriber's queue is full.
 */

#ifndef P4MINISHELL_EVENT_BUS_H
#define P4MINISHELL_EVENT_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One queued event awaiting a subscriber. */
typedef struct {
    char data[P4_CONFIG_EVENT_BUS_DATA_MAX];  /**< JSON payload. */
    uint16_t data_len;                         /**< Bytes of valid data. */
    uint32_t sequence;                         /**< Global sequence number. */
} event_bus_event_t;

/** Opaque subscriber queue handle. */
typedef struct {
    int topic_index;                           /**< Index into topic table. */
    event_bus_event_t events[P4_CONFIG_EVENT_BUS_QUEUE_DEPTH];
    volatile int head;
    volatile int count;
} event_bus_queue_t;

/** Subscribe to a topic. @return queue handle, or NULL if full. */
event_bus_queue_t *event_bus_subscribe(const char *topic);

/** Unsubscribe (drops the queue contents). */
void event_bus_unsubscribe(event_bus_queue_t *queue);

/** Publish an event to all subscribers of a topic.
 *  @return true if delivered to at least one subscriber. */
bool event_bus_publish(const char *topic, const char *data_json, size_t data_len);

/** Poll the next event from a subscriber queue.
 *  @return true if an event was retrieved. */
bool event_bus_poll(event_bus_queue_t *queue, char *out_data, size_t *out_len);

/** Number of events queued for a subscriber. */
int event_bus_queue_count(const event_bus_queue_t *queue);

/** Total subscribers across all topics. */
int event_bus_subscriber_count(void);

/** Global event sequence counter (published since boot). */
uint32_t event_bus_sequence(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EVENT_BUS_H */
