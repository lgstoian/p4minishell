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
- Scrollable transcript (LVGL span group) for coloured command output (read-only). It keeps
  a multi-screenful history and jumps to the output of the command you just submitted.
  Scrolling: drag on the transcript, the input-row `Up`/`Dn` buttons, USB keyboard
  `PageUp`/`PageDown` (one viewport per press), or the USB mouse wheel
  (`P4_CONFIG_TRANSCRIPT_SCROLL_STEP` pixels per notch).
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
| `reboot`, `clear`/`cls`, `prompt` | `components/command/command.c` |
| `date`, `time`, `timezone`, `sntp`/`ntpsync` | `components/clock/clock_commands.c` (dispatched from command.c) |
| `wifi`, `bluetooth`/`bt`, `usb`, `c6ota` (family routing) | `components/command/command.c` → owning module |
| `ping`, `dns`/`nslookup`, `httpget`/`wget` (dispatched here, implemented in networking) | `components/command/command.c` → `components/networking/` |

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

### ps | tasks | top [/b]

Read-only FreeRTOS task introspection. `ps` and `tasks` print a colour-coded
table; `top` prints the same table plus a summary line with the live task
count, free heap, and uptime. Each row shows:

- **Name** — FreeRTOS task name
- **Sta** — state (`RUN` running, `RDY` ready, `BLK` blocked, `SUS` suspended, `DEL` deleted)
- **Prio** — current priority
- **Core** — pinned core, or `-1` when the task has no core affinity (unpinned)
- **HeadB** — stack high-water mark: the minimum free stack bytes remaining
  since the task was created (lower = closer to a stack overflow)
- **CPU%** — the task's CPU share since the previous `ps`/`top`/`tasks` call,
  computed by diffing FreeRTOS run-time counters

`/b` emits uncoloured machine-parsable rows (`name state prio core headb cpu`)
suitable for redirection and pipes, e.g. `ps /b > tasks.txt`.

The command is read-only — it never suspends, deletes, or reprioritises tasks.

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

The on-screen symbols keyboard covers every printable ASCII character
(0x20-0x7E), including the shell-critical pipe `|`, caret `^` (the shell
escape character), tilde `~`, and backtick, so DOS operators and escaped
characters can be typed directly. Use the `1#` / `abc` mode buttons to switch
between text and symbols.

### battery
Read battery ADC pin (GPIO53, 2:1 divider), show scaled voltage, estimated percentage (3.3V-4.2V range), raw ADC data, and light-sleep state.

### battery sleep <on|off|status>
Request or inspect light sleep. Only available when CONFIG_PM_ENABLE is enabled in sdkconfig.

### volume <0-100>
Set speaker volume through ES8311 codec path.

### date [MM-DD-YYYY]
Show the full clock panel or set the system date. With no argument, prints the
local time, UTC time, Unix timestamp, timezone, uptime, and NTP sync status.
`date 08-11-2026` (also accepts `/` separators) sets the date; the C library
clock is updated and a later SNTP sync overrides it. Implemented in
`components/clock/clock_commands.c`.

### time [HH:MM[:SS]]
Show the full clock panel or set the system time. With no argument, prints the
same panel as `date`. `time 14:30:00` (or `14:30`) sets the time. Implemented
in `components/clock/clock_commands.c`.

### timezone [TZ]
Show the current POSIX timezone string, or set a new one. `timezone` prints the
active TZ and the local time; `timezone UTC` restores UTC, and `timezone
CET-1CEST,M3.5.0,M10.5.0/3` selects Central European Time with DST rules. The
local date/time re-renders immediately. Implemented in
`components/clock/clock_commands.c`.

### sntp / ntpsync [sync]
Show the NTP synchronization state or force a fresh exchange. `sntp` prints the
configured server, whether the clock is synced, and the local time. `sntp sync`
(alias `ntpsync sync`) restarts the SNTP client against
`P4_CONFIG_NTP_SERVER` (default `pool.ntp.org`); once Wi-Fi is connected the
clock jumps to the network time. Implemented in
`components/clock/clock_commands.c`.

