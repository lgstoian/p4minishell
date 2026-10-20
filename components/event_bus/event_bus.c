/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file event_bus.c
 * @brief Lightweight pub/sub event bus — static storage, ISR-safe.
 */

#include "event_bus.h"
#include <string.h>
#include "p4heap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_TOPICS      P4_CONFIG_EVENT_BUS_TOPICS
#define MAX_SUBS        P4_CONFIG_EVENT_BUS_SUBSCRIBERS
#define QUEUE_DEPTH     P4_CONFIG_EVENT_BUS_QUEUE_DEPTH
#define DATA_MAX        P4_CONFIG_EVENT_BUS_DATA_MAX
#define TOPIC_LEN       P4_CONFIG_EVENT_BUS_TOPIC_LEN

/* ---- Topic table ---- */
typedef struct {
    char name[TOPIC_LEN];
    int subscriber_indices[MAX_SUBS];  /**< Indices into subscriber pool, -1 = empty. */
    int sub_count;
} topic_entry_t;

/* Bulk tables live in PSRAM, allocated on first use, NOT in .bss: internal
 * SRAM is the scarce boot resource (see ai-context.md "bulk tables stay OUT
 * of .bss"). p4heap_calloc_any is PSRAM-first with an internal fallback so
 * the bus still works where PSRAM is unavailable (unit tests). */
static topic_entry_t *s_topics = NULL;
static int s_topic_count = 0;

/* ---- Subscriber pool ---- */
static event_bus_queue_t *s_subscribers = NULL;
static int s_sub_count = 0;

/* ---- Global sequence ---- */
static volatile uint32_t s_sequence = 0;

/* Lazily allocate the topic table and subscriber pool. Safe to call from a
 * task context (never inside a critical section). Returns false on OOM. */
static bool event_bus_ensure_pool(void)
{
    if (s_topics == NULL) {
        s_topics = p4heap_calloc_any(MAX_TOPICS, sizeof(*s_topics));
        if (s_topics == NULL) {
            return false;
        }
    }
    if (s_subscribers == NULL) {
        event_bus_queue_t *pool = p4heap_calloc_any(MAX_SUBS, sizeof(*pool));
        if (pool == NULL) {
            return false;
        }
        /* calloc zeroes topic_index to 0, but 0 is a valid topic index;
         * -1 is the "free slot" sentinel. */
        for (int i = 0; i < MAX_SUBS; i++) {
            pool[i].topic_index = -1;
        }
        s_subscribers = pool;
    }
    return true;
}

/* ---- Helpers ---- */

static int topic_find(const char *topic)
{
    for (int i = 0; i < s_topic_count; i++) {
        if (strncmp(s_topics[i].name, topic, TOPIC_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static int topic_acquire(const char *topic)
{
    int idx = topic_find(topic);
    if (idx >= 0) return idx;
    if (s_topic_count >= MAX_TOPICS) return -1;
    idx = s_topic_count++;
    memset(&s_topics[idx], 0, sizeof(topic_entry_t));
    strncpy(s_topics[idx].name, topic, TOPIC_LEN - 1);
    s_topics[idx].name[TOPIC_LEN - 1] = '\0';
    for (int i = 0; i < MAX_SUBS; i++) {
        s_topics[idx].subscriber_indices[i] = -1;
    }
    s_topics[idx].sub_count = 0;
    return idx;
}

/* ---- Public API ---- */

event_bus_queue_t *event_bus_subscribe(const char *topic)
{
    if (topic == NULL || topic[0] == '\0') return NULL;
    if (!event_bus_ensure_pool()) return NULL;

    taskENTER_CRITICAL(NULL);

    int topic_idx = topic_acquire(topic);
    if (topic_idx < 0 || s_topics[topic_idx].sub_count >= MAX_SUBS) {
        taskEXIT_CRITICAL(NULL);
        return NULL;
    }

    /* Find a free subscriber slot. */
    int sub_slot = -1;
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_subscribers[i].topic_index < 0) {
            sub_slot = i;
            break;
        }
    }
    if (sub_slot < 0) {
        taskEXIT_CRITICAL(NULL);
        return NULL;
    }

    event_bus_queue_t *q = &s_subscribers[sub_slot];
    memset(q, 0, sizeof(*q));
    q->topic_index = topic_idx;
    q->head = 0;
    q->count = 0;

    /* Register subscriber in topic's list. */
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_topics[topic_idx].subscriber_indices[i] < 0) {
            s_topics[topic_idx].subscriber_indices[i] = sub_slot;
            s_topics[topic_idx].sub_count++;
            break;
        }
    }
    if (s_sub_count < MAX_SUBS) s_sub_count++;

    taskEXIT_CRITICAL(NULL);
    return q;
}

