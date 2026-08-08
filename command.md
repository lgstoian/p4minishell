# P4MiniShell Command Reference

Complete reference for all shell commands available in P4MiniShell.

## ANSI/VT Color Support

Every command uses the same built-in colour scheme by default. There is nothing to
configure: the palette is compiled in and applied automatically on both the LVGL
transcript and the serial console.

Colours are defined once in `components/ansi/ansi_palette.h` as named `SH_*` macros.
Commands compose output from those names rather than choosing colours themselves, so
the whole shell stays consistent and a palette change is a single-file edit.

### Default colour scheme

| Category | Colour | Palette macro |
|----------|--------|---------------|
| Section headings and titles | Bright green | `SH_HEAD` |
| Sub-headings | Bright yellow | `SH_SUBHEAD` |
| Field labels and keys | Cyan | `SH_LBL` |
| Body text | Bright green | `SH_TEXT` |
| Muted, secondary, timestamps | Bright black (grey) | `SH_MUTE` |
| Success, connected, OK | Green / bright green | `SH_OK`, `SH_OK_HI` |
| Errors and failures | Red / bright red | `SH_ERR`, `SH_ERR_HI` |
| Warnings | Yellow / bright yellow | `SH_WARN`, `SH_WARN_HI` |
| Important values (SSID, IP, MAC) | Bright white | `SH_VAL` |
| Numbers, sizes, percentages | Bright magenta | `SH_NUM` |
| Paths and filenames | Bright blue | `SH_PATH` |
| Prompt and command names | Bright cyan | `SH_PROMPT`, `SH_CMD` |
| Usage syntax | Yellow | `SH_USAGE` |
| Help descriptions | White | `SH_DESC` |
| Directory entries | Bold bright blue | `SH_DIR` |
| File entries | White | `SH_FILE` |
| Executable / `.bat` entries | Bright green | `SH_EXE` |
| Listing sizes | Bright magenta | `SH_SIZE` |
| Listing timestamps | Grey | `SH_TIME` |
| Wi-Fi connected / disconnected | Green / grey | `SH_NET_UP`, `SH_NET_DOWN` |
| Bluetooth | Magenta | `SH_BT` |
| USB attached / detached | Green / grey | `SH_USB_UP`, `SH_USB_DOWN` |
| OTA progress and success | Bright green | `SH_OTA` |
| Reset | Default | `SH_RST` |

### Where colour shows up

- **`dir` and `sd ls`** colour each entry by kind: directories in bold bright blue,
  runnable `.bat` files in bright green, ordinary files in white, with grey timestamps
  and magenta sizes. `dir /b` is deliberately left uncoloured so redirected or piped
  output stays machine-parsable.
- **Status commands** (`sysinfo`, `wifi status`, `usb status`, `chkdsk`) use cyan
  labels with bright-white values and bright-magenta numbers.
- **Errors, warnings, and usage lines** are consistent across every command, because
  they all go through the same helpers.
- **`c6ota`** shows progress in bright green and the destructive YES confirmation in
  bright red.

### ANSI format specifiers

These are the low-level codes the palette macros expand to. Prefer the `SH_*` names in
new code; these are listed for reference and for the rare case that needs a raw colour.

| Specifier | Meaning |
|-----------|---------|
| `@R` | Reset all attributes |
| `@B` | **Bold** (not blue) |
| `@D` | Dim |
| `@I` | Italic |
| `@U` | Underline |
| `@k` | Foreground black — **near-invisible on the dark background; use `@K` for grey** |
| `@r` | Foreground red |
| `@g` | Foreground green |
| `@y` | Foreground yellow |
| `@b` | Foreground blue |
| `@m` | Foreground magenta |
| `@c` | Foreground cyan |
| `@w` | Foreground white |
| `@K` | Foreground bright black (grey) |
| `@E` | Foreground bright red |
| `@G` | Foreground bright green |
| `@Y` | Foreground bright yellow |
| `@L` | Foreground bright blue |
| `@M` | Foreground bright magenta |
| `@C` | Foreground bright cyan |
| `@W` | Foreground bright white |
| `@@` | A literal `@` |

Standard printf specifiers work alongside these, including flags, width, and
precision: `@c%-10s@R` and `@M%8.2f@R` behave as expected.

