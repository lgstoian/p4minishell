/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file header_notify_queue.c
 * @brief Pure fixed-size FIFO for header notifications.
 */

#include "header_notify_queue.h"

#include <stdio.h>
#include <string.h>

void header_notify_queue_init(header_notify_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    memset(queue, 0, sizeof(*queue));
}

void header_notify_queue_clear(header_notify_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    queue->head = 0;
    queue->count = 0;
}

int header_notify_queue_count(const header_notify_queue_t *queue)
{
    return (queue != NULL) ? queue->count : 0;
}

bool header_notify_queue_empty(const header_notify_queue_t *queue)
{
    return header_notify_queue_count(queue) == 0;
}

bool header_notify_queue_push(header_notify_queue_t *queue, const char *text,
                              header_notify_level_t level, uint32_t timeout_ms)
{
    int tail;

    if (queue == NULL || text == NULL || text[0] == '\0') {
        return false;
    }

    if (queue->count == P4_CONFIG_HEADER_NOTIFY_QUEUE) {
        /* Full: drop the oldest queued entry so the newest always enters. */
        queue->head = (queue->head + 1) % P4_CONFIG_HEADER_NOTIFY_QUEUE;
        queue->count--;
    }

    tail = (queue->head + queue->count) % P4_CONFIG_HEADER_NOTIFY_QUEUE;
    snprintf(queue->items[tail].text, sizeof(queue->items[tail].text), "%s", text);
    queue->items[tail].level = level;
    queue->items[tail].timeout_ms = timeout_ms;
    queue->count++;
    return true;
}

bool header_notify_queue_pop(header_notify_queue_t *queue, header_notify_item_t *out)
{
    if (queue == NULL || queue->count == 0) {
        return false;
    }
    if (out != NULL) {
        *out = queue->items[queue->head];
    }
    queue->head = (queue->head + 1) % P4_CONFIG_HEADER_NOTIFY_QUEUE;
    queue->count--;
    return true;
}
