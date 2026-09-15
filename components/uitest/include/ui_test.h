/**
 * @file ui_test.h
 * @brief Synthetic touch injection for firmware UI automation and tests.
 *
 * Owns a second LVGL pointer indev whose read callback reports a scripted
 * point/press state. Because it is a real indev, LVGL drives it through the
 * normal press / move / release / long-press / scroll pipeline, so a `ui tap`
 * exercises exactly the same code as a finger on the glass (including
 * hit-testing, buttonmatrix key selection, and event delivery).
 *
 * The engine runs its script on the LVGL task (a small lv_timer); callers
 * block until the script completes, so the `ui` command verbs are
 * deterministic and batch-friendly.
 */

#ifndef P4MINISHELL_UI_TEST_H
#define P4MINISHELL_UI_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the synthetic indev and its script timer (idempotent).
 *  Must run on the LVGL task or under lvgl_port_lock(). */
esp_err_t ui_test_init(void);

/** True once ui_test_init() succeeded. */
bool ui_test_is_ready(void);

/* ========================================================================
 * BLOCKING INPUT PRIMITIVES
 * ========================================================================
 * All coordinates are display pixels with a top-left origin. The trailing
 * _run forms submit a script and block until it has played out and the
 * release has been processed.
 */

/** Press and hold at (x, y) for @p hold_ms, then release. */
esp_err_t ui_test_tap(int x, int y, uint32_t hold_ms);

/** Press at (x, y), hold past the LVGL long-press time, then release. */
esp_err_t ui_test_long_press(int x, int y, uint32_t hold_ms);

/** Drag from (x1,y1) to (x2,y2) over @p duration_ms in @p steps, then release. */
esp_err_t ui_test_swipe(int x1, int y1, int x2, int y2,
                        uint32_t duration_ms, int steps);

/** Low-level hold: press at (x,y) and keep it pressed until ui_test_release(). */
esp_err_t ui_test_press(int x, int y);

/** Low-level hold: move the pressed point to (x,y) (must be pressed). */
esp_err_t ui_test_move(int x, int y);

/** Low-level hold: release the current press. */
esp_err_t ui_test_release(void);

/** Abort any running script and force the synthetic pointer released. */
void ui_test_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_UI_TEST_H */
