/**
 * @file json_commands.c
 * @brief JSON verbs (`json validate|pretty <file>`).
 *
 * Minimal structural validation (nesting, strings, escapes, literals,
 * number shapes) plus pretty-printing with 2-space indent. Operates on
 * whole files within the viewer budget; errors report line/column.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "storage.h"
#include "ansi_palette.h"
#include "command.h"
#include "p4minishell_config.h"
#include "esp_err.h"

#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES

/* Viewer-sized budget shared with the md verb. */
#define JSON_FILE_MAX_BYTES   (64 * 1024)

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    size_t line;
    size_t col;
    char error[128];
} json_parser_t;

static void json_fail(json_parser_t *p, const char *what)
{
    snprintf(p->error, sizeof(p->error), "%s at line %u col %u",
             what, (unsigned)p->line, (unsigned)p->col);
}

static char json_peek(json_parser_t *p)
{
    if (p->pos >= p->len) {
        return '\0';
    }
    return p->src[p->pos];
}

static void json_advance(json_parser_t *p)
{
    if (p->pos < p->len) {
        if (p->src[p->pos] == '\n') {
            p->line++;
            p->col = 1;
        } else {
            p->col++;
        }
        p->pos++;
    }
}

static void json_skip_ws(json_parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            json_advance(p);
        } else {
            break;
        }
    }
}

/* Forward declarations (mutual recursion value <-> array/object). */
static bool json_parse_value(json_parser_t *p, char *out, size_t out_size,
                             size_t *out_len, int indent);

static bool json_out_str(json_parser_t *p, char *out, size_t out_size,
                         size_t *out_len, const char *s, size_t n)
{
    /* NULL out = validate/count mode: track length without storing. */
    if (out == NULL) {
        *out_len += n;
        return true;
    }
    if (*out_len + n >= out_size) {
        json_fail(p, "output too large");
        return false;
    }
    memcpy(out + *out_len, s, n);
    *out_len += n;
    return true;
}

static bool json_out_indent(json_parser_t *p, char *out, size_t out_size,
                            size_t *out_len, int indent)
{
    int i;
    for (i = 0; i < indent; i++) {
        if (!json_out_str(p, out, out_size, out_len, "  ", 2)) {
            return false;
        }
    }
    return true;
}

/* Parse "..." with escapes; echoes the raw literal (validates shape). */
static bool json_parse_string(json_parser_t *p, char *out, size_t out_size,
                              size_t *out_len)
{
    size_t start;

    if (json_peek(p) != '"') {
        json_fail(p, "expected string");
        return false;
    }
    start = p->pos;
    json_advance(p);
    while (p->pos < p->len) {
        char c = json_peek(p);
        if (c == '"') {
            json_advance(p);
            return json_out_str(p, out, out_size, out_len,
                                p->src + start, p->pos - start);
        }
        if (c == '\\') {
            json_advance(p);
            c = json_peek(p);
            /* \" \\ \/ \b \f \n \r \t \uXXXX */
            if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
                c == 'n' || c == 'r' || c == 't') {
                json_advance(p);
            } else if (c == 'u') {
                int k;
                json_advance(p);
                for (k = 0; k < 4; k++) {
                    c = json_peek(p);
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F'))) {
                        json_fail(p, "bad \\u escape");
                        return false;
                    }
                    json_advance(p);
                }
            } else if (c == '\0') {
                break;
            } else {
                json_fail(p, "bad escape");
                return false;
            }
            continue;
        }
        if (c == '\n' || c == '\0') {
            break;
        }
        if ((unsigned char)c < 0x20) {
            json_fail(p, "unescaped control in string");
            return false;
        }
        json_advance(p);
    }
    json_fail(p, "unterminated string");
    return false;
}

static bool json_parse_number(json_parser_t *p, char *out, size_t out_size,
                              size_t *out_len)
{
    size_t start = p->pos;

    if (json_peek(p) == '-') {
        json_advance(p);
    }
    if (json_peek(p) == '0') {
        json_advance(p);
    } else if (json_peek(p) >= '1' && json_peek(p) <= '9') {
        while (json_peek(p) >= '0' && json_peek(p) <= '9') {
            json_advance(p);
        }
    } else {
        json_fail(p, "bad number");
        return false;
    }
    if (json_peek(p) == '.') {
        json_advance(p);
        if (!(json_peek(p) >= '0' && json_peek(p) <= '9')) {
            json_fail(p, "bad fraction");
            return false;
        }
        while (json_peek(p) >= '0' && json_peek(p) <= '9') {
            json_advance(p);
        }
    }
    if (json_peek(p) == 'e' || json_peek(p) == 'E') {
        json_advance(p);
        if (json_peek(p) == '+' || json_peek(p) == '-') {
            json_advance(p);
        }
        if (!(json_peek(p) >= '0' && json_peek(p) <= '9')) {
            json_fail(p, "bad exponent");
            return false;
        }
        while (json_peek(p) >= '0' && json_peek(p) <= '9') {
            json_advance(p);
        }
    }
    return json_out_str(p, out, out_size, out_len,
                        p->src + start, p->pos - start);
}

