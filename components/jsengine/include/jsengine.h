/**
 * @file jsengine.h
 * @module jsengine
 * @layer leaf
 * @domain app
 * @provides QuickJS engine + shell/TUI/FS/batch bindings
 * @status stable
 * @hard-rule ai-context.md#module-layering-rules
 * @brief QuickJS engine + shell/TUI/FS/batch bindings.
 */
#ifndef JSENGINE_H
#define JSENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct JSContext JSContext;
struct dom_tree;

bool jsengine_init(void);
void jsengine_deinit(void);

bool jsengine_eval(const char *code, char *result, size_t result_size);
bool jsengine_run_file(const char *path, char *result, size_t result_size);
bool jsengine_call(const char *path, const char *func,
                   const char *args_json, char *result, size_t result_size);

void jsengine_set_timeout_ms(uint32_t ms);
void jsengine_set_heap_limit(size_t bytes);

/** Bind the DOM the browser is currently rendering (or NULL to clear it).
 *  The document.* bindings read this pointer; the browser sets it around
 *  `<script>` execution so no second DOM registry exists. */
void jsengine_set_dom_tree(struct dom_tree *tree);

void js_bindings_register(JSContext *ctx);
void js_dom_bindings_register(JSContext *ctx, struct dom_tree *tree);
void js_dom_clear_page_state(void);   /* drop document listeners on page load */

/* ---- Async timer queue on the ONE QuickJS runtime ----------------------- */
/** Register a callback (a JS function value) with a delay in milliseconds.
 *  @p repeat makes it an interval. Returns an id > 0, or 0 on failure. */
uint32_t jsengine_timer_add(JSContext *ctx, const void *callback,
                            uint32_t delay_ms, bool repeat);
bool jsengine_timer_cancel(uint32_t id);
/** Run every callback due at or before @p now_ms. Uses a bounded queue, never
 *  spawns a task or a second runtime; a callback error is swallowed. */
int jsengine_timers_run_due(int64_t now_ms);
int jsengine_timers_pending(void);
void jsengine_timers_clear(void);
/** Override the monotonic clock (host deterministic gate). NULL restores the
 *  ESP timer. */
void jsengine_set_clock_fn(int64_t (*now_ms_fn)(void));
int64_t jsengine_now_ms(void);

/** Evaluate a bounded event-handler source with an `event` object in scope.
 *  Used by the DOM dispatch hook; keeps handlers on the same runtime. */
bool jsengine_eval_event(const char *source, const char *event_type,
                         const char *target_id, char *result, size_t result_size);

#endif
