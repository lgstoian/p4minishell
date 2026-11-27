/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_video.c
 * @module test
 * @layer test
 * @domain media
 * @provides pure video/frame-plan, EPUB TOC/spine and archive machine-row cases
 * @status experimental
 * @test tools/host_video_gate.py
 * @hard-rule pure logic only - no SD, decoder, display or task
 * @brief Hardware-free cases for the new purely-computable cores.
 *
 * The decoding and display contracts are checked by tools/host_video_gate.py,
 * which compiles the real cores. This suite only asserts deterministic rules:
 * option parsing, ordering, frame budget, TOC/spine linking and machine rows.
 */
#include "unity.h"

#include "archive.h"
#include "epub.h"
#include "epub_nav_core.h"
#include "video.h"

#include <stdio.h>
#include <string.h>

void test_video_fps_parse_and_interval(void)
{
    int fps = 0;

    TEST_ASSERT_TRUE(video_parse_fps("10", &fps));
    TEST_ASSERT_EQUAL_INT(10, fps);
    TEST_ASSERT_TRUE(video_parse_fps("30", &fps));
    TEST_ASSERT_EQUAL_INT(30, fps);
    TEST_ASSERT_FALSE(video_parse_fps("0", &fps));
    TEST_ASSERT_FALSE(video_parse_fps("31", &fps));
    TEST_ASSERT_FALSE(video_parse_fps("12x", &fps));
    TEST_ASSERT_FALSE(video_parse_fps("-2", &fps));
    TEST_ASSERT_FALSE(video_parse_fps("", &fps));
    TEST_ASSERT_EQUAL_UINT32(100, video_frame_interval_ms(10));
    TEST_ASSERT_EQUAL_UINT32(500, video_frame_interval_ms(2));
    TEST_ASSERT_EQUAL_UINT32(1000, video_frame_interval_ms(0));
    TEST_ASSERT_EQUAL_UINT32(1000 / VIDEO_MAX_FPS, video_frame_interval_ms(99));
}

void test_video_options_parse_flags(void)
{
    video_options_t options;
    char source[VIDEO_PATH_MAX];
    const char *args[] = {"/fps:12", "/loop", "/tui", "/b", "sd:/FRAMES"};

    TEST_ASSERT_TRUE(video_options_parse(args, 5, &options, source,
                                         sizeof(source)));
    TEST_ASSERT_EQUAL_INT(12, options.fps);
    TEST_ASSERT_TRUE(options.loop);
    TEST_ASSERT_TRUE(options.tui);
    TEST_ASSERT_TRUE(options.bare);
    TEST_ASSERT_EQUAL_STRING("sd:/FRAMES", source);
    TEST_ASSERT_TRUE(options.frame_budget > 0);

    TEST_ASSERT_FALSE(video_options_parse(args, 1, &options, source,
                                          sizeof(source)));
    TEST_ASSERT_FALSE(video_parse_fps("1x", &options.fps));
    {
        const char *bad[] = {"/wat", "sd:/FRAMES"};
        TEST_ASSERT_FALSE(video_options_parse(bad, 2, &options, source,
                                              sizeof(source)));
    }
}

void test_video_frame_order_and_dedup(void)
{
    char *frames[8];
    int count = 0;

    TEST_ASSERT_EQUAL(VIDEO_FRAME_BMP, video_frame_kind("FRAME.BMP"));
    TEST_ASSERT_EQUAL(VIDEO_FRAME_PNG, video_frame_kind("a.png"));
    TEST_ASSERT_EQUAL(VIDEO_FRAME_JPEG, video_frame_kind("a.JPEG"));
    TEST_ASSERT_EQUAL(VIDEO_FRAME_JPEG, video_frame_kind("a.jpg"));
    TEST_ASSERT_EQUAL(VIDEO_FRAME_UNKNOWN, video_frame_kind("a.gif"));
    TEST_ASSERT_TRUE(video_frame_supported("frame_002.bmp"));
    TEST_ASSERT_FALSE(video_frame_supported("notes.txt"));

    TEST_ASSERT_TRUE(video_frame_add(frames, &count, 8, "f_010.bmp"));
    TEST_ASSERT_TRUE(video_frame_add(frames, &count, 8, "f_002.bmp"));
    TEST_ASSERT_TRUE(video_frame_add(frames, &count, 8, "F_001.png"));
    TEST_ASSERT_FALSE(video_frame_add(frames, &count, 8, "f_002.bmp"));
    TEST_ASSERT_EQUAL_INT(3, count);

    video_frames_sort(frames, count);
    TEST_ASSERT_EQUAL_STRING("F_001.png", frames[0]);
    TEST_ASSERT_EQUAL_STRING("f_002.bmp", frames[1]);
    TEST_ASSERT_EQUAL_STRING("f_010.bmp", frames[2]);
}