static bool json_parse_literal(json_parser_t *p, char *out, size_t out_size,
                               size_t *out_len, const char *word)
{
    size_t n = strlen(word);
    if (p->pos + n > p->len || strncmp(p->src + p->pos, word, n) != 0) {
        json_fail(p, "bad literal");
        return false;
    }
    p->pos += n;
    p->col += n;
    return json_out_str(p, out, out_size, out_len, word, n);
}

static bool json_parse_array(json_parser_t *p, char *out, size_t out_size,
                             size_t *out_len, int indent)
{
    bool first = true;

    json_advance(p); /* [ */
    if (indent > 64) {
        json_fail(p, "too deeply nested");
        return false;
    }
    json_skip_ws(p);
    if (json_peek(p) == ']') {
        json_advance(p);
        return json_out_str(p, out, out_size, out_len, "[]", 2);
    }
    if (!json_out_str(p, out, out_size, out_len, "[\n", 2)) {
        return false;
    }
    while (true) {
        json_skip_ws(p);
        if (!json_out_indent(p, out, out_size, out_len, indent + 1)) {
            return false;
        }
        if (!json_parse_value(p, out, out_size, out_len, indent + 1)) {
            return false;
        }
        json_skip_ws(p);
        if (json_peek(p) == ',') {
            json_advance(p);
            if (!json_out_str(p, out, out_size, out_len, ",\n", 2)) {
                return false;
            }
            first = false;
            continue;
        }
        if (json_peek(p) == ']') {
            json_advance(p);
            if (!json_out_str(p, out, out_size, out_len, "\n", 1)) {
                return false;
            }
            if (!json_out_indent(p, out, out_size, out_len, indent)) {
                return false;
            }
            return json_out_str(p, out, out_size, out_len, "]", 1);
        }
        json_fail(p, first ? "expected ]" : "expected , or ]");
        return false;
    }
}

static bool json_parse_object(json_parser_t *p, char *out, size_t out_size,
                              size_t *out_len, int indent)
{
    bool first = true;

    json_advance(p); /* { */
    if (indent > 64) {
        json_fail(p, "too deeply nested");
        return false;
    }
    json_skip_ws(p);
    if (json_peek(p) == '}') {
        json_advance(p);
        return json_out_str(p, out, out_size, out_len, "{}", 2);
    }
    if (!json_out_str(p, out, out_size, out_len, "{\n", 2)) {
        return false;
    }
    while (true) {
        json_skip_ws(p);
        if (json_peek(p) != '"') {
            json_fail(p, first ? "expected string key" : "expected string key");
            return false;
        }
        if (!json_out_indent(p, out, out_size, out_len, indent + 1)) {
            return false;
        }
        if (!json_parse_string(p, out, out_size, out_len)) {
            return false;
        }
        json_skip_ws(p);
        if (json_peek(p) != ':') {
            json_fail(p, "expected :");
            return false;
        }
        json_advance(p);
        if (!json_out_str(p, out, out_size, out_len, ": ", 2)) {
            return false;
        }
        json_skip_ws(p);
        if (!json_parse_value(p, out, out_size, out_len, indent + 1)) {
            return false;
        }
        json_skip_ws(p);
        if (json_peek(p) == ',') {
            json_advance(p);
            if (!json_out_str(p, out, out_size, out_len, ",\n", 2)) {
                return false;
            }
            first = false;
            continue;
        }
        if (json_peek(p) == '}') {
            json_advance(p);
            if (!json_out_str(p, out, out_size, out_len, "\n", 1)) {
                return false;
            }
            if (!json_out_indent(p, out, out_size, out_len, indent)) {
                return false;
            }
            return json_out_str(p, out, out_size, out_len, "}", 1);
        }
        json_fail(p, first ? "expected }" : "expected , or }");
        return false;
    }
}

static bool json_parse_value(json_parser_t *p, char *out, size_t out_size,
                             size_t *out_len, int indent)
{
    char c;

    json_skip_ws(p);
    c = json_peek(p);
    if (c == '{') {
        return json_parse_object(p, out, out_size, out_len, indent);
    }
    if (c == '[') {
        return json_parse_array(p, out, out_size, out_len, indent);
    }
    if (c == '"') {
        return json_parse_string(p, out, out_size, out_len);
    }
    if (c == 't') {
        return json_parse_literal(p, out, out_size, out_len, "true");
    }
    if (c == 'f') {
        return json_parse_literal(p, out, out_size, out_len, "false");
    }
    if (c == 'n') {
        return json_parse_literal(p, out, out_size, out_len, "null");
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return json_parse_number(p, out, out_size, out_len);
    }
    if (c == '\0') {
        json_fail(p, "unexpected end");
        return false;
    }
    json_fail(p, "unexpected character");
    return false;
}