### gpio list
Show exposed board GPIO table with pin numbers, current levels, write policy, and role descriptions.

### gpio status
Show current levels and role text for all exposed board pins.

### gpio read <pin>
Read current logic level from any GPIO number.

### gpio set <pin> <0|1>
Drive a GPIO output. Only allowed for pins marked safe for writes in the shell pin table.

### screenshot [filename.bmp] (aliases: scr, capture)
Capture the current LVGL screen as a BMP image. Without a filename, streams the
BMP over the UART/USB-Serial-JTAG console with magic markers for host-side
extraction. With a filename, saves to the SD card with free-space precheck and
partial-destination cleanup on failure.

BMP format: 24-bit RGB888, bottom-up, no compression, 96 DPI. Pixel-perfect
capture of the entire screen (1024x600 = 1,228,800 bytes of pixels + 54-byte header).

**Usage:**
```
screenshot                  Stream BMP to serial with magic markers
screenshot shot.bmp         Save BMP to SD card (current directory)
scr shot.bmp                Alias form
capture shot.bmp            Alias form
```

**Serial streaming:** The BMP is preceded by `=== SCREENSHOT BMP BEGIN ===` and
followed by `=== SCREENSHOT BMP END ===`. Hex-encoded rows are printed for easy
extraction by host tools or a simple Python script.

**SD card save:** Uses the same storage path as `copy`, `write`, etc. Free-space
is prechecked, the session is guarded, and a partial destination is removed on
write failure.

**ERRORLEVEL:** 0 on success, 1 on failure (snapshot error, PSRAM exhaustion, SD
write error, invalid path), 2 on usage error (too many arguments).

**Batch file example:**
```batch
screenshot shot.bmp
if errorlevel 1 echo Screenshot failed
```

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
| call <file.bat> [args] | Execute batch file; `%0` is the script name, `%1..%9`/`%*` are the forwarded arguments, and the caller's errorlevel becomes the script's final errorlevel |
| if [not] errorlevel N cmd | Run cmd when errorlevel is at least N |
| if [not] exist <file> cmd | Run cmd when the file or directory exists |
| if [/i] [not] "a"=="b" cmd | Run cmd when the strings match; `/i` makes the comparison case-insensitive |
| if [not] a EQU\|NEQ\|LSS\|LEQ\|GTR\|GEQ b cmd | Run cmd on a numeric comparison of `a` and `b` (parsed as decimal; non-numeric reads as 0) |
| goto <label> | Jump to a `:label` in the running batch file |
| goto :eof | Jump to the end of the current batch file, unwinding its open setlocal scopes |
| shift | Shift batch arguments left by one position |
| pause | Wait for a keypress |
| choice [/C:keys] [/N] [/T:c,secs] [/S] [text] | Wait for one of the listed keys |
| setlocal | Push a copy of the environment; later changes are local |
| endlocal | Pop the most recent setlocal scope |
| exit [code] | Leave every nested batch file, setting errorlevel |
| exit /b [code] | Leave only the current batch file |
| alias | List every alias |
| alias name | Show one alias |
| alias name=value | Define (or update) an alias; `name=` clears it |
| alias /clear | Clear every alias |
| alias /save [file] | Write the alias table to the SD profile (default `ALIASES.BAT`) |
| alias /load [file] | Reload the alias profile from SD |
| unalias name | Remove one alias |

### alias / unalias — DOSKEY-style macros

Typing an alias at the prompt expands its leading word to the stored value
before the line is parsed, exactly like DOSKEY macros:

```
alias ll=dir /s
ll            ->  dir /s
ll *.txt      ->  dir /s *.txt
alias ls="dir /b"
ls            ->  dir /b
```

- Alias names are case-insensitive (stored uppercase), alphanumeric plus
  `_`, up to `P4_CONFIG_ALIAS_NAME_BYTES`; values up to
  `P4_CONFIG_ALIAS_VALUE_BYTES`; at most `P4_CONFIG_ALIAS_MAX` aliases.
