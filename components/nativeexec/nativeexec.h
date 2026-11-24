/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file nativeexec.h
 * @module nativeexec
 * @layer leaf
 * @domain app
 * @provides the one fixed-ABI loader for opted-in signed native payloads
 * @status experimental
 * @test test/main/test_native_exec.c
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Single loader for one verified `NATIVE/<APP>.P4X` envelope (ABI.md §4.1).
 *
 * There is exactly ONE loader path. This component does not parse ELF, does not
 * relocate, does not create an MMU address space and does not offer a second
 * dispatcher. The payload is a flat, position-independent blob with a fixed
 * export table at offset zero; the envelope's entry ordinal indexes that table
 * and the selected address is a plain offset into the mapped blob. Nothing is
 * executed until `native_pay_check()` has returned NATIVE_PAY_OK and the
 * required API bits have been checked against the installed API table.
 *
 * Layering: the crypto hooks, the single `p4sign` key, the executable-memory
 * map hook and the concrete API provider are installed from the command layer
 * (`components/command/native_commands.c`). This module only owns the loader
 * logic, so the host lane can test its pure half without ESP-IDF.
 *
 * Payload ABI (little-endian, fixed, no ELF):
 *
 * | Offset | Bytes | Field |
 * |--------|-------|-------|
 * | 0      | 4     | magic `P4EX` |
 * | 4      | 2     | export version (u16, currently 1) |
 * | 6      | 2     | export count (u16, 1..P4_CONFIG_NATIVE_MAX_EXPORTS) |
 * | 8      | 4*N   | entry offsets (u32, relative to payload base) |
 *
 * Entry ABI: `int entry(const native_exec_api_t *api, int argc, char **argv)`.
 * Its return value becomes the shell ERRORLEVEL. The API table is a stable,
 * versioned struct copied from the boot-installed provider; no payload ever
 * supplies its own function pointers.
 */

#ifndef P4MINISHELL_NATIVEEXEC_H
#define P4MINISHELL_NATIVEEXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "native_payload.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed payload export table constants (protocol, not configuration). */
#define NATIVE_EXEC_EXPORT_MAGIC       "P4EX"
#define NATIVE_EXEC_EXPORT_VERSION     1u
#define NATIVE_EXEC_EXPORT_HDR_BYTES   8u
#define NATIVE_EXEC_ENTRY_ALIGN        4u

/** Stable API table version; bump only on a breaking ABI change. */
#define NATIVE_EXEC_API_VERSION        1u

/* Configuration with safe fallbacks. The authoritative entries belong in
 * p4minishell_config.h; the build owner is asked to add them (see handoff). */
#ifndef P4_CONFIG_NATIVE_MAX_EXPORTS
#define P4_CONFIG_NATIVE_MAX_EXPORTS   16u
#endif
#ifndef P4_CONFIG_NATIVE_API_SUPPORTED_MASK
#define P4_CONFIG_NATIVE_API_SUPPORTED_MASK                              \
    (NATIVE_PAY_API_CONSOLE | NATIVE_PAY_API_ENV |                       \
     NATIVE_PAY_API_STATE | NATIVE_PAY_API_TIME)
#endif

/** Loader status codes. Zero only means "fully verified AND dispatched". */
typedef enum {
    NATIVE_EXEC_OK              = 0,
    NATIVE_EXEC_ERR_ARG         = -1, /**< NULL argument or impossible view. */
    NATIVE_EXEC_ERR_ENVELOPE    = -2, /**< native_pay_* refused; see plan. */
    NATIVE_EXEC_ERR_ABI_MAGIC   = -3, /**< payload does not start with P4EX. */
    NATIVE_EXEC_ERR_ABI_VERSION = -4, /**< unknown fixed-ABI version. */
    NATIVE_EXEC_ERR_ABI_COUNT   = -5, /**< zero/too many exports or truncated table. */
    NATIVE_EXEC_ERR_ABI_ORDINAL = -6, /**< envelope ordinal outside the table. */
    NATIVE_EXEC_ERR_ABI_OFFSET  = -7, /**< entry offset outside blob or unaligned. */
    NATIVE_EXEC_ERR_API         = -8, /**< required API bits include unsupported groups. */
    NATIVE_EXEC_ERR_NO_PROVIDER = -9, /**< no API table installed at boot. */
    NATIVE_EXEC_ERR_MAP         = -10 /**< no executable-memory map hook or mapping failed. */
} native_exec_status_t;