static void shell_command_json_usage(void)
{
    shell_transcript_appendf_ansi("Usage: json validate <file> | json pretty <file>\n");
    shell_transcript_appendf_ansi("  validate  - check structure, report line/col on error\n");
    shell_transcript_appendf_ansi("  pretty    - validate and print 2-space indented JSON\n");
}

/* Testable core: validate (pretty=false) or pretty-print buf[len] into out.
 * Returns true on success; err carries "msg at line L col C" on failure. */
bool json_validate_text(const char *text, size_t len, char *err, size_t err_size)
{
    json_parser_t parser;
    size_t out_len = 0;
    bool ok;

    if (text == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "null input");
        }
        return false;
    }
    memset(&parser, 0, sizeof(parser));
    parser.src = text;
    parser.len = len;
    parser.line = 1;
    parser.col = 1;
    ok = json_parse_value(&parser, NULL, 0, &out_len, 0);
    if (ok) {
        json_skip_ws(&parser);
        if (parser.pos < parser.len) {
            ok = false;
            json_fail(&parser, "trailing data");
        }
    }
    if (!ok && err != NULL && err_size > 0) {
        snprintf(err, err_size, "%s", parser.error);
    }
    return ok;
}

size_t json_pretty_text(const char *text, size_t len, char *out, size_t out_size,
                        char *err, size_t err_size)
{
    json_parser_t parser;
    size_t out_len = 0;
    bool ok;

    if (text == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    memset(&parser, 0, sizeof(parser));
    parser.src = text;
    parser.len = len;
    parser.line = 1;
    parser.col = 1;
    ok = json_parse_value(&parser, out, out_size, &out_len, 0);
    if (ok) {
        json_skip_ws(&parser);
        if (parser.pos < parser.len) {
            ok = false;
            json_fail(&parser, "trailing data");
        }
    }
    if (!ok) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "%s", parser.error);
        }
        return 0;
    }
    if (out_len + 1 < out_size) {
        out[out_len++] = '\n';
    }
    out[out_len < out_size ? out_len : out_size - 1] = '\0';
    return out_len;
}

void shell_command_json(int argc, char **argv)
{
    bool pretty;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    FILE *file = NULL;
    char *input = NULL;
    char *output = NULL;
    size_t got = 0;
    char err[128];

    if (argc != 3) {
        shell_command_json_usage();
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "validate")) {
        pretty = false;
    } else if (shell_text_equals_ignore_case(argv[1], "pretty")) {
        pretty = true;
    } else {
        shell_command_json_usage();
        batch_set_errorlevel(2);
        return;
    }

    if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("json: invalid path %s", argv[2]);
        batch_set_errorlevel(2);
        return;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_print_error("json: cannot open %s", argv[2]);
        batch_set_errorlevel(1);
        return;
    }
    /* Heap/PSRAM budgets (same class as the md verb). */
    input = malloc(JSON_FILE_MAX_BYTES + 1);
    output = malloc(JSON_FILE_MAX_BYTES * 2 + 1);
    if (input == NULL || output == NULL) {
        shell_transcript_appendf_ansi(SH_ERR "json: out of memory\n" SH_RST);
        batch_set_errorlevel(1);
        free(input);
        free(output);
        fclose(file);
        return;
    }
    got = fread(input, 1, JSON_FILE_MAX_BYTES, file);
    fclose(file);
    input[got] = '\0';

    if (pretty) {
        size_t n = json_pretty_text(input, got, output,
                                    JSON_FILE_MAX_BYTES * 2 + 1,
                                    err, sizeof(err));
        if (n == 0) {
            shell_transcript_appendf_ansi(SH_ERR "json: invalid: %s\n" SH_RST, err);
            batch_set_errorlevel(1);
        } else {
            shell_transcript_appendf("%s", output);
            batch_set_errorlevel(0);
        }
    } else if (json_validate_text(input, got, err, sizeof(err))) {
        shell_transcript_appendf_ansi(SH_OK "json: valid (%u bytes)\n" SH_RST,
                                      (unsigned)got);
        batch_set_errorlevel(0);
    } else {
        shell_transcript_appendf_ansi(SH_ERR "json: invalid: %s\n" SH_RST, err);
        batch_set_errorlevel(1);
    }
    free(input);
    free(output);
}
