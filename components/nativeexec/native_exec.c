/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file native_exec.c
 * @brief The one fixed-ABI native payload loader (pure logic + injected seams).
 *
 * No ESP-IDF header appears in this translation unit: the host lane compiles
 * it unchanged. Cryptography lives in nativepay, the trust key and mbedTLS
 * hooks come from the pkg/NVS core, and executable mapping is an installed
 * hook. That keeps a single loader with three explicit seams and no second
 * runtime, no ELF parser and no relocation step.
 */

#include "nativeexec.h"

#include <string.h>

static native_exec_api_t s_api;
static bool s_api_installed;
static native_exec_map_fn s_map;
static native_exec_unmap_fn s_unmap;

_Static_assert(sizeof(uintptr_t) == sizeof(native_exec_entry_t),
               "native payload ABI needs equal data/function pointer widths");

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int native_exec_abi_validate(const uint8_t *payload, size_t payload_size,
                             uint16_t ordinal, native_exec_layout_t *out)
{
    uint32_t count;
    uint32_t offset;
    size_t header_bytes;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (payload == NULL || out == NULL) {
        return NATIVE_EXEC_ERR_ARG;
    }
    if (payload_size < (size_t)NATIVE_EXEC_EXPORT_HDR_BYTES) {
        return NATIVE_EXEC_ERR_ABI_COUNT;
    }
    if (memcmp(payload, NATIVE_EXEC_EXPORT_MAGIC, 4) != 0) {
        return NATIVE_EXEC_ERR_ABI_MAGIC;
    }
    if (read_le16(payload + 4) != NATIVE_EXEC_EXPORT_VERSION) {
        return NATIVE_EXEC_ERR_ABI_VERSION;
    }
    count = read_le16(payload + 6);
    if (count == 0u || count > P4_CONFIG_NATIVE_MAX_EXPORTS) {
        return NATIVE_EXEC_ERR_ABI_COUNT;
    }
    header_bytes = (size_t)NATIVE_EXEC_EXPORT_HDR_BYTES + (size_t)count * 4u;
    if (header_bytes > payload_size) {
        return NATIVE_EXEC_ERR_ABI_COUNT;
    }
    if ((uint32_t)ordinal >= count) {
        return NATIVE_EXEC_ERR_ABI_ORDINAL;
    }

    offset = read_le32(payload + NATIVE_EXEC_EXPORT_HDR_BYTES + (size_t)ordinal * 4u);
    if (offset < (uint32_t)header_bytes || (size_t)offset >= payload_size ||
        (offset % NATIVE_EXEC_ENTRY_ALIGN) != 0u) {
        return NATIVE_EXEC_ERR_ABI_OFFSET;
    }
    out->export_count = count;
    out->entry_offset = offset;
    out->header_bytes = header_bytes;
    return NATIVE_EXEC_OK;
}

bool native_exec_api_supported(uint32_t required)
{
    return (required & ~(uint32_t)P4_CONFIG_NATIVE_API_SUPPORTED_MASK) == 0u;
}