- Expansion happens only at the interactive prompt — **never inside a batch
  file** — so an alias cannot shadow a batch verb (`set`, `call`, `echo`, ...).
- `alias /save` persists the table as `alias name="value"` lines to
  `sd:/ALIASES.BAT` (a batch file). boot.c auto-runs that profile after
  CONFIG.SYS, so saved aliases are restored every boot without editing
  AUTOEXEC.BAT. `alias /load` reloads manually. Values containing a double
  quote are skipped on save so the profile always round-trips.

### set /a — integer arithmetic

Evaluates a 32-bit signed integer expression. With an assignment the result is
stored; without one it is printed. Comparison and logical operators yield `1`
when true and `0` when false, so a boolean can be stored and tested later.

| Precedence | Operators |
|------------|-----------|
| Lowest | `\|\|` logical or |
| | `&&` logical and |
| | `==` `!=` `<` `>` `<=` `>=` comparison (1/0) |
| | `\|` bitwise or |
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
set /a mask=0xF0 << 4       mask=3840
set /a 100/7                Prints 14 without storing anything
set /a x=5==5               x=1   (comparison result)
set /a x=5!=5               x=0
set /a ok=(5>3)&&(2<4)      ok=1  (logical and)
set /a (1||0)               Prints 1
set /a x=17%%5              x=2   (%% is a literal % in a batch file)
```

**Quoting operators that the shell also uses.** `&&`, `||`, `&`, `|`, `<`,
`>` and `<<` / `>>` are command-chain, pipe, or redirection operators at the
shell level, so an expression containing them must be quoted (or the character
escaped) or the line is split before `set /a` runs:

```
set /a "x=5<6"              x=1
set /a "x=(%total%==14)&&(%total%>10)"
set /a "x=6&3"              x=2   (bitwise and)
set /a "x=5^3"              x=6   (^ is the escape character; ^^ inside
                                   quotes is a literal ^)
