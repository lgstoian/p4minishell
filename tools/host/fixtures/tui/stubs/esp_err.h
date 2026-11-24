/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file esp_err.h
 * @brief Host-only include stub for tools/host_tui_gate.py.
 *
 * ansi.h exposes esp_err_t in a few prototypes but the TUI frame gate calls
 * only the pure palette/ramp functions. This stub supplies the ESP-IDF type so
 * the SAME production headers compile under a plain gcc. It is never built
 * into firmware and defines no colours, parsers or renderers.
 */
#ifndef HOST_TUI_STUB_ESP_ERR_H
#define HOST_TUI_STUB_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK   0
#define ESP_FAIL -1

#endif /* HOST_TUI_STUB_ESP_ERR_H */