## UI Model

- Fixed top header bar with status icons (Wi-Fi, Bluetooth, USB, SD) and system panel (MEM, CPU, BAT) dynamically linked to FreeRTOS
- Scrollable transcript textarea for command output (read-only)
- Single-line input textarea with prompt for command entry
- On-screen LVGL keyboard attached to input line
- Prev/Next buttons for 10-command recall history
- Serial console: idf.py monitor accepts same commands via UART/USB-Serial-JTAG
- Command submission: LV_EVENT_READY on input line (touch) or Enter (serial)
- Heavy commands run on dedicated worker task (not LVGL input callback stack)

## Where Commands Live

| Command group | Implemented in |
|---------------|----------------|
| `help`, `sysinfo`, `version`/`ver`, `about`, `mem`, `debug` | `components/shell/shell.c` |
| `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch` | `components/storage/storage_commands.c` |
| `attrib`, `label`, `xcopy`, `find`, `more`, `tree`, `fc`, `sort` | `components/storage/storage_commands.c` |
| `chkdsk`/`scandisk`, `format` | `components/storage/storage_commands.c` |
| `sd info|ls|stat|cat|eject`, `sdeject` | `components/storage/storage_commands.c` + `storage.c` |
| `set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`, `choice`, `setlocal`, `endlocal`, `exit` | `components/batch/batch.c` |
| Batch file execution, `:label` scanning, `for` loops, `\|` pipes, setlocal scoping | `components/batch/batch.c` |
| Keypress wait (`pause`, `choice`, `more`) and the `prompt` template engine | `components/shell/shell.c` |
| `brightness`, `rotate`, `battery`, `volume`, `gpio`, `display`, `keyboard`, `windows` | `components/command/command.c` |
| `reboot`, `clear`/`cls`, `prompt`, `date`, `time` | `components/command/command.c` |
| `wifi`, `bluetooth`/`bt`, `usb`, `c6ota` (family routing) | `components/command/command.c` → owning module |

Every command is reached through the single dispatcher `shell_execute_command_core()` in
`components/command/command.c`. `main/main.c` contains no command implementations. It routes
LVGL input events into `shell_execute_command_async()`, which queues the line on the command
worker task.

## System Commands

| Command | Description |
|---------|-------------|
| help | Show built-in command list |
| sysinfo | Show board, display, storage, heap, FreeRTOS tasks, uptime, Wi-Fi, and OTA state |
| clear / cls | Clear transcript history and redraw prompt |
| reboot | Restart the board |
| version / ver | Show app banner, ESP-IDF version, chip info, uptime, heap, and task count |
| about | Show shell and board summary with header description, uptime, and task count |
| debug | Show last 5 error/warning entries, Wi-Fi state, heap, warning count |
| mem | Show free heap, total heap, minimum heap, internal heap, task count, PSRAM state |

## Hardware Commands

### brightness <0-100>
Set LCD backlight brightness through the display manager's PWM path (BSP LEDC).
Uses `display_set_brightness()` from `components/display/`.

### rotate <0|90|180|270>
Rotate display and remap GT911 touch orientation to match.
Uses `display_set_rotation()` from `components/display/`. Triggers full UI rebuild
via the window manager's `windows_deinit()` + `windows_init()` cycle through the
registered callback. Touch remapping is handled automatically by the display manager.

### display info
Show comprehensive display information from the display manager: resolution, rotation,
brightness, refresh rate, power state, panel driver, touch driver, timing parameters,
buffer configuration, and MIPI DSI lane info. Uses `display_get_info()` / `display_print_info()`.

### display resolution
Show current effective and native display resolution. Uses `display_get_resolution()`.

### display refresh
Show current display refresh rate configuration. Uses `display_get_refresh_config()`.

### display power <on|sleep|off>
Control display power state. Uses `display_set_power_state()`. Sleep/off turns off
backlight; on restores it.

### windows info
Show window manager layout information: display dimensions, region rectangles for
header, transcript, input row, and keyboard. Uses `windows_get_rect()` and
`windows_get_display_width()`/`windows_get_display_height()`.

### keyboard show|hide|toggle|status
Control the on-screen keyboard visibility. `hide` removes the keyboard and
expands the transcript area; `show` restores it. `toggle` switches between
visible and hidden. `status` reports current visibility, mode, and height.

