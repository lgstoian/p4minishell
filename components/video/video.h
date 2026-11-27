/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file video.h
 * @module video
 * @layer leaf
 * @domain media
 * @provides timed BMP/PNG/JPEG frame slideshow over the one decoder + one cell sampler
 * @status experimental
 * @test test/main/test_video.c, tools/host_video_gate.py
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Pragmatic frame-sequence playback, not a codec.
 *
 * A "video" is an ordered set of ordinary image files (BMP/PNG/JPEG) on the
 * SD card -- either every supported file in a directory, or the paths listed
 * in a plain-text manifest (one path per line, '#'/';' comments). Playback is
 * a timed slideshow driven by the command worker's existing frame loop: there
 * is no decoder of our own, no second playlist, no scheduler and no extra
 * task. Every frame goes through imagefile_decode_fit() (the ONE decoder) and
 * is presented through a callback registered by the command layer, which
 * either blits to the existing gfx canvas or samples cells through the ONE
 * tui_sample_image_cells() sampler.
 *
 * No hardware timing or frame-rate claim is made: the interval is a polite
 * app_delay_ms() between frames, subject to the real decode/present cost.
 */
#ifndef P4MINISHELL_VIDEO_H
#define P4MINISHELL_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gfx.h"          /* gfx_surface_t: the one decoded frame carrier */

#ifdef __cplusplus
extern "C" {
#endif

/** Longest accepted frame path / manifest line. */
#define VIDEO_PATH_MAX 256
/** Hard cap on frames in one plan (the pure-core default; config may raise it). */
#ifndef P4_CONFIG_VIDEO_MAX_FRAMES
#define P4_CONFIG_VIDEO_MAX_FRAMES 256
#endif
/** Hard cap on frames actually presented in one run (bounds a loop forever). */
#ifndef P4_CONFIG_VIDEO_FRAME_BUDGET
#define P4_CONFIG_VIDEO_FRAME_BUDGET 4096
#endif
/** Default and maximum frame rate accepted by /fps:N. */
#define VIDEO_DEFAULT_FPS 8
#define VIDEO_MAX_FPS 30

/** Image kinds the ONE imagefile decoder understands. */
typedef enum {
    VIDEO_FRAME_UNKNOWN = 0,
    VIDEO_FRAME_BMP,
    VIDEO_FRAME_PNG,
    VIDEO_FRAME_JPEG,
} video_frame_kind_t;

/** Parsed `video play` options (everything after the source path is a flag). */
typedef struct {
    int fps;             /**< 1..VIDEO_MAX_FPS. */
    bool loop;           /**< Restart at frame 0 when the plan ends. */
    bool tui;            /**< Sample into TUI cells instead of the gfx canvas. */
    bool bare;           /**< /b: one machine-readable status/report line. */
    int frame_budget;    /**< Maximum frames presented in one run. */
} video_options_t;

/** The single playback cursor (one state store, no second scheduler). */
typedef struct {
    bool playing;          /**< True while a play run owns the cursor. */
    bool stop_requested;   /**< Set by video_stop(), observed by the frame loop. */
    int index;             /**< Current frame index (0-based), -1 when idle. */
    int count;             /**< Frames in the current plan. */
    int fps;               /**< Frame rate used for this run. */
    int frames_played;     /**< Total frames presented (counts looped passes). */
    int loops_done;        /**< Completed wraparounds. */
} video_cursor_t;

/** Default options (fps=VIDEO_DEFAULT_FPS, no loop, gfx surface, budget cap). */
void video_options_default(video_options_t *out);

/**
 * Parse a /fps:N value. Pure: accepts only 1..VIDEO_MAX_FPS decimal digits,
 * fully consumed, no sign or whitespace.
 */
bool video_parse_fps(const char *text, int *fps_out);

/**
 * Parse the flag tokens that follow `video play`. Pure. @p source_out receives
 * the single positional directory/manifest path (NUL-terminated). Any unknown
 * slash flag or a second positional token fails the parse.
 */
bool video_options_parse(const char *const *args, int argc,
                         video_options_t *options_out,
                         char *source_out, size_t source_size);

/** Classify a frame by its extension (pure, case-insensitive). */
video_frame_kind_t video_frame_kind(const char *path);

/** True when @p path names a frame the ONE decoder can read. Pure. */
bool video_frame_supported(const char *path);

/**
 * Append @p path to a caller-owned frame array, ignoring exact duplicates and
 * refusing to overflow @p capacity. Pure. @return true only when the path was
 * newly stored; false on a bad path, a duplicate or a full table (the caller
 * keeps ownership of the string in every case).
 */
bool video_frame_add(char **frames, int *count, int capacity, const char *path);

/** Sort frame paths lexicographically, case-insensitive with a stable tie-break. Pure. */
void video_frames_sort(char **frames, int count);

/** Milliseconds per frame for @p fps (clamped to 1..VIDEO_MAX_FPS). Pure. */
uint32_t video_frame_interval_ms(int fps);

/** True while another frame may be presented within the frame budget. Pure. */
bool video_budget_ok(const video_cursor_t *cursor);

/** Initialize a cursor for a plan. Pure. */
void video_cursor_begin(video_cursor_t *cursor, int count, int fps);

/**
 * Advance one frame. Pure. On the last frame: with @p loop it wraps to index 0
 * and increments loops_done; without it the cursor stops. @return false when
 * the run is over (end-of-plan or budget exhausted).
 */
bool video_cursor_next(video_cursor_t *cursor, bool loop);

/** Request a cooperative stop; the running frame loop observes it. Pure. */
void video_cursor_stop(video_cursor_t *cursor);

/**
 * Format the one-line /b status ("state=... index=... frames=... fps=...
 * played=... loops=..."). Pure; always NUL-terminated when size > 0.
 */
void video_cursor_format(const video_cursor_t *cursor, char *out, size_t size);

/**
 * Filter one manifest line into a frame path. Pure. Blank lines and lines
 * beginning with '#' or ';' are skipped; surrounding whitespace is trimmed.
 * @return true when @p path_out holds a supported frame path.
 */
bool video_manifest_line(const char *line, char *path_out, size_t path_size);

/* ------------------------------------------------------------------------
 * Firmware playback (no new task: runs on the caller's command-worker frame)
 * ------------------------------------------------------------------------ */

/**
 * Present one decoded frame. Registered once by the command layer; the video
 * leaf never includes command.h (that would invert the layer dependency).
 * @p tui selects the ONE cell sampler path, otherwise the existing gfx canvas.
 */
typedef esp_err_t (*video_present_cb_t)(const gfx_surface_t *frame, bool tui,
                                        void *ctx);

/** Install (or clear with NULL) the one presentation callback. */
void video_set_present_callback(video_present_cb_t callback, void *ctx);

/**
 * Build the frame plan from @p source (directory or manifest file) and play it
 * synchronously. Frames are decoded one at a time through imagefile_decode_fit
 * and freed before the next; at most P4_CONFIG_VIDEO_MAX_FRAMES paths are kept.
 * @return ESP_OK on a completed run, ESP_ERR_NOT_FOUND for an empty/unreadable
 *         source, ESP_ERR_INVALID_ARG for bad options, ESP_ERR_INVALID_STATE
 *         when a run is already active.
 */
esp_err_t video_play(const char *source, const video_options_t *options);

/** Cooperative stop for the active run. Safe when idle. */
void video_stop(void);

/** Copy the current cursor state. Safe when idle. */
void video_status(video_cursor_t *out);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_VIDEO_H */
