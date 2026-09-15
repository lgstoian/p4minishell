/**
 * @file header_notify_queue.h
 * @brief Pure fixed-size FIFO for header notifications (no LVGL, no I/O).
 *
 * The header shows one notification at a time. Without a queue a later (often
 * trivial) message would overwrite an earlier important one — an alarm or OTA
 * alert could be lost. This module is the single source of the queue mechanics
 * (order, overflow policy, clear) and is unit-tested; header.c owns the widget
 * and the display timer.
 *
 * Overflow policy: when full, the OLDEST QUEUED entry is dropped so the newest
 * alert always enters. The entry currently displayed is never in this queue
 * (header.c holds it separately), so it can never be evicted by overflow.
 */

#ifndef P4MINISHELL_HEADER_NOTIFY_QUEUE_H
#define P4MINISHELL_HEADER_NOTIFY_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "header.h"
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One queued notification. */
typedef struct {
    char text[P4_CONFIG_HEADER_NOTIFICATION_BYTES];
    header_notify_level_t level;
    uint32_t timeout_ms;
} header_notify_item_t;

/** Fixed-size ring. Treat as opaque; use the functions below. */
typedef struct {
    header_notify_item_t items[P4_CONFIG_HEADER_NOTIFY_QUEUE];
    int head;  /**< Index of the oldest item. */
    int count; /**< Number of queued items (0..P4_CONFIG_HEADER_NOTIFY_QUEUE). */
} header_notify_queue_t;

/** Reset to empty. */
void header_notify_queue_init(header_notify_queue_t *queue);

/** Drop every queued item. */
void header_notify_queue_clear(header_notify_queue_t *queue);

/** Number of queued items. */
int header_notify_queue_count(const header_notify_queue_t *queue);

/** @return true when nothing is queued. */
bool header_notify_queue_empty(const header_notify_queue_t *queue);

/**
 * Append an item. On overflow the oldest queued item is dropped. A NULL or
 * empty @p text is ignored (returns false) so the queue never holds a blank
 * entry; use header_notify_queue_clear() to empty it.
 */
bool header_notify_queue_push(header_notify_queue_t *queue, const char *text,
                              header_notify_level_t level, uint32_t timeout_ms);

/**
 * Remove the oldest item. When @p out is non-NULL it receives a copy.
 * @return false when the queue is empty.
 */
bool header_notify_queue_pop(header_notify_queue_t *queue, header_notify_item_t *out);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_HEADER_NOTIFY_QUEUE_H */