```

`==` and `!=` contain no shell operator, so they work unquoted (`set /a x=5==5`).
An expression with no assignment (for example `set /a 5==3`) is evaluated and
the result is printed. `set /a x==5` therefore prints the value of the
comparison `x==5` rather than assigning it; write `set /a x=(x==5)` to store
it. Both operands of `&&`/`||` are always evaluated (no short-circuiting),
matching cmd.exe, so `set /a "(0&&1/0)"` reports a divide-by-zero error.

Divide by zero, unbalanced parentheses, and malformed input are reported and
set errorlevel to 1 rather than producing a wrong answer.

### if — numeric comparison keywords

`if` accepts the cmd.exe-style numeric keywords in the form
`if [not] [/i] <operand1> <OP> <operand2> <command>`. The operands are parsed
as decimal integers (a non-numeric operand reads as 0), so the result of a
`set /a` computation can be branched on directly:

| Keyword | Meaning |
|---------|---------|
| `EQU` | equal to |
| `NEQ` | not equal to |
| `LSS` | less than |
| `LEQ` | less than or equal to |
| `GTR` | greater than |
| `GEQ` | greater than or equal to |

```
set /a n=42
if %n% EQU 42 echo n_is_42
if %n% GTR 10 echo n_is_large
if 5 LSS 3 echo never
if not 2 GTR 1 echo never
if abc EQU 0 echo non_numeric_reads_as_zero
```

`/i` is ignored for the numeric form (numbers have no case). The keyword form
is separate from the `==` string comparison, so `if a==b` stays a string test
while `if a EQU b` compares numerically.

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
- %0 (script name), %1 through %9, and %* (all arguments, from %1 onward) expansion
- %VAR% environment variable expansion
- rem and :: comment lines
- @ line prefix to suppress echo for one line
- echo on/off flow control
- `:label` targets for `goto` and `call :label`, including the implicit `:eof` end-of-file label
- `for %%var in (set) do command` loops over literal tokens or a single wildcard pattern (for example `for %%F in (*.txt) do echo %%F`)
- PATH-based .bat lookup
- Nested calls up to 4 levels deep; `call` forwards arguments and propagates the callee's errorlevel
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
| format [/FS:FAT\|FAT32] [/A:size] [/V:label] [/Q] | Reformat the SD card, destroying all data |
| disk list / detail / clean | Physical-disk info, MBR partition table, remove partitions |
| disk create partition primary [size=N] | Create a primary MBR partition (N in MB) |
| disk delete partition N | Delete MBR partition N (1-4) |
| disk format [fs=...] [label=...] [au=...] [quick] | diskpart-style format of the volume |

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
| /FS:type | Filesystem type: `FAT` or `FAT32`. Both use the standard ESP-IDF
  helper's size-appropriate selection (FAT12/16 for small volumes, FAT32 for
  modern SD cards). `EXFAT` is not available in this firmware build; requesting
  it warns and formats as FAT32 instead. |
| /A:size | Allocation unit (cluster) size in bytes, with an optional K/M suffix
  (e.g. `/A:32K`). Range `P4_CONFIG_FORMAT_ALLOC_UNIT_MIN`..`_MAX`. |
| /V:label | Volume label to apply afterwards, 11 characters or fewer |
| /Q | Accepted for DOS familiarity; the underlying operation is always quick |

The command prints a warning and requires the exact word `YES` typed at the
prompt before anything is written. The confirmation is read one key at a time
through the shell's key queue, so the reply never reaches the command
dispatcher. (Over serial each key must be sent on its own line; the on-screen
keyboard types it naturally.)

If no interactive input source is attached (no UART console and no USB
keyboard) the command **refuses outright** rather than proceeding, so a batch
file can never silently wipe a card.

After formatting, the FAT type, label, capacity and allocation unit size are
reported. The card must be initialized (mounted, or left initialized by a prior
`disk` command in the same session); a card the BSP cannot mount after a reboot
must be recovered on a host machine or with `BOARD_CFG_SD_FORMAT_ON_MOUNT_FAIL`.

```
format
format /FS:FAT32 /V:DATA
format /FS:FAT32 /A:64K /V:ROOT
```

### disk

diskpart-style physical-disk and partition management for the SD card.

| Subcommand | Meaning |
|------------|---------|
| disk list | Show the physical disk(s): card name, capacity, sectors, sector size |
| disk detail | Show disk geometry and the MBR partition table (boot flag, type, start, size) |
| disk clean | Remove all MBR partitions (destructive, requires `YES`) |
| disk create partition primary [size=N] | Create a primary FAT32 partition aligned to 1 MiB; `size` in MB (default: rest of card) |
| disk delete partition N | Delete MBR partition N (1-4), destructive, requires `YES` |
| disk format [fs=FAT32] [label=X] [au=size] [quick] | diskpart-style format of the volume |

After `disk clean` or `disk create partition primary`, run `format` (or
`disk format`) to create the filesystem. `disk clean`/`delete`/`format` require
the exact `YES` confirmation word like `format`.

```
disk list
disk detail
disk clean
disk create partition primary
disk create partition primary size=2048
disk delete partition 1
disk format fs=fat32 label=DATA au=32K quick
```

## Boot Scripting

At every boot the firmware looks for CONFIG.SYS and AUTOEXEC.BAT on the SD
card root (components/boot). Missing files are generated once from built-in
templates when P4_CONFIG_BOOT_GENERATE_DEFAULTS is set; with no SD card the
whole sequence is a silent no-op, identical to the previous boot behaviour.

CONFIG.SYS is parsed line-by-line. Blank lines and REM/; comments are
skipped; keywords are case-insensitive. Directives:

| Directive | Effect |
|-----------|--------|
| SET name=value | Set a batch environment variable |
| PATH=dir1;dir2;... | Set the command search PATH |
| PROMPT=template | Set the DOS prompt template |
| ECHO ON\|OFF | Set the batch echo default |
| ROTATE=0\|90\|180\|270 | Set display rotation |
| BRIGHTNESS=0-100 | Set backlight brightness |
| DISPLAY_POWER=ON\|OFF\|SLEEP | Set display power state |
| VOLUME=0-100 | Set speaker volume |
| WIFI_SSID=... WIFI_PASSWORD=... | Store auto-connect target (password never echoed) |
| WIFI=ON\|OFF WIFI_AUTOCONNECT=ON\|OFF | Wi-Fi runtime policy |
| BLUETOOTH=ON\|OFF BT_ADVERTISE=ON\|OFF | Hosted BLE policy |
| USB_KEYBOARD=ON\|OFF USB_MOUSE=ON\|OFF | USB HID policy |
| GPIO <n> = OUT [HIGH\|LOW] | Initial level at a safe output pin (reserved pins refused) |

Unknown directives print a single muted warning and are skipped. AUTOEXEC.BAT
then runs through the normal batch pipeline with full batch power
(if/or/goto/call, pipes, redirection, chaining).

All limits and filenames are configurable in p4minishell_config.h
(P4_CONFIG_BOOT_*) and documented in p4minishell_config.yaml under
oot_scripting.

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
| find [path] [/NAME:pat] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B] | Recursively list files by name / size / date |
| more [file] | Page a text file, waiting for a key between pages |
| tree [path] [/F] [/A] | Draw a recursive directory outline |
| fc <file1> <file2> | Compare two text files line by line |
| sort [file] [/R] [/I] [/U] | Print a file with its lines sorted |

### find

`find` has two modes. The presence of any discovery switch selects file
discovery; with the classic switches only it is the original text search.

**Text search:** `find <text> [file]`

| Switch | Meaning |
|--------|---------|
| /I | Case-insensitive match |
| /N | Prefix each match with its line number |
| /C | Print only the match count |
| /V | Print the lines that do NOT match |

With no file argument, `find` reads the pending `<` or pipe input source.

Example: `find "timeout" boot.log /I /N`

**File discovery:** `find [path] [/NAME:pattern] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B]`

Recursively walks the starting directory (default: current directory) and
prints entries that pass every supplied filter.

| Switch | Meaning |
|--------|---------|
| /NAME:pattern | Filename wildcard (e.g. `*.log`, `*config*`). A wildcard in the path argument also splits into directory + pattern, like `dir`. |
| /SIZE:spec | Byte size filter with an optional K/M/G suffix. Use the redirection-safe range syntax `N-M` (range), `N-` (at least), `-M` (at most), or `N` (exact). The comparison forms `>N` / `>=N` / `<N` / `<=N` are also accepted when quoted (bare `>`/`<` are the shell's input/output operators). |
| /NEWER:date | Only entries modified on/after `YYYY-MM-DD` |
| /OLDER:date | Only entries modified on/before `YYYY-MM-DD` |
| /DIRS | Include directories as well as files |
| /B | Bare: full paths only, no colour — redirectable / pipable |

Examples:
```
find /NAME:*.log
find /sdcard /NAME:*config* /SIZE:1K-64K
find /logs /NAME:*.txt /NEWER:2026-01-01 /B
find /DIRS /B
```
Recursion is depth-bounded by `P4_CONFIG_DIR_RECURSE_DEPTH_MAX` and matches are
capped by `P4_CONFIG_FIND_MATCH_MAX` (256), with a clear truncation note.

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
| wifi status | Multi-line colour-coded report: state, target SSID, SSID, BSSID, channel, RSSI, PHY mode, IPv4, netmask, gateway, DNS, association uptime |
| wifi scan | Scan for nearby access points, sorted by RSSI (strongest first), column-aligned, capped at `P4_CONFIG_WIFI_SCAN_LIMIT` |
| wifi scan /b | Bare scan: one SSID per line, no colour — redirectable, machine-parsable |
| wifi diag | Run full diagnostic: connection state, IP status, network scan |
| wifi connect | Connect using sdkconfig default credentials |
| wifi connect <ssid> <pass> | Connect with runtime credentials (password masked) |
| wifi disconnect | Disconnect current station session |
| wifi known | Show saved networks (SSIDs only — never passwords) with preferred / priority / last-used markers |
| wifi save [ssid] | Save the connected (or given) network to the SD known-list |
| wifi forget <ssid> / wifi delete <ssid> | Remove one saved network |
| wifi forget all / wifi clear known | Clear the entire known-list |
| wifi preferred <ssid> | Mark a saved network as preferred for auto-connect |

### Wi-Fi Behavior
- Boot-time startup in background task (does not block shell UI)
- ESP-Hosted version compatibility gate: refuses init if C6 firmware != host 2.12.x
- Recovery guidance points to coprocessor/esp32c6_slave or c6ota default
- Restores automatically after successful c6ota
- Transcript-facing diagnostics on boot and post-OTA restore

### Persistent Known Wi-Fi Networks
- The known-list is a plain, hand-editable file at `sd:/WIFI.KNOWN` (see
  `P4_CONFIG_WIFI_KNOWN_FILE`), one network per line:
  `SSID|PASSWORD|AUTH|PRIORITY|PREFERRED|LAST_CONNECTED|CONNECT_COUNT`.
- On boot with `WIFI_AUTOCONNECT=ON`, the firmware scans and connects to the best
  visible known network (preferred / highest priority / strongest RSSI), then
  falls back to the classic single-credential path when the list is empty or no
  known network is in range. `WIFI_AUTOCONNECT` remains the master switch.
- Successful connections (`wifi connect`, CONFIG.SYS `WIFI_SSID=`/`WIFI_PASSWORD=`,
  or auto-connect) update the list automatically when the SD card is present.
- When the SD card is absent, ejected, read-only, full, or the file is missing or
  corrupt, every known-list command reports a clear message and sets
  ERRORLEVEL 1; boot and normal operation continue unchanged. Passwords are
  stored in the file but never echoed to the transcript, history, or debug log.

## Connectivity Commands

### ping <host-or-ip> [count]

Classic ICMP echo. Sends `count` echo requests (default 4, hard maximum 10),
prints each reply line and a DOS-style statistics summary, then sets
ERRORLEVEL so batch files can branch:

- **ERRORLEVEL 0** — at least one reply was received (success)
- **ERRORLEVEL 1** — every request timed out, the host did not resolve, or
  Wi-Fi is not started / not connected
- **ERRORLEVEL 2** — usage error (missing or malformed arguments)

```
ping 8.8.8.8
ping 192.168.1.1 5
ping example.com
```

Example output:

```
Reply from 192.168.1.1: bytes=32 time=2ms TTL=64