void event_bus_unsubscribe(event_bus_queue_t *queue)
{
    if (queue == NULL || queue->topic_index < 0) return;
    if (s_topics == NULL || s_subscribers == NULL) return;

    taskENTER_CRITICAL(NULL);

    int topic_idx = queue->topic_index;
    /* Remove from topic's subscriber list. */
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_topics[topic_idx].subscriber_indices[i] == (int)(queue - s_subscribers)) {
            s_topics[topic_idx].subscriber_indices[i] = -1;
            s_topics[topic_idx].sub_count--;
            break;
        }
    }
    queue->topic_index = -1;
    queue->count = 0;
    queue->head = 0;
    if (s_sub_count > 0) s_sub_count--;

    taskEXIT_CRITICAL(NULL);
}

bool event_bus_publish(const char *topic, const char *data_json, size_t data_len)
{
    if (topic == NULL) return false;
    if (!event_bus_ensure_pool()) return false;

    taskENTER_CRITICAL(NULL);

    int topic_idx = topic_find(topic);
    if (topic_idx < 0) {
        taskEXIT_CRITICAL(NULL);
        return false;
    }

    uint32_t seq = ++s_sequence;
    bool delivered = false;

    for (int i = 0; i < MAX_SUBS; i++) {
        int sub_slot = s_topics[topic_idx].subscriber_indices[i];
        if (sub_slot < 0) continue;
        event_bus_queue_t *q = &s_subscribers[sub_slot];

        if (q->count >= QUEUE_DEPTH) {
            /* Drop oldest. */
            q->head = (q->head + 1) % QUEUE_DEPTH;
            q->count--;
        }

        int write_idx = (q->head + q->count) % QUEUE_DEPTH;
        event_bus_event_t *ev = &q->events[write_idx];

        size_t copy_len = data_len < (size_t)DATA_MAX ? data_len : (size_t)DATA_MAX;
        if (data_json && copy_len > 0) {
            memcpy(ev->data, data_json, copy_len);
        }
        ev->data_len = (uint16_t)copy_len;
        ev->sequence = seq;
        q->count++;
        delivered = true;
    }

    taskEXIT_CRITICAL(NULL);
    return delivered;
}

bool event_bus_poll(event_bus_queue_t *queue, char *out_data, size_t *out_len)
{
    if (queue == NULL || out_data == NULL || queue->topic_index < 0) return false;

    taskENTER_CRITICAL(NULL);

    if (queue->count <= 0) {
        taskEXIT_CRITICAL(NULL);
        return false;
    }

    event_bus_event_t *ev = &queue->events[queue->head];
    memcpy(out_data, ev->data, ev->data_len);
    if (out_len) *out_len = ev->data_len;
    queue->head = (queue->head + 1) % QUEUE_DEPTH;
    queue->count--;

    taskEXIT_CRITICAL(NULL);
    return true;
}

int event_bus_queue_count(const event_bus_queue_t *queue)
{
    if (queue == NULL) return 0;
    return queue->count;
}

int event_bus_subscriber_count(void)
{
    return s_sub_count;
}

uint32_t event_bus_sequence(void)
{
    return s_sequence;
}