### battery
Read battery ADC pin (GPIO53, 2:1 divider), show scaled voltage, estimated percentage (3.3V-4.2V range), raw ADC data, and light-sleep state.

### battery sleep <on|off|status>
Request or inspect light sleep. Only available when CONFIG_PM_ENABLE is enabled in sdkconfig.

### volume <0-100>
Set speaker volume through ES8311 codec path.

### gpio list
Show exposed board GPIO table with pin numbers, current levels, write policy, and role descriptions.

### gpio status
Show current levels and role text for all exposed board pins.

### gpio read <pin>
Read current logic level from any GPIO number.

### gpio set <pin> <0|1>
Drive a GPIO output. Only allowed for pins marked safe for writes in the shell pin table.

## DOS-Style File Commands

All file commands operate on SD card through guarded mount/unmount. Working directory and environment are RAM-only (not persisted across boots).

| Command | Description |
|---------|-------------|
| cd / chdir | Show current working directory |
| cd <path> | Change working directory (sd:/, /sdcard/, relative, ., ..) |
| dir [path] [options] | List directory entries (see the option table below) |
| copy <src> <dst> | Copy file on SD, with free-space and self-copy guards |
| move <src> <dst> | Move or rename file/directory |
| del / erase <path> | Delete file from SD |
| ren / rename <src> <dst> | Rename file or directory |
| md / mkdir <path> | Create directory |
| rd / rmdir <path> | Remove empty directory |
| type <path> | Print text-safe file preview (no raw binary) |
| write <path> <text> | Create or overwrite text file |
| append <path> <text> | Append text to file |
| touch <path> | Create empty file or refresh timestamp |

### dir options

Options may appear before or after the path, and the `:` separator is optional.

| Option | Meaning |
|--------|---------|
| /W | Wide multi-column listing; directories shown as `[name]` |
| /P | Pause after each screenful; Enter or Space continues, `Q` quits |
| /S | Recurse into subdirectories, with a grand total at the end |
| /B | Bare listing, names only. Under `/S` prints full paths so it can be piped |
| /L | Lowercase names |
| /A:attrs | Filter by attribute (see below) |
| /O:order | Sort order (see below) |

**Attribute filters** for `/A`. Combine letters freely; prefix any letter with `-`
to exclude instead of require.

| Letter | Matches |
|--------|---------|
| D | Directories |
| H | Hidden |
| S | System |
| R | Read-only |
| A | Archive |

**Sort orders** for `/O`. Prefix with `-` to reverse. Name is always the
tie-breaker, so listings are deterministic.

| Letter | Sorts by |
|--------|----------|
| N | Name |
| S | Size |
| E | Extension |
| D | Date and time |
| G | Directories first, then the other key |

Examples:

```
dir                        Detailed listing with timestamps and free space
dir /w                     Four-column wide listing
dir /s /b                  Every file in the tree, full paths, pipeable
dir *.log /o:-d            Log files, newest first
dir /a:d /o:n              Directories only, sorted by name
dir /a:-h /p               Skip hidden entries, pause each screenful
dir logs\ /s /o:gs         Recurse, directories first then by size
```

Each directory prints its own file and directory counts. A `/S` run adds a
grand total, and every non-bare listing ends with the free space on the volume.
Listings are bounded to 128 entries per directory and recursion to 8 levels.

## Environment and Batch Commands

| Command | Description |
|---------|-------------|
| set | List all RAM-only environment variables |
| set NAME=VALUE | Create or update environment variable |
| set /a NAME=<expr> | Evaluate integer arithmetic and store the result |
| set /p NAME=<prompt> | Prompt the user and store the typed line |
| path | Show current batch PATH |
| path <dir1>;<dir2>;... | Replace PATH for .bat lookup |
| echo <text> | Print text after variable expansion |
| echo on / echo off | Enable/disable batch command echoing |
| call <file.bat> [args] | Execute batch file with %1..%9 expansion |
| if [not] errorlevel N cmd | Run cmd when errorlevel is at least N |
| if [not] exist <file> cmd | Run cmd when the file exists |
| if [not] "a"=="b" cmd | Run cmd when the strings match |
| goto <label> | Jump to a `:label` in the running batch file |
| shift | Shift batch arguments left by one position |
| pause | Wait for a keypress |
| choice [/C:keys] [/N] [/T:c,secs] [/S] [text] | Wait for one of the listed keys |
| setlocal | Push a copy of the environment; later changes are local |
| endlocal | Pop the most recent setlocal scope |
| exit [code] | Leave every nested batch file, setting errorlevel |
| exit /b [code] | Leave only the current batch file |

