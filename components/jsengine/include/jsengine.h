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

#endif
