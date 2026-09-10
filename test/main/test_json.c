/**
 * @file test_json.c
 * @brief Unit tests for the JSON validate/pretty core (json_commands.c).
 */

#include "unity.h"
#include "command.h"
#include <string.h>

void test_json_validate_ok(void)
{
    char err[128];

    TEST_ASSERT_TRUE(json_validate_text("{}", 2, err, sizeof(err)));
    TEST_ASSERT_TRUE(json_validate_text("[]", 2, err, sizeof(err)));
    TEST_ASSERT_TRUE(
        json_validate_text("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\"}",
                           strlen("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\"}"),
                           err, sizeof(err)));
    TEST_ASSERT_TRUE(json_validate_text("  { \"k\" : -1.5e+3 }  ",
                                        strlen("  { \"k\" : -1.5e+3 }  "),
                                        err, sizeof(err)));
    TEST_ASSERT_FALSE(json_validate_text(NULL, 0, err, sizeof(err)));
}

void test_json_validate_bad(void)
{
    char err[128];

    TEST_ASSERT_FALSE(json_validate_text("{", 1, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "line 1") != NULL);
    TEST_ASSERT_FALSE(json_validate_text("{\"a\":}", 6, err, sizeof(err)));
    TEST_ASSERT_FALSE(json_validate_text("[1,]", 4, err, sizeof(err)));
    TEST_ASSERT_FALSE(json_validate_text("{'a':1}", 7, err, sizeof(err)));
    TEST_ASSERT_FALSE(json_validate_text("{\"a\":tru}", 8, err, sizeof(err)));
    TEST_ASSERT_FALSE(json_validate_text("{\"a\":01}", 8, err, sizeof(err)));
    TEST_ASSERT_FALSE(json_validate_text("{\"a\":\"\\q\"}", 9, err, sizeof(err)));
    TEST_ASSERT_FALSE(json_validate_text("{} {}", 4, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "trailing") != NULL);
}

void test_json_pretty(void)
{
    char out[256];
    char err[128];
    size_t n;

    n = json_pretty_text("{\"a\":1,\"b\":[1,2]}", 17, out, sizeof(out),
                         err, sizeof(err));
    TEST_ASSERT_GREATER_THAN(0, (int)n);
    TEST_ASSERT_EQUAL_STRING("{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ]\n}\n",
                             out);
    /* Empty containers stay collapsed. */
    n = json_pretty_text("{\"e\":{},\"l\":[]}", 15, out, sizeof(out),
                         err, sizeof(err));
    TEST_ASSERT_GREATER_THAN(0, (int)n);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"e\": {}"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"l\": []"));
    /* Invalid input yields 0 + error. */
    TEST_ASSERT_EQUAL_size_t(0, json_pretty_text("{", 1, out, sizeof(out),
                                                 err, sizeof(err)));
    TEST_ASSERT_EQUAL_size_t(0, json_pretty_text(NULL, 0, out, sizeof(out),
                                                 err, sizeof(err)));
    TEST_ASSERT_EQUAL_size_t(0, json_pretty_text("{}", 2, NULL, 0,
                                                 err, sizeof(err)));
}
