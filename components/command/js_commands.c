#include "p4minishell_config.h"
#include "jsengine.h"
#include "shell.h"
#include "storage.h"
#include <string.h>
#include <stdlib.h>

static int js_command_eval(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("js eval <code>");
        return 2;
    }

    char *code = malloc(P4_CONFIG_JS_EVAL_MAX_BYTES);
    if (!code) {
        shell_print_error("Out of memory");
        return 1;
    }

    code[0] = '\0';
    size_t pos = 0;
    for (int i = 1; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (pos + len + 2 >= P4_CONFIG_JS_EVAL_MAX_BYTES) {
            break;
        }
        if (i > 1) {
            code[pos++] = ' ';
        }
        memcpy(code + pos, argv[i], len);
        pos += len;
    }
    code[pos] = '\0';

    char *result = malloc(P4_CONFIG_JS_EVAL_MAX_BYTES);
    if (!result) {
        free(code);
        shell_print_error("Out of memory");
        return 1;
    }

    shell_sd_session_t sd_sess;
    shell_sd_begin(&sd_sess);
    bool ok = jsengine_eval(code, result, P4_CONFIG_JS_EVAL_MAX_BYTES);
    shell_sd_end(&sd_sess, "js eval");

    if (result[0] != '\0') {
        shell_transcript_appendf("%s\n", result);
    }
    free(code);
    free(result);
    return ok ? 0 : 1;
}

static int js_command_run(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("js run <file.js>");
        return 2;
    }

    char *result = malloc(P4_CONFIG_JS_EVAL_MAX_BYTES);
    if (!result) {
        shell_print_error("Out of memory");
        return 1;
    }

    shell_sd_session_t sd_sess;
    shell_sd_begin(&sd_sess);
    bool ok = jsengine_run_file(argv[1], result, P4_CONFIG_JS_EVAL_MAX_BYTES);
    shell_sd_end(&sd_sess, "js run");

    if (ok) {
        if (result[0]) {
            shell_transcript_appendf("%s\n", result);
        }
        free(result);
        return 0;
    } else {
        shell_transcript_appendf("%s\n", result);
        free(result);
        return 1;
    }
}

static int js_command_call(int argc, char **argv)
{
    if (argc < 3) {
        shell_print_usage("js call <file.js> <function> [args_json]");
        return 2;
    }

    const char *args_json = (argc >= 4) ? argv[3] : NULL;

    char *result = malloc(P4_CONFIG_JS_EVAL_MAX_BYTES);
    if (!result) {
        shell_print_error("Out of memory");
        return 1;
    }

    shell_sd_session_t sd_sess;
    shell_sd_begin(&sd_sess);
    bool ok = jsengine_call(argv[1], argv[2], args_json, result, P4_CONFIG_JS_EVAL_MAX_BYTES);
    shell_sd_end(&sd_sess, "js call");

    if (ok) {
        if (result[0]) {
            shell_transcript_appendf("%s\n", result);
        }
        free(result);
        return 0;
    } else {
        shell_transcript_appendf("%s\n", result);
        free(result);
        return 1;
    }
}

int shell_command_js(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("js <eval|run|call> ...");
        return 2;
    }

    /* The helpers index their own arguments from 1 (the first payload),
     * treating argv[0] as the subcommand name. Pass the narrowed view so
     * `js run file.js` does not try to open a file literally named "run"
     * and `js eval code` does not prepend the word "eval" to the script. */
    if (strcmp(argv[1], "eval") == 0) {
        return js_command_eval(argc - 1, argv + 1);
    } else if (strcmp(argv[1], "run") == 0) {
        return js_command_run(argc - 1, argv + 1);
    } else if (strcmp(argv[1], "call") == 0) {
        return js_command_call(argc - 1, argv + 1);
    }

    shell_print_usage("js <eval|run|call> ...");
    return 2;
}
