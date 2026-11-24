/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_map.c
 * @brief `map` verb — associative key->value store.
 *
 * A real hash map layered on the flat slot table, backed by hashed names
 * (MAP#<hash>) with short collision chains. All storage lives in the MAP
 * class slot slab; keys and values are stored as hidden env vars.
 */

#include "batch.h"
#include "shell.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_NAME_BYTES   P4_CONFIG_ENV_NAME_BYTES
#define MAP_VALUE_BYTES  P4_CONFIG_ENV_VALUE_BYTES
#define MAP_KEY_BYTES    P4_CONFIG_MAP_KEY_BYTES
#define MAP_MAX_CHAIN    P4_CONFIG_MAP_MAX_CHAIN

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

/** Simple hash function for string keys (djb2). */
static unsigned int map_hash(const char *key)
{
    unsigned int h = 5381;
    while (*key != '\0') {
        h = ((h << 5) + h) ^ (unsigned char)*key;
        key++;
    }
    return h;
}

/** Build a slot name for a map entry: "MAPNAME#hash#index". */
static void map_slot_name(const char *map_name, unsigned int hash, int chain_idx,
                          char *out, size_t out_size)
{
    snprintf(out, out_size, "%s#%u#%d", map_name, hash, chain_idx);
}

/** Build the metadata slot name for a map: "MAPNAME.__size". */
static void map_meta_name(const char *map_name, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s.__size", map_name);
}

static bool map_parse_entry(const char *entry, char *key_out, size_t key_size,
                             char *val_out, size_t val_size);

/** Find a key in the map's collision chain. Returns the chain index or -1.
 *  On match, writes the slot name to `slot_out`. Slots store "key=value", so
 *  the stored entry must be split before the key can be compared (the old
 *  whole-entry compare never matched a real key). */
static int map_find_key(const char *map_name, const char *key, char *slot_out, size_t slot_size)
{
    unsigned int h = map_hash(key);
    for (int i = 0; i < MAP_MAX_CHAIN; i++) {
        char slot[MAP_NAME_BYTES];
        map_slot_name(map_name, h, i, slot, sizeof(slot));
        const char *entry = shell_env_get(slot);
        char stored_key[MAP_KEY_BYTES];
        char stored_value[MAP_VALUE_BYTES];

        if (entry == NULL) {
            return -1; /* empty slot, key not found */
        }
        if (!map_parse_entry(entry, stored_key, sizeof(stored_key),
                             stored_value, sizeof(stored_value))) {
            continue;
        }
        if (strcmp(stored_key, key) == 0) {
            if (slot_out != NULL) {
                snprintf(slot_out, slot_size, "%s", slot);
            }
            return i;
        }
    }
    return -1;
}

/** Get the current map size. */
static int map_size(const char *map_name)
{
    char meta[MAP_NAME_BYTES];
    map_meta_name(map_name, meta, sizeof(meta));
    const char *val = shell_env_get(meta);
    return (val != NULL) ? (int)strtol(val, NULL, 10) : 0;
}

/** Set the map size. */
static esp_err_t map_set_size(const char *map_name, int size)
{
    char meta[MAP_NAME_BYTES];
    char numbuf[12];
    map_meta_name(map_name, meta, sizeof(meta));
    snprintf(numbuf, sizeof(numbuf), "%d", size);
    return shell_env_set(meta, numbuf);
}

/** Store a key-value pair in the map. The slot stores "key\0value". */
static esp_err_t map_store_slot(const char *map_name, const char *key, const char *value)
{
    unsigned int h = map_hash(key);
    /* Find existing or first empty slot in chain */
    for (int i = 0; i < MAP_MAX_CHAIN; i++) {
        char slot[MAP_NAME_BYTES];
        map_slot_name(map_name, h, i, slot, sizeof(slot));
        const char *existing = shell_env_get(slot);
        if (existing == NULL || strcmp(existing, key) == 0) {
            /* Store as "key=value" (separator is '=') */
            char entry[MAP_VALUE_BYTES];
            snprintf(entry, sizeof(entry), "%.*s=%.*s",
                     MAP_KEY_BYTES - 1, key,
                     (int)(MAP_VALUE_BYTES - MAP_KEY_BYTES - 2), value);
            return shell_env_set(slot, entry);
        }
    }
    return ESP_ERR_NO_MEM; /* chain full */
}

/** Parse "key=value" entry stored in a slot. */
static bool map_parse_entry(const char *entry, char *key_out, size_t key_size,
                            char *val_out, size_t val_size)
{
    const char *eq = strchr(entry, '=');
    if (eq == NULL) return false;
    size_t klen = (size_t)(eq - entry);
    if (klen >= key_size) klen = key_size - 1;
    memcpy(key_out, entry, klen);
    key_out[klen] = '\0';
    snprintf(val_out, val_size, "%s", eq + 1);
    return true;
}

/* ========================================================================
 * SHARED ENUMERATION / LOOKUP (used by /keys, /values and `for /K`)
 * ======================================================================== */

int shell_map_collect_keys(const char *map_name,
                           char keys[][P4_CONFIG_ENV_NAME_BYTES], int max)
{
    char prefix[MAP_NAME_BYTES];
    /* Scratch slot names are line-sized in aggregate: heap, never stack, on
     * the recursive batch path (32 KB worker stack rule). */
    char (*slots)[P4_CONFIG_ENV_NAME_BYTES] = NULL;
    int slot_count;
    int count = 0;

    if (map_name == NULL || map_name[0] == '\0' || keys == NULL || max <= 0) {
        return 0;
    }
    slots = malloc(sizeof(*slots) * (P4_CONFIG_ENV_MAP_SLOTS + 8));
    if (slots == NULL) {
        return 0;
    }
    snprintf(prefix, sizeof(prefix), "%s#", map_name);
    slot_count = shell_env_collect_prefix(prefix, slots, P4_CONFIG_ENV_MAP_SLOTS + 8);
    for (int i = 0; i < slot_count && count < max; i++) {
        const char *entry = shell_env_get(slots[i]);
        char key[MAP_KEY_BYTES];
        char value[MAP_VALUE_BYTES];

        if (entry == NULL ||
            !map_parse_entry(entry, key, sizeof(key), value, sizeof(value)) ||
            key[0] == '\0') {
            continue;
        }
        snprintf(keys[count], P4_CONFIG_ENV_NAME_BYTES, "%s", key);
        count++;
    }
    free(slots);
    return count;
}

/** Emit the collected keys (or their values, in the same order) into a
 *  DEST[0..n-1] array plus DEST.__len. One writer behind /keys and /values. */
static void map_emit_slots(const char *dest_name,
                           char keys[][P4_CONFIG_ENV_NAME_BYTES], int count,
                           const char *map_name, bool emit_values)
{
    char len_name[MAP_NAME_BYTES];
    char len_buf[12];

    snprintf(len_name, sizeof(len_name), "%s.__len", dest_name);
    shell_env_set(len_name, NULL);
    for (int i = 0; i < count; i++) {
        char slot[MAP_NAME_BYTES];
        char value[MAP_VALUE_BYTES];

        if (emit_values && map_name != NULL) {
            if (!shell_map_lookup(map_name, keys[i], value, sizeof(value))) {
                value[0] = '\0';
            }
        } else {
            snprintf(value, sizeof(value), "%s", keys[i]);
        }
        snprintf(slot, sizeof(slot), "%s[%d]", dest_name, i);
        shell_env_set(slot, value);
    }
    snprintf(len_buf, sizeof(len_buf), "%d", count);
    shell_env_set(len_name, len_buf);
}

bool shell_map_lookup(const char *map_name, const char *key,
                      char *out, size_t out_size)
{
    char slot[MAP_NAME_BYTES];
    const char *entry;
    char parsed_key[MAP_KEY_BYTES];
    char value[MAP_VALUE_BYTES];

    if (out == NULL || out_size == 0 || key == NULL) {
        return false;
    }
    out[0] = '\0';
    if (map_find_key(map_name, key, slot, sizeof(slot)) < 0) {
        return false;
    }
    entry = shell_env_get(slot);
    if (entry == NULL ||
        !map_parse_entry(entry, parsed_key, sizeof(parsed_key),
                         value, sizeof(value))) {
        return false;
    }
    snprintf(out, out_size, "%s", value);
    return true;
}

/* ========================================================================
 * VERB IMPLEMENTATION
 * ======================================================================== */

void shell_command_map(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage(
            "Usage: MAP /set MAPNAME KEY VALUE\n"
            "       MAP /get MAPNAME KEY DEST\n"
            "       MAP /has MAPNAME KEY\n"
            "       MAP /del MAPNAME KEY\n"
            "       MAP /keys MAPNAME DEST\n"
            "       MAP /values MAPNAME DEST\n"
            "       MAP /incr MAPNAME KEY [STEP]\n"
            "       MAP /size MAPNAME\n"
            "       MAP /clear MAPNAME");
        batch_set_errorlevel(2);
        return;
    }

    if (argc < 3) {
        shell_print_error("map: needs a map name");
        batch_set_errorlevel(2);
        return;
    }

    const char *map_name = argv[2];

    if (argv[1][0] != '/' || argv[1][1] == '\0') {
        shell_print_error("map: expected a /switch");
        batch_set_errorlevel(2);
        return;
    }

    char flag = (char)toupper((unsigned char)argv[1][1]);

    /* /set MAPNAME KEY VALUE */
    if (flag == 'S' && argc >= 5) {
        char key[MAP_KEY_BYTES];
        snprintf(key, sizeof(key), "%s", argv[3]);
        /* Value may contain spaces - join remaining args */
        char value[MAP_VALUE_BYTES];
        value[0] = '\0';
        for (int i = 4; i < argc; i++) {
            if (i > 4) strlcat(value, " ", sizeof(value));
            strlcat(value, argv[i], sizeof(value));
        }

        /* Check if key already exists (update in place) */
        char slot[MAP_NAME_BYTES];
        int chain_idx = map_find_key(map_name, key, slot, sizeof(slot));
        if (chain_idx >= 0) {
            /* Update existing */
            char entry[MAP_VALUE_BYTES];
            snprintf(entry, sizeof(entry), "%.*s=%.*s",
                     MAP_KEY_BYTES - 1, key,
                     (int)(MAP_VALUE_BYTES - MAP_KEY_BYTES - 2), value);
            shell_env_set(slot, entry);
        } else {
            /* New entry */
            esp_err_t err = map_store_slot(map_name, key, value);
            if (err != ESP_OK) {
                shell_print_error("map /set: %s",
                    err == ESP_ERR_NO_MEM ? "hash chain full" : "store failed");
                batch_set_errorlevel(1);
                return;
            }
            /* Increment size */
            map_set_size(map_name, map_size(map_name) + 1);
        }
        batch_set_errorlevel(0);
        return;
    }

    /* /get MAPNAME KEY DEST */
    if (flag == 'G' && argc >= 5) {
        char slot[MAP_NAME_BYTES];
        int chain_idx = map_find_key(map_name, argv[3], slot, sizeof(slot));
        if (chain_idx < 0) {
            shell_print_error("map /get: key '%s' not found", argv[3]);
            batch_set_errorlevel(1);
            return;
        }
        const char *entry = shell_env_get(slot);
        char key[MAP_KEY_BYTES], value[MAP_VALUE_BYTES];
        if (!map_parse_entry(entry, key, sizeof(key), value, sizeof(value))) {
            shell_print_error("map /get: corrupt entry");
            batch_set_errorlevel(1);
            return;
        }
        shell_env_set(argv[4], value);
        batch_set_errorlevel(0);
        return;
    }

    /* /has MAPNAME KEY */
    if (flag == 'H' && argc >= 4) {
        char slot[MAP_NAME_BYTES];
        int chain_idx = map_find_key(map_name, argv[3], slot, sizeof(slot));
        batch_set_errorlevel(chain_idx >= 0 ? 0 : 1);
        return;
    }

    /* /del MAPNAME KEY */
    if (flag == 'D' && argc >= 4) {
        char slot[MAP_NAME_BYTES];
        int chain_idx = map_find_key(map_name, argv[3], slot, sizeof(slot));
        if (chain_idx < 0) {
            shell_print_error("map /del: key '%s' not found", argv[3]);
            batch_set_errorlevel(1);
            return;
        }
        shell_env_set(slot, NULL);
        int sz = map_size(map_name);
        if (sz > 0) map_set_size(map_name, sz - 1);
        batch_set_errorlevel(0);
        return;
    }

    /* /keys MAPNAME DEST - emit keys into DEST[0..n-1]. The one collector is
     * shared with `for /K`, so there is no second hash-slot enumeration. */
    if (flag == 'K' && argc >= 4) {
        const char *dest_name = argv[3];
        char (*keys)[P4_CONFIG_ENV_NAME_BYTES] =
            malloc(sizeof(*keys) * (P4_CONFIG_ENV_MAP_SLOTS + 8));
        int count;

        if (keys == NULL) {
            shell_print_error("map /keys: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        count = shell_map_collect_keys(map_name, keys, P4_CONFIG_ENV_MAP_SLOTS + 8);
        map_emit_slots(dest_name, keys, count, NULL, false);
        free(keys);
        batch_set_errorlevel(0);
        return;
    }

    /* /values MAPNAME DEST - emit the values of the same keys, in the same
     * order as /keys (so `"map /keys" then "map /values"` line up). */
    if (flag == 'V' && argc >= 4) {
        const char *dest_name = argv[3];
        char (*keys)[P4_CONFIG_ENV_NAME_BYTES] =
            malloc(sizeof(*keys) * (P4_CONFIG_ENV_MAP_SLOTS + 8));
        int count;

        if (keys == NULL) {
            shell_print_error("map /values: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        count = shell_map_collect_keys(map_name, keys, P4_CONFIG_ENV_MAP_SLOTS + 8);
        map_emit_slots(dest_name, keys, count, map_name, true);
        free(keys);
        batch_set_errorlevel(0);
        return;
    }

    /* /incr MAPNAME KEY [STEP] - numeric increment in place. A missing key
     * starts at 0; the new value is printed and ERRORLEVEL is 0/1. */
    if (flag == 'I' && argc >= 4) {
        char value[MAP_VALUE_BYTES];
        bool existed;
        long current = 0;
        long step = (argc >= 5) ? strtol(argv[4], NULL, 10) : 1;
        char numbuf[24];

        if (map_name[0] == '\0' || argv[3][0] == '\0') {
            shell_print_error("map /incr: needs a map name and key");
            batch_set_errorlevel(2);
            return;
        }
        existed = shell_map_lookup(map_name, argv[3], value, sizeof(value));
        if (existed) {
            char *end = NULL;

            current = strtol(value, &end, 10);
            if (end == value) {
                current = 0;
            }
        }
        current += step;
        snprintf(numbuf, sizeof(numbuf), "%ld", current);
        if (map_store_slot(map_name, argv[3], numbuf) != ESP_OK) {
            shell_print_error("map /incr: hash chain full");
            batch_set_errorlevel(1);
            return;
        }
        if (!existed) {
            (void)map_set_size(map_name, map_size(map_name) + 1);
        }
        shell_transcript_appendf("%s\n", numbuf);
        batch_set_errorlevel(0);
        return;
    }

    /* /size MAPNAME */
    if (flag == 'Z' && argc >= 3) {
        char numbuf[12];
        snprintf(numbuf, sizeof(numbuf), "%d", map_size(map_name));
        shell_transcript_appendf("%s\n", numbuf);
        batch_set_errorlevel(0);
        return;
    }

    /* /clear MAPNAME — enumerate through the one shared collector. The old
     * bounded 0..65535 hash scan silently missed djb2 hashes above that range
     * (even a one-character key hashes larger), so entries survived /clear. */
    if (flag == 'C' && argc >= 3) {
        char prefix[MAP_NAME_BYTES];
        char (*slots)[P4_CONFIG_ENV_NAME_BYTES] =
            malloc(sizeof(*slots) * (P4_CONFIG_ENV_MAP_SLOTS + 8));
        int count;
        char meta[MAP_NAME_BYTES];

        if (slots == NULL) {
            shell_print_error("map /clear: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        snprintf(prefix, sizeof(prefix), "%s#", map_name);
        count = shell_env_collect_prefix(prefix, slots, P4_CONFIG_ENV_MAP_SLOTS + 8);
        for (int i = 0; i < count; i++) {
            (void)shell_env_set(slots[i], NULL);
        }
        free(slots);
        map_meta_name(map_name, meta, sizeof(meta));
        (void)shell_env_set(meta, NULL);
        batch_set_errorlevel(0);
        return;
    }

    shell_print_error("map: unknown switch /%s", argv[1] + 1);
    batch_set_errorlevel(2);
}