void test_video_manifest_and_cursor(void)
{
    video_cursor_t cursor;
    char path[VIDEO_PATH_MAX];
    char line[160];

    TEST_ASSERT_FALSE(video_manifest_line("  # comment\n", path, sizeof(path)));
    TEST_ASSERT_FALSE(video_manifest_line("; other\n", path, sizeof(path)));
    TEST_ASSERT_FALSE(video_manifest_line("\n", path, sizeof(path)));
    TEST_ASSERT_TRUE(video_manifest_line("  sd:/FRAMES/f0.bmp  \n",
                                         path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("sd:/FRAMES/f0.bmp", path);
    TEST_ASSERT_FALSE(video_manifest_line("notes.txt\n", path, sizeof(path)));

    video_cursor_begin(&cursor, 2, 10);
    TEST_ASSERT_TRUE(cursor.playing);
    TEST_ASSERT_EQUAL_INT(0, cursor.index);
    TEST_ASSERT_TRUE(video_budget_ok(&cursor));
    TEST_ASSERT_TRUE(video_cursor_next(&cursor, false));
    TEST_ASSERT_EQUAL_INT(1, cursor.index);
    TEST_ASSERT_FALSE(video_cursor_next(&cursor, false));
    TEST_ASSERT_FALSE(cursor.playing);

    video_cursor_begin(&cursor, 2, 10);
    TEST_ASSERT_TRUE(video_cursor_next(&cursor, true));   /* 0 -> 1 */
    TEST_ASSERT_TRUE(video_cursor_next(&cursor, true));   /* 1 -> 0, loop */
    TEST_ASSERT_EQUAL_INT(1, cursor.loops_done);
    TEST_ASSERT_EQUAL_INT(2, cursor.frames_played);
    video_cursor_stop(&cursor);
    TEST_ASSERT_FALSE(video_budget_ok(&cursor));
    video_cursor_format(&cursor, line, sizeof(line));
    TEST_ASSERT_NOT_NULL(strstr(line, "state=stopping"));
    TEST_ASSERT_NOT_NULL(strstr(line, "loops=1"));
}

void test_video_epub_toc_spine_edges(void)
{
    epub_chapter_t spine[3];
    epub_toc_entry_t toc[3];
    int i;

    memset(spine, 0, sizeof(spine));
    memset(toc, 0, sizeof(toc));
    for (i = 0; i < 3; i++) {
        spine[i].linear = true;
        snprintf(spine[i].href, sizeof(spine[i].href), "Text/c%d.xhtml", i + 1);
    }
    spine[2].linear = false;   /* an explicit non-linear itemref */
    TEST_ASSERT_EQUAL_INT(2, epub_spine_linear_count(spine, 3));
    TEST_ASSERT_EQUAL_INT(-1, epub_nav_step(0, 0, 1));
    TEST_ASSERT_EQUAL_INT(-1, epub_nav_step(0, 3, -1));
    TEST_ASSERT_EQUAL_INT(1, epub_nav_step(0, 3, 1));
    TEST_ASSERT_EQUAL_INT(-1, epub_nav_step(2, 3, 1));
    TEST_ASSERT_TRUE(epub_toc_depth_sane(0));
    TEST_ASSERT_FALSE(epub_toc_depth_sane(-1));
    TEST_ASSERT_TRUE(epub_href_match("./Text/c1.xhtml", "OEBPS/Text/c1.xhtml"));
    TEST_ASSERT_TRUE(epub_href_match("Text/c1.xhtml#top", "Text/c1.xhtml"));

    snprintf(toc[0].href, sizeof(toc[0].href), "Text/c1.xhtml");
    snprintf(toc[0].label, sizeof(toc[0].label), "One");
    toc[0].chapter_index = -1;
    snprintf(toc[1].href, sizeof(toc[1].href), "Text/c999.xhtml");
    toc[1].chapter_index = -1;
    snprintf(toc[2].href, sizeof(toc[2].href), "Text/c2.xhtml");
    toc[2].chapter_index = -1;

    TEST_ASSERT_EQUAL_INT(2, epub_toc_link_spine(toc, 3, spine, 3));
    TEST_ASSERT_EQUAL_INT(0, toc[0].chapter_index);
    TEST_ASSERT_EQUAL_INT(-1, toc[1].chapter_index);
    TEST_ASSERT_EQUAL_STRING("One", spine[0].title);
    TEST_ASSERT_EQUAL_INT(0, epub_toc_target(toc, 3, spine, 3, 0));

    /* A stale chapter_index must not block navigation when the href still
     * resolves -- this is the stale-TOC case the host gate canaries check. */
    toc[2].chapter_index = 99;
    TEST_ASSERT_EQUAL_INT(1, epub_toc_target(toc, 3, spine, 3, 2));
    TEST_ASSERT_EQUAL_INT(-1, epub_toc_target(toc, 3, spine, 3, 1));
}

void test_video_archive_machine_row_bounds(void)
{
    archive_entry_t entry;
    char row[384];

    memset(&entry, 0, sizeof(entry));
    snprintf(entry.path, sizeof(entry.path), "FRAMES/frame|one.bmp");
    entry.size = 123;
    entry.mtime = 1700000000;
    entry.crc = 0x89ABCDEFu;
    entry.crc_known = true;
    entry.format = ARCHIVE_FORMAT_USTAR;
    TEST_ASSERT_TRUE(archive_entry_bounds_ok(&entry, 1024));
    TEST_ASSERT_FALSE(archive_entry_bounds_ok(&entry, 100));

    TEST_ASSERT_TRUE(archive_format_machine_line(&entry, row, sizeof(row)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, "f|123|1700000000|crc|2309737967|"));
    TEST_ASSERT_NOT_NULL(strstr(row, "FRAMES/frame/one.bmp"));
    TEST_ASSERT_NULL(strchr(row, '\n'));

    snprintf(entry.path, sizeof(entry.path), "../escape.bmp");
    TEST_ASSERT_FALSE(archive_entry_bounds_ok(&entry, 1024));
}
