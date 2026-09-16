# Writing a P4MiniShell Batch App

In P4MiniShell, **a `.bat` file is an app**. It runs from the SD card on the
same shell pipeline as the built-in commands, so it gets variables, control
flow, pipes, redirection, modals, and persistent state for free. This tutorial
takes you from a one-line script to an installable package.

For the exhaustive language reference see the batch sections of
[`command.md`](command.md).

---

## 1. Hello, app

Create `APPS/HELLO.BAT` on the SD card (or use `edit HELLO.BAT`):

```bat
@echo off
echo Hello from P4MiniShell!
echo Today is %DATE% at %TIME%.
```

Run it by typing its name at the prompt (`HELLO`) or `HELLO.BAT`. Put it
anywhere on `PATH`, or under `sd:/APPS`, and it also appears in `launch`.

Batch files are plain text (UTF-8) with CRLF or LF line endings. `rem` and `::`
are comments. `@echo off` suppresses command echo.

## 2. Arguments and variables

```bat
@echo off
if "%~1"=="" (
  echo usage: GREET name
  exit /b 2
)
set NAME=%~1
echo Hello, %NAME%!
```

- `%0` is the script name, `%1`..`%9` are arguments, `%*` is all of them.
- `%~1` strips surrounding quotes; `set NAME=value` sets an environment
  variable and `%NAME%` expands it.
- Unset variables expand to the empty string, so `if "%X%"==""` works.
- Dynamic values: `%DATE%`, `%TIME%`, `%RANDOM%` (0..32767), `%CD%`,
  `%ERRORLEVEL%`.

## 3. Control flow

```bat
@echo off
set /a count=0
:loop
set /a count+=1
echo step %count%
if %count% LSS 3 goto loop

if exist NOTES.TXT echo NOTES.TXT is here
if errorlevel 1 echo the previous command failed
```

- `if [not] [/i] ...` supports `errorlevel N`, `exist <path>`, `defined VAR`,
  string tests, and numeric keywords (`EQU NEQ LSS LEQ GTR GEQ`).
- `for %%v in (a b c) do echo %%v` and wildcard forms
  `for %%f in (*.txt) do echo %%f`.
- `for /f "eol=; skip=1 delims=, tokens=1,2" %%a in (data.csv) do echo %%a=%%b`
  iterates file lines.
- `goto :eof` ends the current file; `call :label` calls a local label;
  `call LIB.BAT::routine args` calls a shared-library routine with automatic
  variable isolation (`exit /b` returns).
- `setlocal` / `endlocal` scope variables; every scope is unwound when the
  frame returns.
- `set /a` does integer arithmetic; `calc` does floating point (see below).

## 4. Pipes, redirection, chaining

```bat
type LOG.TXT | findstr ERROR > errors.txt
echo done & echo next
dothething && echo ok || echo failed
```

`|` chains stages (up to 4), `<` feeds input, `>`/`>>` redirect output and
append, and `&` / `&&` / `||` chain commands. All of it works inside a batch
file.

## 5. Prompting the user

Use the native modal surfaces for polished interaction (touch or serial):

```bat
dialog "Confirm" "Delete the file?" Yes No
if errorlevel 1 echo cancelled

list "Pick a colour" Red Green Blue /t:15
if errorlevel 255 echo timed out
echo you picked index %errorlevel%

ask "Your name?" /v:WHO
echo hello %WHO%
```

- `dialog "title" "message" [btn...]` returns the button index (or 255).
- `list "title" item...` returns the 0-based selected index (or 255).
- `ask "prompt" [default]` stores the answer in `ASK_RESULT` or `/v:NAME`
  (`/p` masks it, `/t:secs` adds a timeout).
- `browse [path]` picks a file (`BROWSE_RESULT`).
- Simple text prompts: `set /p NAME=<prompt>` and `set /p NAME=< file` to read
  a line from a file or pipe.

Remember: a `list` serial selection is **1-based** (the number you type) while
the returned ERRORLEVEL is the **0-based index**.

## 6. Persistent state and data

Everything lives on the SD card:

```bat
appconfig MYAPP            per-app settings file (sd:/APPS/MYAPP.INI)
ini set MYAPP.CFG theme=amber
ini get MYAPP.CFG theme
temp new scratch.txt       a temp file under sd:/tmp
db create NOTES            a record store
db add NOTES "title=Todo;body=Buy milk"
export db NOTES json backup.json
```

- `ini` reads/writes `KEY=VALUE` files; `appconfig <app>` gives each app its own
  `sd:/APPS/<APP>.INI`; `temp` manages temporary files.
- `db` is a Palm-OS-style record store with categories, secret fields, and
  `/field:` queries. See [`command.md`](command.md) for the full `db` family.
- `crypt` encrypts files (AES-256-GCM); `archive` makes USTAR `.p4a` backups.

## 7. Full-screen TUIs and graphics

For anything beyond line output, draw with the TUI or GFX layers (both are
available from batch):

```bat
draw box 2 2 30 8 single "Status"
draw text 4 4 "Booting..."
color 14
tui fullscreen on
```

`draw` provides boxes, lines, text, bars, tables, scrollable lists, cursor
control, and hold/refresh. `gfx` provides a pixel canvas with sprites and BMP
images for games (`apps/gfxdemo/BOUNCE.BAT`). `plot` renders graphs and charts.
See the "TUI / graphics" sections of [`command.md`](command.md) and the
reference apps under `apps/`.

For long-running work, `start <command>` runs a background job and
`taskkill <job>` stops it.

## 8. Package it

To make an app installable with `pkg`:

1. Add an `APPINFO` file (`sd:/APPS/<APP>.APPINFO`, INI format):

   ```ini
   title=My App
   description=What it does
   version=1.0
   ```

2. Build a bundle `PKGS/<APP>/` containing the app's files, its `.APPINFO`, and
   a `<APP>.ASSETS` manifest (`path=HEXCRC`, generated by
   `apps/push_pkgs.py`).

3. Install on device:

   ```
   pkg list
   pkg info MYAPP
   pkg install MYAPP
   ```

The reference apps under `apps/` (especially `apps/companion/`) show every
technique in this tutorial in a complete, pure-batch program.

## 9. Debugging tips

- `echo on` shows each command as it runs (or `@echo off` to silence).
- `help /all`, `command.md`, and the `proc` command (`proc /args /depth
  /errorlevel`) help inspect state.
- `debug` shows the last error/warning messages.
- Set the ERRORLEVEL explicitly with `exit /b N` so callers can branch.
- Watch the transcript: colours and labels make failures obvious.
- The Companion's `LIB.BAT` uses `call ::routine` self-tests
  (`SVC.BAT`/`AGENDA.BAT` show background jobs).

## 10. Limits worth knowing

| Limit | Value |
|-------|-------|
| Batch nesting depth | 4 |
| Labels per file | 32 |
| Batch argument width / command line | 4096 bytes (`P4_CONFIG_COMMAND_BYTES`) |
| Environment variables | 24 |
| Pipe stages | 4 |
| Chained commands | 8 |
| `setlocal` scopes | 8 |
| RAM-resident batch file | 128 KB (larger files stream from SD) |

The full list is in the "Shell State" section of
[`ai-context.md`](ai-context.md) and in `p4minishell_config.yaml`.
