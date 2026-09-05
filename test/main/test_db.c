/**
 * @file test_db.c
 * @brief Unit tests for the Palm-OS-style SD record store (components/db).
 *
 * Covers the pure, hardware-free validation logic. The full add/get/set/del/
 * find/purge/export/import cycle and the applib wrappers are exercised
 * on-hardware (see `apps/companion/db_test.py` and command.md), because the
 * SD-backed operations require a live card and a properly initialized storage
 * layer that the unit-test harness does not provide.
 */

#include "unity.h"
#include "db.h"
#include <string.h>

/* ========================================================================
 * DATABASE NAME VALIDATION (pure logic, no SD)
 * ======================================================================== */

void test_db_name_valid_basic(void)
{
    TEST_ASSERT_TRUE(db_name_valid("contacts"));
    TEST_ASSERT_TRUE(db_name_valid("phonebook2"));
    TEST_ASSERT_TRUE(db_name_valid("my_notes"));
    TEST_ASSERT_TRUE(db_name_valid("a"));
}

void test_db_name_valid_rejects_bad(void)
{
    TEST_ASSERT_FALSE(db_name_valid(NULL));
    TEST_ASSERT_FALSE(db_name_valid(""));
    TEST_ASSERT_FALSE(db_name_valid("a/b"));        /* path separator */
    TEST_ASSERT_FALSE(db_name_valid("a\\b"));       /* path separator */
    TEST_ASSERT_FALSE(db_name_valid("a:b"));        /* colon */
    TEST_ASSERT_FALSE(db_name_valid("a.b"));        /* dot */
    TEST_ASSERT_FALSE(db_name_valid(".."));         /* dot */
    TEST_ASSERT_FALSE(db_name_valid("."));
}

void test_db_name_valid_length(void)
{
    char long_name[P4_CONFIG_DB_NAME_BYTES + 4];

    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    TEST_ASSERT_FALSE(db_name_valid(long_name));

    /* At the limit: exactly NAME_BYTES-1 chars is valid. */
    {
        char at_limit[P4_CONFIG_DB_NAME_BYTES];
        memset(at_limit, 'y', sizeof(at_limit) - 1);
        at_limit[sizeof(at_limit) - 1] = '\0';
        TEST_ASSERT_TRUE(db_name_valid(at_limit));
    }
}