--- 192.168.1.1 ping statistics ---
    Packets: Sent = 4, Received = 4, Lost = 0 (0% loss),
Approximate round trip times in milli-seconds:
    Minimum = 2ms, Maximum = 4ms, Average = 3ms
```

The command participates fully in the pipeline:

- **Redirection**: `ping 8.8.8.8 > ping.txt` writes the plain-text report to
  an SD file (ANSI colour is stripped for redirected output).
- **Pipes**: `ping 192.168.1.1 | find "Reply"` feeds the reply lines to the
  next stage.
- **Chaining**: `ping 8.8.8.8 && echo network_up`, `ping 10.0.0.1 || echo down`.
- **Batch files / AUTOEXEC.BAT**: `if errorlevel 1 goto nolink` works exactly
  as in DOS; the session runs on its own task and the worker task blocks only
  for a bounded total (`count` × (timeout + interval) + margin), so it never
  hangs.

A count above 10 is clamped with a warning; timings and packet loss are
reported in classic `Sent/Received/Lost` + `Minimum/Maximum/Average` form.

### dns <hostname> (alias nslookup)

Resolves the A records of a hostname through lwIP DNS and prints the IPv4
address list.

```
dns example.com
nslookup example.com
```

- **ERRORLEVEL 0** — resolved at least one A record
- **ERRORLEVEL 1** — could not resolve, or Wi-Fi is not started
- **ERRORLEVEL 2** — usage error

Redirectable and piped like any other command: `dns example.com > dns.txt`.
With this build's lwIP DNS cache (`CONFIG_LWIP_DNS_MAX_HOST_IP=1`) a name
normally resolves to a single A record.

### httpget <url> [localfile] (alias wget)

Performs a simple HTTPS (or HTTP) GET over the same `esp_http_client` stack
used by `c6ota`. No headers to set, no methods beyond GET — this is the
deliberately minimal diagnostic/scraping tool.

- `httpget https://example.com` — prints the response header (HTTP status,
  content-type, size) and then the response body to the transcript. The body
  print is bounded (`P4_CONFIG_HTTP_PRINT_BODY_BYTES`, 4 KiB) and sanitized so
  a binary payload cannot corrupt the transcript.