### set /a — integer arithmetic

Evaluates a 32-bit signed integer expression. With an assignment the result is
stored; without one it is printed.

| Precedence | Operators |
|------------|-----------|
| Lowest | `\|` bitwise or |
| | `^` bitwise xor |
| | `&` bitwise and |
| | `<<` `>>` shifts |
| | `+` `-` add, subtract |
| | `*` `/` `%` multiply, divide, remainder |
| | `-` `~` `!` unary minus, bitwise not, logical not |
| Highest | `( )` grouping |

Compound assignments are supported: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`,
`^=`, `<<=`, `>>=`.

Numbers may be decimal, `0x` hexadecimal, or leading-zero octal. A bare name
reads an environment variable, and **an undefined variable evaluates to 0**, so
a counter works on first use without initialising it.

```
set /a total=2+3*4          total=14
set /a total=(2+3)*4        total=20
set /a count+=1             Increment, works even when count is undefined
set /a mask=0xF0 >> 4       mask=15
set /a 100/7                Prints 14 without storing anything
```

Divide by zero, unbalanced parentheses, and malformed input are reported and
set errorlevel to 1 rather than producing a wrong answer.

### set /p — prompted input

Prints the prompt and reads a line from the user.

```
set /p name=Enter your name:
set /p answer=Continue? 
```

Backspace edits, ESC cancels, Enter submits. **An empty line leaves the
variable unchanged**, matching DOS. Errorlevel is set to 1 when the input was
cancelled, timed out, or no interactive key source is attached, so a batch file
can branch on it.

### Line continuation

A trailing `^` joins the next physical line, letting a long command be split
for readability:

```
copy "very long source name.txt" ^
     "very long destination name.txt"
```

Up to 8 lines may be joined. A doubled `^^` at end of line is an escaped
literal caret, not a continuation.

### Batch File Features
- %0 (script name), %1 through %9, and %* (all arguments) expansion
- %VAR% environment variable expansion
- rem and :: comment lines
- @ line prefix to suppress echo for one line
- echo on/off flow control
- `:label` targets for `goto` and `call :label`
- `for %%var in (set) do command` loops
- PATH-based .bat lookup
- Nested calls up to 4 levels deep
- setlocal/endlocal scoping up to 8 levels deep, auto-unwound when a file returns
- `set /a` integer arithmetic and `set /p` prompted input
- Trailing `^` line continuation, up to 8 joined lines

### choice

Waits for one of the allowed keys and sets errorlevel to that key's 1-based index.
Unmatched keys are ignored.

| Switch | Meaning |
|--------|---------|
| /C:list | Allowed keys, default `YN`. Example: `/C:YNC` |
| /N | Do not display the key list |
| /T:c,secs | Default to key `c` after `secs` seconds |
| /S | Case-sensitive key matching |

Example: `choice /C:YNC /T:N,10 Overwrite the file` prints
`Overwrite the file [Y,N,C]?` and defaults to `N` after ten seconds.

### pause, choice, and more without a keyboard

`pause`, `choice`, and `more` block on a real keypress delivered by the UART console,
a USB keyboard, or the on-screen keyboard. When none of those is attached the commands
fall back to their configured delay (or the first choice) and say so, so a headless
board never stalls a batch file. Every wait is also bounded by a 30-second timeout.

### prompt

`prompt` sets a DOS-style template that drives both the UART console prompt and the
LVGL input line.

| Code | Expands to | Code | Expands to |
|------|------------|------|------------|
| `$p` | Current path | `$g` | `>` |
| `$n` | Drive letter | `$l` | `<` |
| `$d` | Current date | `$b` | `\|` |
| `$t` | Current time | `$q` | `=` |
| `$v` | Firmware version | `$a` | `&` |
| `$s` | Space | `$c` | `(` |
| `$_` | Newline | `$f` | `)` |
| `$$` | `$` | `$e` | ESC (UART only) |
| `$h` | Backspace over the previous character | | |

Codes are case-insensitive. An unknown code renders literally. `prompt` with no argument
shows the stored template and its rendered form; `prompt /?` lists the codes.

Example: `prompt $t $p$g ` renders as `14:32:07 /sdcard/logs> `.

### Quoting and escaping

| Form | Meaning |
|------|---------|
| `"text"` | Groups text into one argument. Variables still expand inside. |
| `'text'` | Groups text into one argument literally. Variables do NOT expand. |
| `^c` | Makes the next character literal, including `^&`, `^\|`, `^>`, `^<`, `^"`, `^%`, `^^` |

