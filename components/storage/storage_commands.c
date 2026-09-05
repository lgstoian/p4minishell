/**
 * @file storage_commands.c
 * @brief DOS-style file command implementations for P4MiniShell.
 *
 * Holds every command that reads or writes the SD filesystem: navigation,
 * listing, the recursive directory tree, file manipulation, the extended DOS
 * tools (`attrib`, `label`, `xcopy`), the text utilities (`find`, `more`,
 * `fc`, `sort`), and the `sd` command family. The primitives these build on —
 * guarded SD sessions, path resolution, FATFS conversion, size formatting,
 * wildcard matching, and input redirection — live in storage.c.
 *
 * Every text-processing command resolves its input through
 * storage_resolve_input_source(), so an explicit filename, a `<` redirection,
 * and a `|` pipe stage all reach the same code path.
 *
 * All transcript output and debug logging go through shell.c.
 */

#include "storage_commands.h"
#include "storage.h"
#include "shell.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
