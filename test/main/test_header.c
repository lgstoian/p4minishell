/**
 * @file test_header.c
 * @brief Unit tests for the pure header layout policy
 * (components/header/header_layout.c).
 *
 * Widgets, measurement, and rendering are hardware-verified with
 * tools/header_test.py; this covers the width/priority decisions.
 */

#include "unity.h"
#include "header_layout.h"

/* Convenience: all levels equal except the ones under test. */
static void fill(int out[HEADER_LEVEL_COUNT], int full, int shrt, int mn)
{
    out[HEADER_LEVEL_FULL] = full;
    out[HEADER_LEVEL_SHORT] = shrt;
    out[HEADER_LEVEL_MIN] = mn;
}

void test_header_layout_all_full(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 260, 180, 110);
    header_layout_compute(&l, 1024, 4, status, sys, 100);

    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_FULL, l.status_level);
    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_FULL, l.sys_level);
    TEST_ASSERT_TRUE(l.show_status);
    TEST_ASSERT_TRUE(l.show_sys);
    TEST_ASSERT_TRUE(l.show_center);
    TEST_ASSERT_TRUE(l.show_sep);
    TEST_ASSERT_TRUE(l.show_cpu_graph);
    TEST_ASSERT_TRUE(l.show_battery_bar);
    TEST_ASSERT_FALSE(l.smaller_font);
    TEST_ASSERT_EQUAL_INT(300, l.left_w);
    TEST_ASSERT_EQUAL_INT(260, l.right_w);
    TEST_ASSERT_EQUAL_INT(1024 - 300 - 260 - 8, l.center_w);
}

void test_header_layout_compacts_sides(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 300, 180, 110);
    /* 600 avail: full/full needs 600+8+100 > 600; full/short needs
     * 300+180+8+100 = 588 -> fits. */
    header_layout_compute(&l, 600, 4, status, sys, 100);

    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_FULL, l.status_level);
    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_SHORT, l.sys_level);
    TEST_ASSERT_TRUE(l.show_center);
    TEST_ASSERT_FALSE(l.show_sep);
    TEST_ASSERT_FALSE(l.show_cpu_graph);
    TEST_ASSERT_TRUE(l.show_battery_bar);
    TEST_ASSERT_EQUAL_INT(600 - 300 - 180 - 8, l.center_w);
}

void test_header_layout_center_yields_first(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 300, 180, 110);
    /* 340 avail, center_min 150: no combo leaves room for the center, but
     * short/min sides (200+110+8 = 318) fit -> center hidden, sides kept,
     * no smaller font needed. Notifications yield first. */
    header_layout_compute(&l, 340, 4, status, sys, 150);

    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_SHORT, l.status_level);
    TEST_ASSERT_EQUAL_INT(HEADER_LEVEL_MIN, l.sys_level);
    TEST_ASSERT_FALSE(l.show_center);
    TEST_ASSERT_EQUAL_INT(0, l.center_w);
    TEST_ASSERT_FALSE(l.show_sep);
    TEST_ASSERT_FALSE(l.show_cpu_graph);
    TEST_ASSERT_FALSE(l.show_battery_bar);
    TEST_ASSERT_FALSE(l.smaller_font);
}

void test_header_layout_requests_smaller_font(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 300, 180, 110);
    /* 200 avail: even min/min (238) doesn't fit -> smaller font requested;
     * with sides 120+110+4 = 234 > 200, the sys panel is dropped. */
    header_layout_compute(&l, 200, 4, status, sys, 100);

    TEST_ASSERT_TRUE(l.smaller_font);
    TEST_ASSERT_FALSE(l.show_center);
    TEST_ASSERT_FALSE(l.show_sys);
    TEST_ASSERT_TRUE(l.show_status);
    TEST_ASSERT_EQUAL_INT(120, l.left_w);
    TEST_ASSERT_EQUAL_INT(0, l.right_w);
}

void test_header_layout_degenerate(void)
{
    int status[HEADER_LEVEL_COUNT];
    int sys[HEADER_LEVEL_COUNT];
    header_layout_t l;

    fill(status, 300, 200, 120);
    fill(sys, 300, 180, 110);
    /* Zero width: everything hides, no overlap, no crash. */
    header_layout_compute(&l, 0, 4, status, sys, 100);
    TEST_ASSERT_FALSE(l.show_status);
    TEST_ASSERT_FALSE(l.show_sys);
    TEST_ASSERT_FALSE(l.show_center);
    TEST_ASSERT_EQUAL_INT(0, l.left_w);
    TEST_ASSERT_EQUAL_INT(0, l.right_w);

    /* NULL-safe. */
    header_layout_compute(NULL, 100, 4, status, sys, 10);
}