Quoting and escape markup is removed before a command sees its arguments, so
`echo "hello world"`, `echo 'hello world'`, and `echo hello^ world` all print
`hello world`.

Use quoting when a value may contain spaces or operators:

```
copy "my notes.txt" backup\          Space in a filename
echo "a | b"                         Pipe as data, not a pipeline
echo 'literal %PATH% text'           Percent signs kept verbatim
echo a^&b                            Ampersand as data, not a chain separator
write log.txt "value ^"quoted^""     Escaped quotes inside a quoted run
```

The same rules apply everywhere the shell looks for an operator: redirection targets,
pipe stages, and chain separators.

### Redirection

| Operator | Meaning |
|----------|---------|
| `> file` | Write command transcript output to an SD file (overwrite) |
| `>> file` | Append command transcript output to an SD file |
| `< file` | Read a file as the command's input |

All three may appear on one line in any order, for example
`sort < unsorted.txt > sorted.txt`. An operator that is quoted or caret-escaped is
treated as data.

### Command chaining

| Operator | Meaning |
|----------|---------|
| `a & b` | Run `b` after `a`, regardless of the outcome |
| `a && b` | Run `b` only if `a` succeeded |
| `a \|\| b` | Run `b` only if `a` failed |

Operators mix freely on one line, and up to 8 commands may be chained:

```
cd logs && dir                       List only if the directory change worked
type missing.txt || echo not found   Report only on failure
md backup & copy *.txt backup\       Both run regardless
badcmd || echo fallback              Unknown commands set errorlevel 9009
```

A command counts as failed when it is unrecognized or leaves a new non-zero
errorlevel behind.

A single `|` is the pipe operator, not a chain separator, so a pipeline can be one
link in a chain: `type f.txt | sort && echo sorted`.

Chain splitting happens before variable expansion, so a variable whose value contains
`&` cannot inject an extra command.

### Pipes

`cmd1 | cmd2 | cmd3` chains up to four stages. Each stage spools its output to a
temporary file on SD that the next stage reads, because the shell runs one command at a
time on a single worker task. A `|` inside double quotes is data, not a separator.

Example: `type log.txt | find "error" /I | sort /U`

The text-processing commands (`find`, `more`, `sort`) read the piped or redirected input
whenever no filename argument is supplied.

## DOS-Style Extended Commands

| Command | Description |
|---------|-------------|
| attrib [path] | Show file/directory attributes (R/H/S/A) |
| attrib +R\|-R <file> | Set or clear read-only attribute |
| attrib +H\|-H <file> | Set or clear hidden attribute |
| attrib +S\|-S <file> | Set or clear system attribute |
| attrib +A\|-A <file> | Set or clear archive attribute |
| label | Show current FAT volume label |
| label <name> | Set volume label (max 11 chars) |
| xcopy <src> <dst> [/S] | Copy files and directories recursively |

