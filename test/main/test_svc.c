/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_svc.c
 * @brief Unit tests for batch sync + service cores (batch/sync.c, svc.c).
 *
 * Covers the sync name rule, mutex take/release/foreign-refusal semantics,
 * event signal/wait-once/timeout behavior, and the service registry guards
 * that need no SD card (unknown names, bad names, empty table). Registry
 * file I/O, supervision restarts, and boot hooks are hardware-verified
 * through the batch suite (they need the SD card and the bg pool).
 */

#include "unity.h"
#include "batch.h"

#include <string.h>

void test_sync_name_ok(void)
{
    TEST_ASSERT_TRUE(sync_name_ok("m"));
    TEST_ASSERT_TRUE(sync_name_ok("MoodTick_1-2"));
    TEST_ASSERT_FALSE(sync_name_ok(NULL));
    TEST_ASSERT_FALSE(sync_name_ok(""));
    TEST_ASSERT_FALSE(sync_name_ok("has space"));
    TEST_ASSERT_FALSE(sync_name_ok("semi;colon"));
    TEST_ASSERT_FALSE(sync_name_ok("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
}

void test_sync_mutex_take_release(void)
{
    /* Fresh name: take, recursive re-take by the owner, release. */
    TEST_ASSERT_EQUAL(ESP_OK, sync_mutex_lock("P4T_M1", 0, 0));
    TEST_ASSERT_EQUAL(ESP_OK, sync_mutex_lock("P4T_M1", 0, 0));
    TEST_ASSERT_EQUAL(ESP_OK, sync_mutex_unlock("P4T_M1", 0));
    /* Unknown and bad names refuse without touching the table. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, sync_mutex_unlock("P4T_NOPE", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sync_mutex_unlock("", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sync_mutex_lock(NULL, 0, 0));
}

void test_sync_mutex_foreign(void)
{
    /* Slot 1 owns; slot 0 supervises (may release); a third party may not.
     * Only slots 0..BG_TASKS exist, so "foreign" here is exercised through
     * the owner path plus release_owned. */
    TEST_ASSERT_EQUAL(ESP_OK, sync_mutex_lock("P4T_M2", 0, 1));
    TEST_ASSERT_EQUAL(ESP_OK, sync_mutex_unlock("P4T_M2", 0));
    TEST_ASSERT_EQUAL(ESP_OK, sync_mutex_lock("P4T_M2", 0, 1));
    sync_release_owned(1);
    /* After the owner's death the mutex is free again (no wedging). */
    TEST_ASSERT_EQUAL(ESP_OK, sync_mutex_lock("P4T_M2", 0, 0));
    TEST_ASSERT_EQUAL(ESP_OK, sync_mutex_unlock("P4T_M2", 0));
    sync_release_owned(99);
    sync_release_owned(-1);
}

void test_sync_event_once_and_timeout(void)
{
    /* Sticky until taken, exactly once. */
    TEST_ASSERT_EQUAL(ESP_OK, sync_event_signal("P4T_E1"));
    TEST_ASSERT_EQUAL(ESP_OK, sync_event_wait("P4T_E1", 0, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, sync_event_wait("P4T_E1", 150, 0));
    /* Unknown event waits (created on demand) then times out. */
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, sync_event_wait("P4T_E2", 150, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sync_event_signal(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sync_event_wait("", 0, 0));
}

void test_svc_registry_guards(void)
{
    svc_entry_t e;

    /* Empty table reads clean. */
    TEST_ASSERT_EQUAL_INT(0, svc_count());
    e = svc_describe(0);
    TEST_ASSERT_FALSE(e.used);
    e = svc_describe(99);
    TEST_ASSERT_FALSE(e.used);
    /* Unknown names fail without SD card access. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, svc_start("P4T_NOSVC"));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, svc_stop("P4T_NOSVC"));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, svc_remove("P4T_NOSVC"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, svc_start(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, svc_stop(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, svc_remove(NULL));
    /* Name/line validation happens before any file I/O. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, svc_install("bad name!", "x", false, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, svc_install("ok", "", false, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, svc_install("ok", NULL, false, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, svc_install(NULL, "x", false, false));
}

void test_svc_bg_slot_helpers(void)
{
    /* Test task runs unbound: foreground slot, deadline/poll harmless. */
    TEST_ASSERT_EQUAL_INT(0, batch_bg_current_slot());
    batch_bg_set_deadline(0, 0);
    batch_bg_set_deadline(99, 1234);
    batch_bg_poll_timeout();
}