- `httpget https://example.com/page.html` — saves the **exact** body to the
  current working directory / SD card through the storage write path: cwd-
  relative resolution, guarded SD session, free-space precheck (an overwrite
  reclaims the old file's space), and partial-destination cleanup if the write
  fails. Prints `httpget: saved N bytes to <path>` on success.

```
httpget https://example.com
httpget http://192.168.1.1/status > status.txt
httpget https://example.com/page.html downloaded.html
```

**ERRORLEVEL** (works in batch files and AUTOEXEC.BAT):

- **0** — an HTTP 2xx was received and (with a localfile) the body saved.
- **1** — any failure: non-2xx status, connection refused / TLS failure,
  unreachable host or timeout, body over the size cap, SD write failure, or
  Wi-Fi not connected.
- **2** — usage error (missing URL or unsupported URL scheme).

Examples in a batch file:

```
httpget https://example.com/api/v1/status > srv.json
if errorlevel 1 echo server_down
httpget https://example.com/page.html page.html && echo downloaded
```

**Redirection and pipes.** Output goes through the transcript appenders, so
`httpget url > file` captures the printed header + body and
`httpget url | find "200"` pipes it. For the complete untruncated body, use
the localfile form rather than `>` redirection.

**Config.** `P4_CONFIG_HTTP_TIMEOUT_MS` (default 15 s), 
`P4_CONFIG_HTTP_MAX_BODY_BYTES` (512 KiB PSRAM cap; larger responses are
refused with a clear error), `P4_CONFIG_HTTP_FOLLOW_REDIRECTS` (1 = follow up
to 3 redirects), and `P4_CONFIG_HTTP_USER_AGENT`. All HTTP/TLS code lives in
`components/networking/`; no socket, mbedTLS, or esp_http_client call exists
outside it. Requires an active Wi-Fi connection (`wifi connect` first); with
no connection it prints a clear DOS-style error and sets a non-zero
errorlevel. Timeouts are bounded so the worker task is never hung.

## Bluetooth Commands

| Command | Description |
|---------|-------------|
| bluetooth status | Colour-coded report: hosted readiness, controller, NimBLE, sync, scan, advertising (with active name), C6 firmware version, last error |
| bluetooth scan [limit] | Bounded BLE scan (default 8 s) through hosted NimBLE on C6; prints name + address + RSSI sorted strongest-first, up to `limit` (default 8) |
| bluetooth advertise on [name] | Start non-connectable BLE advertising. `name` is session-only (RAM-only, never persisted); without it the configured default name is used |
| bluetooth advertise off | Stop BLE advertising |
| bt ... | Alias for bluetooth command family |

### Bluetooth Lifecycle
- bluetooth enable initializes hosted controller and NimBLE host once
- Subsequent scan/advertise commands reuse active session
- Hosted NimBLE VHCI on ESP32-C6 over ESP-Hosted SDIO
- `bluetooth scan` is always bounded by `P4_CONFIG_BT_SCAN_DURATION_MS`, so it
  always terminates and the worker task never hangs

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