### Wildcard Support
- `*` matches any sequence of characters
- `?` matches any single character
- Supported in: `dir`, `del`, `copy`
- Example: `dir *.txt`, `del *.bak`, `copy *.c backup\`

## Volume Management

| Command | Description |
|---------|-------------|
| chkdsk [path] [/F] | Report volume capacity; `/F` also verifies every directory is readable |
| scandisk | Alias for `chkdsk` |
| format [/FS:type] [/V:label] [/Q] | Reformat the SD card, destroying all data |

### chkdsk

Reports the volume label, total, used and free space, the allocation unit size,
and the total and free cluster counts.

`/F` additionally walks every directory verifying that each entry can be read,
reporting file and directory counts plus any directory it could not open.
Recursion is bounded to 8 levels.

**The check is read-only.** This firmware never rewrites FAT structures. A card
with genuine corruption should be imaged and repaired on a host machine, so
`chkdsk` reports problems rather than attempting an in-place fix that could make
the damage worse.

```
chkdsk
chkdsk /F
chkdsk logs\ /F
```

### format

Reformats the SD card. **All data is destroyed.**

| Option | Meaning |
|--------|---------|
| /FS:type | Filesystem type: `FAT`, `FAT32`, or `EXFAT` |
| /V:label | Volume label to apply afterwards, 11 characters or fewer |
| /Q | Accepted for DOS familiarity; the underlying operation is always quick |

The command prints a warning and requires the exact word `YES` typed at the
prompt before anything is written. The confirmation is read one key at a time
through the shell's key queue, so the reply never reaches the command
dispatcher.

If no interactive input source is attached (no UART console and no USB
keyboard) the command **refuses outright** rather than proceeding, so a batch
file can never silently wipe a card.

```
format
format /FS:FAT32 /V:DATA
```

## Storage Guardrails

Larger file operations check the volume before they start, so a failure is
reported up front instead of leaving a half-written file behind.

- **Free-space precheck** — `copy`, `move`, `write`, and `append` refuse an
  operation that would leave less than 64 KB free. An overwrite credits the
  space the destination already occupies, so replacing a file in place does not
  need double the room.
- **Self-copy protection** — `copy a.txt a.txt` and the equivalent `move` are
  refused. Without this the destination would be opened for truncation and the
  source destroyed before the first read. The comparison is case-insensitive
  because FAT is.
- **Partial-destination cleanup** — if a copy fails mid-transfer the incomplete
  destination is deleted and the removal is reported, so a truncated file never
  masquerades as a complete one.
- **Progress reporting** — copies of files larger than 256 KB report percentage
  progress in 10% steps.
- **Capacity reporting** — `sd info`, `chkdsk`, and the end of every `dir`
  listing show current free space.
- **Attribute preservation** — `copy`, `move`, and `xcopy` carry the source's
  R/H/S/A attributes to the destination, and `xcopy /S` carries them onto the
  directories it creates. Attributes are applied after the data is written,
  because a read-only destination cannot be opened for writing. A failure to
  set them is logged but does not fail an otherwise-complete copy. Copying onto
  an existing read-only file reports `use attrib -R to clear it`.

## Text and Directory Utilities

All of these resolve relative paths against the current directory and run inside a
guarded SD session.

| Command | Description |
|---------|-------------|
| find <text> [file] [/I] [/N] [/C] [/V] | Search a file for a literal substring |
| more [file] | Page a text file, waiting for a key between pages |
| tree [path] [/F] [/A] | Draw a recursive directory outline |
| fc <file1> <file2> | Compare two text files line by line |
| sort [file] [/R] [/I] [/U] | Print a file with its lines sorted |

### find

| Switch | Meaning |
|--------|---------|
| /I | Case-insensitive match |
| /N | Prefix each match with its line number |
| /C | Print only the match count |
| /V | Print the lines that do NOT match |

With no file argument, `find` reads the pending `<` or pipe input source.

Example: `find "timeout" boot.log /I /N`

### more

Prints 20 lines per page (`P4_CONFIG_MORE_PAGE_LINES`), then waits at the
`-- More --` prompt. Enter or Space advances one page; `Q` stops. With no file argument
it reads the pending `<` or pipe input source.

### tree

Recursive directory outline with DOS box-drawing connectors.

| Switch | Meaning |
|--------|---------|
| /F | Include files as well as directories (DOS default is directories only) |
| /A | Use plain ASCII connectors |

Recursion is bounded to 8 levels (`P4_CONFIG_TREE_DEPTH_MAX`) and the total entry count
to 128 (`P4_CONFIG_SD_LIST_LIMIT`).

Example output:

```
Folder PATH listing for volume A:
/sdcard/projects
+---src\
|   +---main.c
|   \---util.c
\---docs\
    \---readme.txt

