/**
 * @file storage_csv.c
 * @brief Pure RFC-4180-subset CSV line parser for the `csv` verbs.
 *
 * Comma separators, `"quoted"` fields (which may contain commas), and `""`
 * escapes inside quoted fields. The caller supplies a scratch buffer that
 * receives the dequoted fields back to back; each recorded field is an
 * offset/length pair into that scratch. No heap, no SD, no transcript, so
 * test/ exercises this file directly.
 *
 * When a field does not fit the remaining scratch its bytes are dropped but
 * the field is still counted, and parsing continues with the next field, so
 * the returned count is always the true width of the row.
 */

#include "storage_commands.h"
#include "p4minishell_config.h"

#include <string.h>

#ifndef P4_CONFIG_CSV_MAX_COLS
#define P4_CONFIG_CSV_MAX_COLS 32
#endif

#ifndef P4_CONFIG_CSV_FIELD_BYTES
#define P4_CONFIG_CSV_FIELD_BYTES 256
#endif

int csv_split_line(const char *line, char *scratch, size_t scratch_size,
                   csv_field_t *fields, int max_fields)
{
    size_t used = 0;
    int count = 0;
    const char *p;

    if (line == NULL || scratch == NULL || scratch_size == 0 ||
        fields == NULL || max_fields <= 0) {
        return 0;
    }
    scratch[0] = '\0';
    /* An empty line carries no fields; anything else carries at least one. */
    if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r') {
        return 0;
    }
    p = line;
    for (;;) {
        size_t field_start = used;
        size_t field_len = 0;
        bool quoted = (*p == '"');

        if (quoted) {
            p++;
            while (*p != '\0' && *p != '\n' && *p != '\r') {
                if (*p == '"') {
                    if (*(p + 1) == '"') {
                        /* Doubled quote: one literal quote. */
                        if (used + 1 < scratch_size) {
                            scratch[used++] = '"';
                        }
                        field_len++;
                        p += 2;
                    } else {
                        /* Closing quote. */
                        p++;
                        break;
                    }
                } else {
                    if (used + 1 < scratch_size) {
                        scratch[used++] = *p;
                    }
                    field_len++;
                    p++;
                }
            }
            /* Skip to the comma or the line end (tolerates junk after the
             * closing quote instead of failing the whole row). */
            while (*p != '\0' && *p != ',' && *p != '\n' && *p != '\r') {
                p++;
            }
        } else {
            while (*p != '\0' && *p != ',' && *p != '\n' && *p != '\r') {
                if (used + 1 < scratch_size) {
                    scratch[used++] = *p;
                }
                field_len++;
                p++;
            }
        }
        /* Record only fields fully held by the scratch; every field is
         * still counted so the caller learns the true row width. */
        if (field_start + field_len < scratch_size && count < max_fields) {
            scratch[field_start + field_len] = '\0';
            fields[count].off = field_start;
            fields[count].len = field_len;
            used = field_start + field_len + 1;
        } else if (count < max_fields) {
            /* Partial field: rewind so the next field reuses the space. */
            used = field_start;
            if (used < scratch_size) {
                scratch[used] = '\0';
            }
        }
        count++;
        if (*p == '\0' || *p == '\n' || *p == '\r') {
            return count;
        }
        /* Skip the comma. A comma at the line end opens one trailing empty
         * field on the next pass. */
        p++;
        if (*p == '\0' || *p == '\n' || *p == '\r') {
            if (count < max_fields && used < scratch_size) {
                scratch[used] = '\0';
                fields[count].off = used;
                fields[count].len = 0;
            }
            count++;
            return count;
        }
    }
}
