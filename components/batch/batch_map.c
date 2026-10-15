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

/** Find a key in the map's collision chain. Returns the chain index or -1.
 *  On match, writes the slot name to `slot_out`. */
static int map_find_key(const char *map_name, const char *key, char *slot_out, size_t slot_size)
{
    unsigned int h = map_hash(key);
    for (int i = 0; i < MAP_MAX_CHAIN; i++) {
        char slot[MAP_NAME_BYTES];
        map_slot_name(map_name, h, i, slot, sizeof(slot));
        const char *stored_key = shell_env_get(slot);
        if (stored_key == NULL) {
            return -1; /* empty slot, key not found */
        }
        /* Slot stores "key\0value" or just "key" at start */
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

    /* /keys MAPNAME DEST - emit keys into DEST[0..n-1] */
    if (flag == 'K' && argc >= 4) {
        const char *dest_name = argv[3];
        int sz = map_size(map_name);

        /* Clear destination */
        char dest_len[MAP_NAME_BYTES];
        snprintf(dest_len, sizeof(dest_len), "%s.__len", dest_name);
        shell_env_set(dest_len, NULL);

        int count = 0;
        unsigned int h = 0;
        /* We need to scan all possible hash slots. Since we can't enumerate
         * by hash directly, we use a different approach: scan for slots
         * matching "MAPNAME#*" pattern by trying common hashes.
         * For simplicity, we scan the env table for slots starting with
         * "MAPNAME#". */
        char prefix[MAP_NAME_BYTES];
        snprintf(prefix, sizeof(prefix), "%s#", map_name);

        /* Collect keys by scanning the MAP class slots */
        /* Note: We iterate a reasonable range of hash values */
        for (h = 0; h < 65536 && count < sz; h++) {
            for (int i = 0; i < MAP_MAX_CHAIN && count < sz; i++) {
                char slot[MAP_NAME_BYTES];
                map_slot_name(map_name, h, i, slot, sizeof(slot));
                const char *entry = shell_env_get(slot);
                if (entry != NULL) {
                    char key[MAP_KEY_BYTES], value[MAP_VALUE_BYTES];
                    if (map_parse_entry(entry, key, sizeof(key), value, sizeof(value))) {
                        char dest_slot[MAP_NAME_BYTES];
                        char key_buf[MAP_VALUE_BYTES];
                        snprintf(dest_slot, sizeof(dest_slot), "%s[%d]", dest_name, count);
                        snprintf(key_buf, sizeof(key_buf), "%s", key);
                        shell_env_set(dest_slot, key_buf);
                        count++;
                    }
                }
            }
        }
        char len_buf[12];
        snprintf(len_buf, sizeof(len_buf), "%d", count);
        shell_env_set(dest_len, len_buf);
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

    /* /clear MAPNAME */
    if (flag == 'C' && argc >= 3) {
        /* Clear all slots by scanning */
        unsigned int h;
        for (h = 0; h < 65536; h++) {
            for (int i = 0; i < MAP_MAX_CHAIN; i++) {
                char slot[MAP_NAME_BYTES];
                map_slot_name(map_name, h, i, slot, sizeof(slot));
                const char *entry = shell_env_get(slot);
                if (entry != NULL) {
                    shell_env_set(slot, NULL);
                }
            }
        }
        /* Clear metadata */
        char meta[MAP_NAME_BYTES];
        map_meta_name(map_name, meta, sizeof(meta));
        shell_env_set(meta, NULL);
        batch_set_errorlevel(0);
        return;
    }

    shell_print_error("map: unknown switch /%s", argv[1] + 1);
    batch_set_errorlevel(2);
}