2 directories, 3 file(s)
```

### fc

Reports each differing line with both files' contents, and flags lines present in only
one file when the lengths differ. Prints `FC: no differences encountered` when the files
match.

### sort

| Switch | Meaning |
|--------|---------|
| /R | Reverse (descending) order |
| /I | Case-insensitive comparison |
| /U | Drop duplicate lines after sorting |

Holds up to 1024 lines (`P4_CONFIG_SORT_LINE_MAX`) and reports when the input is
truncated. With no file argument it reads the pending `<` or pipe input source.

Example: `sort names.txt /I /U > unique.txt`

## Wi-Fi Commands

| Command | Description |
|---------|-------------|
| wifi status | Show Wi-Fi runtime state, target SSID, AP info, IP info |
| wifi scan | Scan for nearby access points (SSID, RSSI, channel, auth mode) |
| wifi diag | Run full diagnostic: connection state, IP status, network scan |
| wifi connect | Connect using sdkconfig default credentials |
| wifi connect <ssid> <pass> | Connect with runtime credentials (password masked) |
| wifi disconnect | Disconnect current station session |

### Wi-Fi Behavior
- Boot-time startup in background task (does not block shell UI)
- ESP-Hosted version compatibility gate: refuses init if C6 firmware != host 2.12.x
- Recovery guidance points to coprocessor/esp32c6_slave or c6ota default
- Restores automatically after successful c6ota
- Transcript-facing diagnostics on boot and post-OTA restore

## Bluetooth Commands

| Command | Description |
|---------|-------------|
| bluetooth status | Show hosted Bluetooth readiness, NimBLE state, advertising state |
| bluetooth scan | BLE scan through hosted NimBLE on C6 (up to 8 devices) |
| bluetooth advertise on | Start non-connectable BLE advertising |
| bluetooth advertise off | Stop BLE advertising |
| bt ... | Alias for bluetooth command family |

### Bluetooth Lifecycle
- bluetooth enable initializes hosted controller and NimBLE host once
- Subsequent scan/advertise commands reuse active session
- Hosted NimBLE VHCI on ESP32-C6 over ESP-Hosted SDIO

## USB Commands

| Command | Description |
|---------|-------------|
| usb status | Show USB host, MSC, and HID state |
| usb ls [path] | Mount MSC drive at /usb0 and list files |
| usb keyboard on | Enable transcript echo for HID boot keyboard |
| usb keyboard off | Disable keyboard transcript echo |
| usb mouse on | Enable transcript echo for HID boot mouse |
| usb mouse off | Disable mouse transcript echo |

### USB Keyboard Auto-Detect
- Plug in a USB HID keyboard to automatically type commands into the shell
- On-screen keyboard is automatically hidden when USB keyboard is detected
- On-screen keyboard is restored when USB keyboard is unplugged
- Full US keyboard layout supported: letters, numbers, symbols, keypad, navigation keys, function keys
- Modifier keys (Shift, Ctrl, Alt, GUI) are tracked for proper character mapping
- Special keys: Enter (submit command), Backspace, ESC (clear line), Tab, arrows (cursor/history), Delete, Home, End
- Use `keyboard show` to force the on-screen keyboard visible even with USB keyboard attached
- Use `keyboard hide` to hide it again; auto-detect resumes on next plug/unplug event

## SD Tools

| Command | Description |
|---------|-------------|
| sd | Show SD status and available subcommands |
| sd info | Mount SD; report card metadata, root availability, and filesystem total/used/free |
| sd ls [path] | List directory with full long filenames, entry types, sizes |
| sd stat <path> | Show resolved path, type, size, mode for file/directory |
| sd cat <path> [max_bytes] | Text-safe file preview (1-8192 bytes, non-printable sanitized) |

## C6 OTA Commands

### c6ota <source>
Perform ESP-Hosted SDIO OTA update for ESP32-C6.

Sources:
- sd:/path/to/firmware.bin - Load image from SD card
- http://host/path or https://host/path - Download then transfer
- default - Auto-load esp32c6_hosted_slave.bin or network_adapter.bin from SD root

### OTA Flow
1. Validate source and show factory warning if needed (v2.3.0)
2. For HTTP/S: wait for Wi-Fi, download full image
3. Validate ESP-IDF app header (magic 0xE9 + chip ID 0x000D)
4. Prompt: WARNING: This will reboot the C6. Type YES to continue
5. Stop Wi-Fi, keep hosted transport alive
6. Transfer in 1500-byte chunks over SDIO
7. Progress: C6 OTA: XX% (YYYY KB / ZZZZ KB) every 5%
8. Success: C6 OTA completed successfully! Type reboot to activate new firmware.
9. Restore Wi-Fi on failure, request post-OTA restore on success

## Unsupported Commands

These commands are registered in the dispatcher but only print an error message.
They are listed here for reference and to prevent confusion if typed.

| Command | Reason |
|---------|--------|
| rgb led <color> | No RGB LED wiring declared in board metadata; stub prints an error |
| rgb <r> <g> <b> | No RGB LED wiring declared in board metadata; stub prints an error |
| camera init | No camera stack in current workspace; stub prints an error |
| camera snap <filename> | No camera stack in current workspace; stub prints an error |

## Runtime Notes

- sd ls shows full long filenames (FATFS LFN, 255-char limit, UTF-8)
- Directory listings bounded to 128 entries per directory
- Directory recursion (`dir /s`, `tree`, `chkdsk /F`) bounded to 8 levels
- sd cat preview bounded to 8192 bytes
- **Recursive walkers keep their per-level buffers on the heap**, not the worker task stack.
  `dir /s`, `tree`, and `chkdsk /F` each release one level's buffer before descending, so
  memory use stays flat with depth and the 8 KB task stack is never at risk.
- **Free-space guardrails**: writes that would leave under 64 KB free are refused before any
  file is opened, so an overwrite cannot destroy existing contents and then fail
- **`format` requires interactive confirmation** and refuses to run when no key source is
  attached, so it can never execute unattended from a batch file
- **`chkdsk` is read-only** and never rewrites FAT structures
- SD VO4 LDO acquired at 3300 mV before mounts (no ldo warning spam)
- Commands run on worker task (prevents LVGL stack overflow)
- Family commands (wifi, sd, c6ota) receive full unsplit command text
- Serial prompt stateful: no P4Shell> spam during idle polling
- Password masking in transcript and command recall history (applied at both the caller and inside the history store, so no path can persist a Wi-Fi password)
- **Variable expansion & redirection**: Environment variables (`%VAR%`), batch script name (`%0`), batch arguments (`%1`..`%9`), all arguments (`%*`), output redirection (`>` / `>>`), and input redirection (`<`) are handled by `shell_execute_command()` in `components/command/command.c` before dispatch
- Nested execution contexts (`if`, `for`, pipes, batch lines) re-enter the full pipeline, so they inherit variable expansion and redirection
- Redirection captures exactly the transcript delta produced by the command, using
  `shell_transcript_get_length()` before dispatch and `shell_transcript_get_text_from()` after
- Input redirection and pipe stages both publish through one slot in `components/storage/`, so
  `sort < f.txt` and `type f.txt | sort` reach the same code in the text-processing commands
- Pipe stages spool through SD rather than streaming: the shell runs one command at a time on a
  single worker task, so there is no second process to stream into. Every spool file is removed
  on every exit path.
- **Keypress wait**: `pause`, `choice`, and `more` block on a real keystroke from the UART
  console, a USB keyboard, or the on-screen keyboard. While a wait is active every input source
  routes keys to the wait instead of the command line, so an answer is never dispatched as a
  command. Waits are bounded by a 30-second timeout, and fall back to a timed delay when no
  interactive key source is attached.
- **Prompt template**: `prompt` drives both the UART console and the LVGL input line from one
  template. The input line snapshots the prefix it painted, so a template or path change between
  two LVGL events cannot corrupt command extraction.
- **One quote/escape scanner**: the argument tokenizer, redirection parsing, pipe splitting,
  chain splitting, and variable expansion all call the same helpers in `components/shell/`, so
  they can never disagree about whether a character is syntax or data
- **Parse order**: chain separators are found before variable expansion, and expansion runs per
  chain segment. A variable whose value contains `&`, `|`, or `>` therefore cannot inject
  syntax into the line — its contents are always data.
- **Chain success test**: a link succeeds when it was recognized and did not leave a new
  non-zero errorlevel. The outcome is tracked per link rather than re-read from the global
  errorlevel, so a stale value from an earlier line cannot pick the wrong branch.