int native_exec_plan(const uint8_t *envelope, size_t len,
                     native_exec_plan_t *out)
{
    native_pay_view_t view;
    native_exec_layout_t layout;
    int rc;

    if (out == NULL) {
        return NATIVE_EXEC_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    rc = native_pay_parse(envelope, len, &view);
    out->envelope_status = rc;
    if (rc != NATIVE_PAY_OK) {
        out->abi_status = NATIVE_EXEC_ERR_ENVELOPE;
        return NATIVE_EXEC_ERR_ENVELOPE;
    }

    out->entry_ordinal = view.entry_ordinal;
    out->required_api_set = view.required_api_set;
    out->payload_size = view.payload_size;

    rc = native_exec_abi_validate(view.payload, view.payload_size,
                                  view.entry_ordinal, &layout);
    out->abi_status = rc;
    if (rc != NATIVE_EXEC_OK) {
        return rc;
    }
    out->export_count = layout.export_count;
    out->entry_offset = layout.entry_offset;

    out->api_ok = native_exec_api_supported(view.required_api_set);
    return out->api_ok ? NATIVE_EXEC_OK : NATIVE_EXEC_ERR_API;
}

int native_exec_inspect(const uint8_t *envelope, size_t len,
                        native_exec_plan_t *out)
{
    native_pay_view_t view;
    int rc;

    if (out == NULL) {
        return NATIVE_EXEC_ERR_ARG;
    }
    rc = native_pay_check(envelope, len, &view);
    if (rc != NATIVE_PAY_OK) {
        memset(out, 0, sizeof(*out));
        out->envelope_status = rc;
        out->verify_status = rc;
        out->abi_status = NATIVE_EXEC_ERR_ENVELOPE;
        return NATIVE_EXEC_ERR_ENVELOPE;
    }
    rc = native_exec_plan(envelope, len, out);
    out->verify_status = NATIVE_PAY_OK;
    return rc;
}

void native_exec_set_api(const native_exec_api_t *api)
{
    if (api == NULL) {
        memset(&s_api, 0, sizeof(s_api));
        s_api_installed = false;
        return;
    }
    s_api = *api;
    /* A provider counts as installed only when every pointer promised by the
     * supported-API mask exists; a partial table is an explicit failure. */
    s_api_installed = s_api.version == NATIVE_EXEC_API_VERSION &&
                      s_api.console_print != NULL && s_api.env_get != NULL &&
                      s_api.env_set != NULL && s_api.state_get != NULL &&
                      s_api.state_set != NULL && s_api.time_ms != NULL;
}

bool native_exec_api_ready(void)
{
    return s_api_installed;
}

void native_exec_set_map_ops(native_exec_map_fn map, native_exec_unmap_fn unmap)
{
    if (map == NULL || unmap == NULL) {
        s_map = NULL;
        s_unmap = NULL;
        return;
    }
    s_map = map;
    s_unmap = unmap;
}

bool native_exec_map_ready(void)
{
    return s_map != NULL && s_unmap != NULL;
}

int native_exec_launch(const uint8_t *envelope, size_t len,
                       int argc, char **argv)
{
    native_pay_view_t view;
    native_exec_plan_t plan;
    native_exec_api_t api;
    native_exec_entry_t entry;
    void *code;
    uintptr_t entry_addr;
    int rc;

    /* Every gate below precedes the map and the indirect call. There is no
     * fallback path that runs an unverified or unsupported payload. */
    rc = native_exec_inspect(envelope, len, &plan);
    if (rc != NATIVE_EXEC_OK) {
        return rc;
    }
    if (!native_exec_api_ready()) {
        return NATIVE_EXEC_ERR_NO_PROVIDER;
    }
    if (!native_exec_map_ready()) {
        return NATIVE_EXEC_ERR_MAP;
    }
    rc = native_pay_parse(envelope, len, &view);
    if (rc != NATIVE_PAY_OK) {
        return NATIVE_EXEC_ERR_ENVELOPE;
    }

    code = s_map(view.payload, view.payload_size);
    if (code == NULL) {
        return NATIVE_EXEC_ERR_MAP;
    }
    /* One fixed ABI: the signed entry ordinal selects an offset, never a
     * symbol, section or relocation record. The cast is local to the loader. */
    entry_addr = (uintptr_t)code + (uintptr_t)plan.entry_offset;
    memcpy(&entry, &entry_addr, sizeof(entry));
    api = s_api;
    rc = entry(&api, argc, argv);
    s_unmap(code, view.payload_size);
    return rc;
}

const char *native_exec_status_name(int status)
{
    switch (status) {
        case NATIVE_EXEC_OK:              return "ok";
        case NATIVE_EXEC_ERR_ARG:         return "bad argument";
        case NATIVE_EXEC_ERR_ENVELOPE:    return "envelope refused";
        case NATIVE_EXEC_ERR_ABI_MAGIC:   return "payload abi magic mismatch";
        case NATIVE_EXEC_ERR_ABI_VERSION: return "payload abi version mismatch";
        case NATIVE_EXEC_ERR_ABI_COUNT:   return "bad export count";
        case NATIVE_EXEC_ERR_ABI_ORDINAL: return "entry ordinal out of range";
        case NATIVE_EXEC_ERR_ABI_OFFSET:  return "entry offset out of range";
        case NATIVE_EXEC_ERR_API:         return "unsupported required api";
        case NATIVE_EXEC_ERR_NO_PROVIDER: return "no api provider installed";
        case NATIVE_EXEC_ERR_MAP:         return "executable mapping failed";
        default:                          return "unknown";
    }
}
