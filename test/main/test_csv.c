/**
 * @file test_csv.c
 * @brief Unit tests for the pure CSV helpers (storage_csv.c, csv_commands.c).
 *
 * Covers csv_split_line() (commas, quoted fields, "" escapes, empty and
 * trailing fields, over-max and scratch-exhaustion counting) and
 * csv_substitute_refs() (R1C1 substitution, case-insensitivity,
 * out-of-range and non-numeric reads as 0, range aggregates). No SD or
 * hardware needed.
 */

#include "unity.h"
#include "storage_commands.h"
#include "command.h"

#include <string.h>

#define CSV_SCRATCH 1024
#define CSV_MAXF 8

static int split(const char *line, char *scratch, csv_field_t *fields, int maxf)
{
    return csv_split_line(line, scratch, CSV_SCRATCH, fields, maxf);
}

void test_csv_split_simple(void)
{
    char scratch[CSV_SCRATCH];
    csv_field_t fields[CSV_MAXF];
    int n = split("a,b,c", scratch, fields, CSV_MAXF);

    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_INT(1, fields[0].len);
    TEST_ASSERT_EQUAL_INT(1, fields[1].len);
    TEST_ASSERT_EQUAL_INT(1, fields[2].len);
    TEST_ASSERT_EQUAL_STRING("a", scratch + fields[0].off);
    TEST_ASSERT_EQUAL_STRING("b", scratch + fields[1].off);
    TEST_ASSERT_EQUAL_STRING("c", scratch + fields[2].off);
}

void test_csv_split_quoted(void)
{
    char scratch[CSV_SCRATCH];
    csv_field_t fields[CSV_MAXF];
    int n = split("a,\"b,c\",\"d\"\"e\",", scratch, fields, CSV_MAXF);

    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_STRING("a", scratch + fields[0].off);
    TEST_ASSERT_EQUAL_STRING("b,c", scratch + fields[1].off);
    TEST_ASSERT_EQUAL_STRING("d\"e", scratch + fields[2].off);
    TEST_ASSERT_EQUAL_INT(0, fields[3].len);
}

void test_csv_split_empty(void)
{
    char scratch[CSV_SCRATCH];
    csv_field_t fields[CSV_MAXF];

    TEST_ASSERT_EQUAL_INT(0, split("", scratch, fields, CSV_MAXF));
    TEST_ASSERT_EQUAL_INT(0, split("\r\n", scratch, fields, CSV_MAXF));
    TEST_ASSERT_EQUAL_INT(0, split(NULL, scratch, fields, CSV_MAXF));
    TEST_ASSERT_EQUAL_INT(2, split(",", scratch, fields, CSV_MAXF));
    TEST_ASSERT_EQUAL_INT(3, split(",,", scratch, fields, CSV_MAXF));
}

void test_csv_split_over_max(void)
{
    char scratch[CSV_SCRATCH];
    csv_field_t fields[2];
    int n = split("a,b,c,d", scratch, fields, 2);

    /* True width is reported; only the first two are recorded. */
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_STRING("a", scratch + fields[0].off);
    TEST_ASSERT_EQUAL_STRING("b", scratch + fields[1].off);
}

void test_csv_split_exhaustion(void)
{
    char tiny[8];
    csv_field_t fields[CSV_MAXF];
    int n = csv_split_line("abcdef,gh", tiny, sizeof(tiny), fields, CSV_MAXF);

    /* "abcdef" (6+1 bytes) fits in 8; "gh" does not fit the remainder but
     * is still counted, so the true width survives exhaustion. */
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("abcdef", tiny + fields[0].off);
}

void test_csv_substitute_refs(void)
{
    char *cells[4] = {"10", "20", "hello", "=R1C1+R1C2"};
    char out[128];

    csv_substitute_refs("R1C1+R1C2", cells, 2, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("10+20", out);

    csv_substitute_refs("r2c1*2", cells, 2, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0*2", out);

    csv_substitute_refs("R9C9", cells, 2, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0", out);

    csv_substitute_refs("plain", cells, 2, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("plain", out);

    csv_substitute_refs("R1C2x", cells, 2, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("20x", out);
}

void test_csv_substitute_ranges(void)
{
    /* 10 20 30 / 4 abc "" / 7 8 9 ; blanks read as 0, COUNT skips them. */
    char *cells[9] = {"10", "20", "30", "4", "abc", "", "7", "8", "9"};
    char out[128];

    csv_substitute_refs("SUM(R1C1:R1C3)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("60", out);
    csv_substitute_refs("SUM(R1C1:R3C3)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("88", out);
    csv_substitute_refs("AVG(R1C1:R1C2)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("15", out);
    csv_substitute_refs("MIN(R1C1:R2C2)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0", out);
    csv_substitute_refs("MAX(R1C1:R2C2)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("20", out);
    csv_substitute_refs("COUNT(R1C1:R3C3)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("8", out);

    /* Corners normalize, case and spaces tolerated, grid clips. */
    csv_substitute_refs("SUM(R1C3:R1C1)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("60", out);
    csv_substitute_refs("sum(r1c1:r1c3)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("60", out);
    csv_substitute_refs("SUM( R1C1 : R1C3 )", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("60", out);
    csv_substitute_refs("SUM(R1C1:R9C9)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("88", out);

    /* Empty rects fold to 0; mixed with scalar refs. */
    csv_substitute_refs("SUM(R9C9:R9C9)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0", out);
    csv_substitute_refs("COUNT(R9C9:R9C9)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0", out);
    csv_substitute_refs("1+SUM(R1C1:R1C2)*2", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1+30*2", out);

    /* Near-misses pass through for calc to reject (or env to resolve). */
    csv_substitute_refs("SUMMARY", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("SUMMARY", out);
    csv_substitute_refs("SUM(1)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("SUM(1)", out);
    csv_substitute_refs("SUM(R1C1)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("SUM(R1C1)", out);
    csv_substitute_refs("TOTAL(R1C1:R1C2)", cells, 3, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("TOTAL(R1C1:R1C2)", out);
}

void test_csv_format_field(void)
{
    char out[64];

    csv_format_field("plain", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("plain", out);
    csv_format_field("a,b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("\"a,b\"", out);
    csv_format_field("say \"hi\"", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("\"say \"\"hi\"\"\"", out);
    csv_format_field("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(strlen("\"a,b\"") + 1, csv_format_field("a,b", NULL, 0));
}