/** Pure outcome of inspecting one envelope. Every field is filled on OK and
 *  on a structural failure; `envelope_status` is the exact native_pay code. */
typedef struct {
    int envelope_status;      /**< native_pay_parse() code (header verdict). */
    int verify_status;        /**< native_pay_check() code (trust verdict). */
    int abi_status;           /**< native_exec_status_t for the export table. */
    bool api_ok;              /**< required API bits are all supported. */
    uint16_t entry_ordinal;   /**< from the signed envelope header. */
    uint32_t required_api_set;/**< from the signed envelope header. */
    uint32_t export_count;    /**< decoded export-table count. */
    uint32_t entry_offset;    /**< offset of the selected entry in the payload. */
    size_t payload_size;      /**< payload byte count from the envelope. */
} native_exec_plan_t;

/** Derived layout of the fixed export table (no relocation is performed). */
typedef struct {
    uint32_t export_count;
    uint32_t entry_offset;
    size_t header_bytes;
} native_exec_layout_t;

/** Stable API table handed to the single registered entry ABI. Function
 *  pointers are supplied by the boot provider only; unchecked groups stay
 *  NULL and are refused through P4_CONFIG_NATIVE_API_SUPPORTED_MASK. */
typedef struct {
    uint32_t version;                              /**< NATIVE_EXEC_API_VERSION. */
    int (*console_print)(const char *text);        /**< NATIVE_PAY_API_CONSOLE. */
    const char *(*env_get)(const char *key);       /**< NATIVE_PAY_API_ENV. */
    int (*env_set)(const char *key, const char *value);
    int (*state_get)(const char *key, char *out, size_t size); /**< ..._STATE. */
    int (*state_set)(const char *key, const char *value);
    uint32_t (*time_ms)(void);                     /**< NATIVE_PAY_API_TIME. */
} native_exec_api_t;

/** The one entry ABI. Identical call shape for every registered payload. */
typedef int (*native_exec_entry_t)(const native_exec_api_t *api,
                                   int argc, char **argv);

/** Execute-memory seam. The map hook returns a writable, executable address
 *  or NULL; the unmap hook releases it. Both are installed once at boot. */
typedef void *(*native_exec_map_fn)(const uint8_t *payload, size_t size);
typedef void (*native_exec_unmap_fn)(void *code, size_t size);

/** Validate the fixed export table at the start of a payload. This is pure:
 *  no memory is mapped and no function pointer is formed or called. */
int native_exec_abi_validate(const uint8_t *payload, size_t payload_size,
                             uint16_t ordinal, native_exec_layout_t *out);

/** True when @p required is a subset of P4_CONFIG_NATIVE_API_SUPPORTED_MASK. */
bool native_exec_api_supported(uint32_t required);

/** Structural plan: envelope header verdict + export table + API bits. No
 *  cryptography is run here, so a caller still has to verify the signature. */
int native_exec_plan(const uint8_t *envelope, size_t len,
                     native_exec_plan_t *out);

/** Full inspection: verify the envelope with the installed hooks/key, then
 *  plan. Never reports OK without a valid signature. */
int native_exec_inspect(const uint8_t *envelope, size_t len,
                        native_exec_plan_t *out);

/** Install (or, with NULL, clear) the stable API provider. The struct is
 *  copied; the caller owns any buffers its functions point at. */
void native_exec_set_api(const native_exec_api_t *api);
bool native_exec_api_ready(void);

/** Install (or, with NULL, clear) the executable-memory map hooks. */
void native_exec_set_map_ops(native_exec_map_fn map, native_exec_unmap_fn unmap);
bool native_exec_map_ready(void);

/** THE loader: inspect -> check API bits -> map -> call the one entry ABI ->
 *  unmap. No execution happens on any failure path. Returns the payload's
 *  exit code on success (>= 0) or a negative native_exec_status_t. */
int native_exec_launch(const uint8_t *envelope, size_t len,
                       int argc, char **argv);

/** Stable diagnostic name (never NULL). */
const char *native_exec_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_NATIVEEXEC_H */
