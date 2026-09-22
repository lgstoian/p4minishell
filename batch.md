# P4MiniShell Batch App Specification

> **Read this file and you can write batch apps.** It is the single complete
> reference for the `.bat` language that runs on the P4MiniShell firmware
> (ESP32-P4 + ESP32-C6, DOS-shaped shell). For the full shell command surface
> see [`command.md`](command.md); for the native C SDK see [`SDK.md`](SDK.md);
> for architecture see [`documentation.md`](documentation.md); for the
> step-by-step tutorial see [`tutorial_batch.md`](tutorial_batch.md).

- **Version:** v1.2.1 · **Line endings:** CRLF or LF · **Encoding:** UTF-8
  (FATFS long filenames are UTF-8) · **Size:** ≤128 KB runs RAM-resident
  (`P4_CONFIG_BATCH_FILE_MAX_BYTES`); larger files stream from SD with
  identical semantics.

---

## 1. What a batch app is

A batch app is a plain-text `.bat` file on the SD card. No compilation step.
It runs as a command stream on the single command-worker task (no process
isolation): it shares the transcript, the 64-slot RAM environment, the cwd,
and ERRORLEVEL with the whole shell (see §3).

**Discovery.** A file becomes an app by living on `PATH` (default `sd:/`) or
under `sd:/APPS`, with an optional `sd:/APPS/<NAME>.APPINFO` metadata file.
`launch` lists discovered apps (never shadowing a built-in); `launch <name>`
runs one. The boot `LAUNCH_APP=` CONFIG.SYS directive can auto-launch after
`AUTOEXEC.BAT`.

**Minimal app (`HELLO.BAT`):**

```bat
@echo off
echo Hello, %1!
exit /b 0
```

Run it: `hello world`, or `launch hello`.

**Hybrid apps (a shim over a C entry).** A batch file is also the launch
contract for apps whose heavy lifting is in C. The shim declares itself with
`type=hybrid` in `APPS/<APP>.APPINFO` and `entry=<name>`, then calls the
linked-in entry by name as if it were a command — the shell dispatches it,
its stdout is the transcript, and its return value is ERRORLEVEL:

```bat
@echo off
rem FRONT.BAT - hybrid shim driving the linked `hello` entry
set MYAPP_MODE=full
hello %*
if errorlevel 1 echo [M-MYAPP] failed
exit /b %ERRORLEVEL%
```

That is the whole mechanism: no new runtime, no `dlopen`. `launch /list` tags
the shim ` [hybrid]`. The frozen contract and the C side live in
[`ABI.md`](ABI.md) and [`tutorial_native.md`](tutorial_native.md).

---

## 2. Lines, echo, comments, continuation

- `@` as the first character suppresses per-line echo for that line;
  `@echo off` silences the file (restored per frame).
- `rem <text>` and `::` are comments, **opaque to end of line**: `rem a | b`
  never pipes, `rem x > f` never writes. (The comment gate runs before the
  chain/pipe/redirect parsers.)
- A trailing `^` joins the next physical line (bounded by
  `P4_CONFIG_LINE_CONTINUATION_MAX` = 8). An escaped `^^` is a literal caret,
  not a continuation.
- Nesting is capped at 4 levels (`P4_CONFIG_BATCH_DEPTH_MAX`); every nested
  line re-enters the full pipeline (expansion + redirection).

---

## 3. Batch process model (the contract)

| Concern | Definition |
|---------|------------|
| **stdout** | Every command's transcript output; captured by `>` / `>>`. A batch line inherits the caller's redirect. |
| **stderr** | Not a separate stream. Errors interleave on the transcript; failure is signalled by ERRORLEVEL. |
| **stdin** | The storage input-redirection slot: a `< file`, a pipe stage, or an explicit filename. Consumed by text tools (`sort`, `more`, `find`, …), by `for /f` over an empty set, and by `set /p NAME=< file` (one line). The interactive key queue backs `set /p` / `pause` / `choice` when no redirect is active. |
| **argv** | `%0` = script name, `%1`..`%9` = caller arguments, `%*` = everything from `%1`. `call` / `call :label` / `gosub` push a fresh frame; `shift` slides it. |
| **cwd** | RAM-only current directory (storage-owned). Relative paths resolve at run time. |
| **PATH** | RAM-only `PATH` slot (default `sd:/`). Resolution tries the literal name, `name.bat`, then each `;`-separated PATH entry with both forms. |
| **environment** | 64 RAM slots (`P4_CONFIG_ENV_VAR_MAX`), session-global. `set` / `set /a` / `set /p` / `calc` mutate it; `call <file>::<routine>` isolates it; `setlocal` / `endlocal` snapshot/restore it (a scope left open is unwound on frame return). |
| **errorlevel** | Integer exit code. Read with `if errorlevel N` (true when ≥ N) or `%ERRORLEVEL%`; set by commands, `choice` (1-based key index), `exit /b N`, `return N`. |

---

## 4. Quoting, chaining, pipes, redirection

One scanner backs all five surfaces, so they never disagree. Rules:

- `"text"` groups **with** `%VAR%` expansion; `'text'` groups **literally**
  (no expansion); `^c` escapes one character (`^&` `^|` `^>` `^"` `^^` `^%`).
- **Chains:** `&` always runs the next link, `&&` runs on success, `||` runs
  on failure. Splitting happens *before* expansion, so a variable whose value
  contains `&` cannot inject a command.
- **Pipes:** `cmd1 | cmd2 | ...` (up to `P4_CONFIG_PIPE_STAGE_MAX` stages).
  Each stage but the last spools to an SD temp file; the next stage reads it.
  A `.bat` file can sit mid-pipeline (stdin via `for /f ... in ()`,
  `set /p NAME=<`, or a text tool; stdout via `echo` / `>`).
- **Redirection:** `>` / `>>` capture stdout (transcript delta), `<` feeds
  stdin. Quote-aware; last occurrence of each direction wins (COMMAND.COM).
  Target paths resolve against the cwd.
- A batch file is a valid pipe stage and a valid redirect target/source.

---

## 5. Variables and expansion

Names normalize to upper case; allowed characters: letters, digits, `_`,
`[`, `]`. `set NAME=` (empty) clears the slot. `set` with no args lists the
table. `set NAME` prints one value. Undefined `%VAR%` expands to empty (so
`if "%var%"==""` works).

| Form | Meaning |
|------|---------|
| `%VAR%` | Environment value (empty when undefined). |
| `%0` `%1`..`%9` `%*` | Script name / caller args / all args from `%1`. |
| `%%` | Literal `%`. |
| `%ERRORLEVEL%` | Current errorlevel as decimal. |
| `%DATE%` `%TIME%` | `MM-DD-YYYY`, `HH:MM:SS` (device clock). |
| `%RANDOM%` | `0..32767`. |
| `%CD%` | Current directory. |
| `%~[f expert][d][p][n][x][s]N` | Argument modifiers: `f` full path, `d` drive (always empty on FATFS), `p` directory with trailing `/`, `n` base name, `x` extension, `s` accepted/ignored. |
| `%VAR:~start[,len]%` | Substring: `start` kept from 0; negative counts from the end; negative `len` drops that many from the end. `%S:~1,3%`, `%S:~-2%`. |
| `%VAR:old=new%` | Replace every (case-insensitive) `old` with `new`. `%P:l=L%`, `%P:o=%` deletes. |
| `!VAR!` | Delayed expansion: same named values and `:~`/`:` forms as `%VAR%`, but resolved at execution time. Only inside a `setlocal enabledelayedexpansion` scope; otherwise literal. `!!` is a literal `!`, `^!` never expands, `'...'` stays literal. |
| `^%` | Literal percent (never expands). |
| `'...'` | Single-quoted runs stay fully literal. |

**`%%` vs `%`:** batch files write `%%v` (the doubled percent is the file
escape); the interactive prompt writes `%v`. The `for` loop variable accepts
both. Inside a batch file, `%%n%%` double-expands one level per nesting pass
(the `call` idiom for indirect/dynamic names, see §6).

**Delayed expansion** (`setlocal enabledelayedexpansion`, restored by
`endlocal` like the variables themselves) makes `!VAR!` read the live table
at execution time instead of parse time — the loop-safe form:

```bat
setlocal enabledelayedexpansion
set /a n=0
while n LSS 3 do set /a n=n+1
echo !n! lives here too
endlocal
```

Without the scope, `!n!` stays literal (so existing `Hello!` text never
breaks). `enableextensions`/`disableextensions` are accepted and ignored
(extensions are always on) so portable scripts parse.

---

## 6. Arrays (indexed variables)

Brackets are ordinary name characters, so `NAME[index]` slots just work —
no declaration, no new storage, same 64-slot table:

```bat
set SPR[0]=10
set SPR[1]=20
echo %SPR[0]%+%SPR[1]%
set /a SPR[0]+=5
calc SPR[1]*2
```

- Indices are text: `SPR[0]` and `SPR[00]` are different slots. Keep one
  convention (decimal, no leading zeros). Keys may be words too
  (`CFG[theme]=amber`) — anything without spaces/`=`/`%`/`!`.
- Dynamic index: precompute with `for /L` (loop var substitutes textually),
  double-expand (`call echo %%SPR[%%i]%%`), or read live with `!SPR[%i%]!`
  inside a delayed scope.
- Iterate one prefix: `for /A %%k in (SPR) do echo %%k=%%l` binds the index
  to `%%k` and the live value to the next letter (`%%l`), in slot order.
  Use letters `a`–`y` so the value letter exists.
- Works in `set /a` (integer math), `calc` (float math, bare `SPR[1]`
  identifiers), `%VAR%` expansion, and the `%VAR:~%` / `%VAR:old=new%` forms.
- Cleanup: `set SPR[0]=` frees the slot. Arrays share the 64-slot budget
  with everything else — a 22-variable game plus a 10-element array fits;
  two such games nested do not.

---

## 7. `set /a` integer arithmetic

`set /a NAME=<expr>` evaluates over 32-bit signed integers and stores the
result (also prints `NAME=value`); `set /a <expr>` prints without storing.
Undefined variables read as 0, so `set /a n=n+1` works on first use.
Compound assignment (`+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`)
applies against the current value. Precedence follows cmd.exe:
`||`, `&&`, comparisons (`==` `!=` `<` `>` `<=` `>=` → 1/0), `|`, `^`, `&`,
`<<` `>>`, `+` `-`, `*` `/` `%`, unary `-` `~` `!`, parentheses. Literals
accept decimal, `0x` hex, and octal. An expression using `&` `|` `&&` `||`
`<` `>` must be quoted at the shell level (those split first).

---

## 8. `calc` floating-point and string calculator

`calc [NAME=] <expr>` evaluates over doubles/strings, stores into an
environment variable when `NAME=` is given, sets ERRORLEVEL (0 ok,
1 domain/syntax, 2 usage). Strings use `'`/`"` with `+` concatenation;
`&H`/`0x` hex literals; `PI` constant; seeded `RAN#`; undefined variables
read as 0.

- Math: `ABS SGN INT FIX FRAC ROUND SQR EXP LN LOG SIN COS TAN SINH COSH TANH
  ASN ASIN ACS ATN FACT NCR NPR DMS DEG` (+hyperbolic `ASINH ACOSH ATANH`),
  `MOD POL REC` (`POL`/`REC` also store into `X`/`Y`).
- String: `VAL VALF STR$ HEX$ BIN$ OCT$ VALB ASC CHR$ LEN LEFT$ MID$ RIGHT$
  UPPER$ LOWER$ TRIM$ INSTR REPLACE$` (`INSTR` returns a 1-based position or
  0; `INSTR(start,text,needle)` starts at a 1-based index like `MID$`).
- Base/unit: `BIN$ OCT$ VALB C2F F2C IN2MM MM2IN LB2KG KG2LB`.
- Financial (HP-12C signs): `PV FV PMT NPER RATE NPV IRR SLN SYD DB`
  (`calc /fin` cheatsheet); dates over epoch-day serials: `DATE YEAR MONTH DAY
  DOW TODAY DATEADD DAYS EOMONTH DATEVALUE DATESTR` (`calc /date`).
- Modes: `calc /deg|/rad|/angle` (trig angle mode), `calc /hex` (print
  integral result as `&H` hex).
- Sampling hook: `plot` evaluates `calc` expressions over the `X`/`T`
  environment variables (restored afterwards).

---

## 9. `if` conditions

```
if [not] [/i] errorlevel N <cmd>      (true when errorlevel >= N)
if [not] [/i] exist <path> <cmd>
if [not] [/i] defined <name> <cmd>
if [not] [/i] <a> EQU|NEQ|LSS|LEQ|GTR|GEQ <b> <cmd>   (decimal, non-numeric = 0)
if [not] [/i] <a>==<b> <cmd>          (string compare)
```

`/i` = case-insensitive string compare. `not` negates every form. There is no
`if/else` keyword — chain two opposite tests or `goto`.

---

## 10. Loops: `for`, `for /L`, `for /f`, `while`

All loop bodies re-enter the full pipeline per iteration (expansion +
redirection apply inside). `^C` / Stop unwinds every loop. `goto` inside a
body breaks the loop (a pending goto always clears on frame return, so it
cannot leak into the caller).

### 10.1 Classic and wildcard `for`

```
for %%v in (a b c) do <cmd>        :: literal token list
for %%F in (*.txt) do <cmd>        :: wildcard expanded via storage
```

The set is one pattern when it holds `*`/`?`, else a space-separated token
list. `%%v`/`%v` substitutes textually per iteration.

### 10.2 Numeric `for /L` (cmd.exe parity)

```
for /L %%i in (start,step,end) do <cmd>
```

Counts inclusively; a negative step counts down. The value binds as decimal
text (`n=%%i` works). Bounded by `P4_CONFIG_FORL_ITER_MAX` (100000).

```bat
for /L %%i in (0,1,7) do set SPR[%%i]=0
for /L %%i in (10,-2,0) do echo %%i
```

### 10.3 `for /D` directories and `for /R` trees (cmd.exe parity)

```
for /D %%v in (prefix*) do <cmd>       :: directory names, not files
for /R [path] %%v in (*.txt) do <cmd>  :: recursive files under path (cwd default)
```

`/D` expands wildcard tokens to directories (literal tokens pass through
unverified, like cmd). `/R` walks to `P4_CONFIG_DIR_RECURSE_DEPTH_MAX` (8),
emits full paths, caps at the listing limit, and skips unreadable
subdirectories; an empty set matches everything (`*`).

### 10.4 `for /f` file-line loops

```
for /f "eol=c skip=n delims=xyz tokens=a,b,m-n" %%v in (file-set) do <cmd>
```

Iterates file lines (or the active `< file` / pipe source when the set is
empty, or a command's captured output with `in ('command')`). Options:
`eol=` (comment marker), `skip=` (leading lines), `delims=` (default space +
tab; empty `delims=` = whole line one token), `tokens=` (1-based indices
bound to consecutive letters `%%a %%b ...`; `*` captures the rest).
The mechanism behind BASIC `READ`/`DATA`/`INPUT#` verbs.

### 10.5 `while` condition loops

```
while <expr> do <cmd>
```

Re-runs the body while the `set /a` expression is nonzero
(comparisons/logic per §7, plus the DOS keywords `EQU NEQ LSS LEQ GTR GEQ`
which read like `if`). Bounded by `P4_CONFIG_WHILE_ITER_MAX` (100000);
ERRORLEVEL 0 done, 1 capped/condition error, 2 usage.

**Live values — read this twice.** The pipeline expands `%VAR%` once before
the loop starts, so `%n%` inside would freeze at its entry value. Two forms
stay live:

```bat
set /a n=0
while n LSS 3 do set /a n=n+1        :: bare names read the table per pass
while %%n%% LSS 3 do set /a n=n+1    :: %% re-expands to the live value
```

Rules: never use a bare `<` `>` `&` `|` in the condition (the pipeline
claims them as redirection/pipes first) — the `LSS`/`GTR`/… keywords exist
for exactly this reason. Body `%`-forms likewise re-expand per pass from
pristine text. Countdown: `while n GTR 0 do set /a n=n-1`.

---

## 11. Subroutines: `call`, `gosub`, `return`, `on`, `goto`, `shift`

- `goto <label>` / `goto :eof` (EOF ends the frame like end-of-file).
  Missing label aborts the frame; labels accept `:name` or bare `name`.
- `call :label [args]` / `gosub :label [args]` run a local block, resume on
  `return [code]` / `exit /b [code]` / `goto :eof` / EOF. Missing target sets
  errorlevel 1 and continues.
- `call <file.bat> [args]` runs another file; `call <file.bat>::<routine>
  [args]` / `gosub <file.bat>::<routine> [args]` run a shared-library routine
  with automatic variable isolation (a library routine's temporaries never
  leak; final errorlevel propagates).
- `on <expr> goto|gosub|call <l1>[,<l2>...]` — BASIC computed dispatch: the
  `set /a` index (1-based) selects a target; out of range falls through.
- `switch <value> <m1>:<l1> [<m2>:<l2> ...] [/d:<label>]` — string-match
  dispatch (the string twin of `on ... goto`): first case-insensitive match
  jumps, else the `/d:` default, else fallthrough with ERRORLEVEL 1. Matches
  split on the last `:` so `C:\x:lbl` works; a missing label aborts like
  `goto`. Only in batch files.
- `if <cond> (true-cmd) [else (false-cmd)]` — single-line parenthesized
  groups: the command (or `&`-chained sequence) runs through the pipeline
  when the condition holds, the `else` group when it does not. Groups cannot
  hold unbalanced parens; multi-line branches stay `:label` + `goto`/`gosub`
  subroutines (see §17).
- `shift` slides `%1`..`%9`. `proc` introspects the stack (`/args` `/name`
  `/depth` `/errorlevel` `/echo` `/stdin`); `proc /labels` and `proc /goto`
  report scanned labels / pending goto.
- `exit /b [code]` leaves one file; bare `exit` unwinds every level.

---

## 12. Input, choice, menus, modals

- `set /p NAME=<prompt>` (add `/T:secs` timeout, `/P` hidden). Empty input
  leaves the variable unchanged. `set /p NAME=< file` reads one line from a
  redirect/pipe instead of the keyboard (BASIC `INPUT#`).
- `pause`, `choice [/C:keys] [/N] [/T:c,secs] [/S] [text]` (ERRORLEVEL =
  1-based key index), `delay <ms>` (pure deterministic wait — melodies and
  demos; unlike light-sleep `sleep`).
- Native modal surfaces (shared `components/modal/` runtime, never a private
  loop): `dialog`, `list` (serial selection is 1-based, ERRORLEVEL is the
  0-based index, `q` cancels with 255), `ask`, `browse`, `view`, `hexview`,
  `form` (multi-field: text/password/check/select/range, `/t:secs`
  auto-cancel; values prefill from and write back to environment variables),
  `menu` (numbered, ERRORLEVEL = index), `notify` (header notification).
- Every key wait is bounded (`P4_CONFIG_KEY_WAIT_TIMEOUT_MS` unless the
  command defines its own); on a headless board commands fall back to a timed
  path instead of stalling. Serial keys need Enter (line-buffered UART); USB
  and on-screen keyboards deliver raw keys; serial Ctrl+C is a foreground
  break (`abort_cancels` surfaces poll it every 100 ms).
- When a screen is static (same fields every run), declare it once as a
  `.FRM` file instead of hand-rolling the verbs — see §13.

---

## 13. Declarative screens (`.FRM` files — no hand-rolled modals)

A `.FRM` file is one screen as flat `KEY=VALUE` lines (the same shape as
`APPS/<APP>.APPINFO`). `screen run <file>` renders it through the existing
`dialog` / `list` / `ask` / `form` / `menu` verbs — one implementation per
surface, the file only declares. Results land in the same environment
variables with the same ERRORLEVEL contract as the verbs.
`screen info <file>` describes a screen without showing UI. A multi-screen
app is a batch file: several `screen run` calls branched with
`if errorlevel` / `goto`, exactly like hand-rolled modal sequences.

```ini
title=Network setup
type=form
timeout=60
field0=SSID=text:SSID
field1=Password=password:PASS
field2=DHCP=check:DHCP
```

| Key | Meaning |
|-----|---------|
| `type=` | Required: `dialog` `list` `ask` `form` `menu`. |
| `title=` | Screen title (required except `menu`, which prints its own). |
| `timeout=` | Seconds; the surface auto-cancels (same `/t:` semantics). |
| `message=` | `dialog` body (required). |
| `buttons=` | `dialog` buttons as `A|B` (1–2; default `OK`). |
| `items=` | `list`/`menu` entries as `a|b|c` (required, up to 32). |
| `var=` | `list`/`ask` result variable (`ask` defaults to `ASK_RESULT`). |
| `prompt=` `default=` | `ask` prompt (required) and default text. |
| `password=` | `ask` hides input when `1`/`yes`/`true`/`on`. |
| `field0..field11=` | `form` fields as `Label=type[:arg]:VAR` (`text`/`password`/`check`/`select` with `a|b|c` / `range` with `min-max`; at least one required). |

```bat
screen run SETUP.FRM
if errorlevel 255 goto cancelled
echo SSID=%SSID% DHCP=%DHCP%
```

### 13.1 Multi-screen flows (`.FLOW` files)

When an app is a chain of screens, `screen flow <file.flow>` runs a
**declarative navigation graph** so you do not hand-write a `goto` web. A
flow is one flat `KEY=VALUE` file (the same INI shape) whose nodes point at
existing `.FRM` files — the flow only routes; the batch file that invoked it
acts on the collected variables and the final ERRORLEVEL.

```ini
flow.start=welcome
flow.title=Network setup

welcome.screen=WELCOME.FRM
welcome.*=menu

menu.screen=MENU.FRM
menu.var=MENUCHOICE
menu.err:1=wifi
menu.err:2=about
menu.*=end

wifi.screen=WIFI.FRM
wifi.*=menu
about.screen=ABOUT.FRM
about.*=menu
```

| Key | Meaning |
|-----|---------|
| `flow.start=` | Required entry step id. |
| `flow.title=` | Optional flow title (shown by `screen info`). |
| `<id>.screen=` | The `.FRM` file the step shows (required per step). |
| `<id>.var=` | Optional env var whose value the `val:` rules match. |
| `<id>.err:<n>=<target>` | Rule: screen ERRORLEVEL equals `n`. |
| `<id>.val:<text>=<target>` | Rule: the step's `var` equals `text` (case-insensitive). |
| `<id>.*=<target>` | Fallback rule (always matches). |

- Rules are evaluated **in file order; first match wins**. A target is another
  step id, or `end`/`exit` (stop; ERRORLEVEL = the last screen's).
- A step with no matching rule ends the flow. Rules with an empty `err:`/
  `val:` never match.
- Step ids are `[A-Za-z0-9_-]+` (no dots); `flow`, `end`, and `exit` are
  reserved.
- `screen flow` prints one muted `flow: <id>` line per step (a trace, and what
  the harness asserts). Stops at `P4_CONFIG_SCREEN_FLOW_ITER_MAX` transitions
  with ERRORLEVEL 1, honors the foreground break (`^C`), and refuses an
  ill-formed graph (missing `flow.start`, unknown step/target, unknown rule
  token) before showing anything.
- `screen info <file.flow>` lists the steps and rules; `screen run` on a flow
  refuses and points at `screen flow`.
- Batch-only in practice: screens are modal, so the flow blocks the worker
  like any `screen run`. Dynamic screen text is not expanded — a `.FRM`'s
  values are literal.

```bat
screen flow SETUP.FLOW
echo last screen errorlevel %ERRORLEVEL%
echo collected SSID=%SSID% MODE=%MODE%
```

## 14. Persistent state, settings, temp files

All state lives on the SD card (atomic temp+rename writes, free-space
prechecks, partial-file cleanup):

- `ini <list|get|set|del|load|save> <file> [key] [value]` — any `KEY=VALUE`
  file; `appconfig <app> ...` — the app's own `sd:/APPS/<APP>.INI`;
  `temp [new [ext] | clean]` — `sd:/tmp` scratch files.
- `alias` / `unalias` (prompt-only expansion, never in batch files;
  `alias /save` → `sd:/ALIASES.BAT`, auto-run at boot), `bind F1..F12`
  (`sd:/BIND.BAT`), `macro record|stop|play|status`.
- `config` is the only runtime writer of CONFIG.SYS (brightness, rotation,
  volume, prompt, Wi-Fi autoconnect, display timeout, OSK/header prefs);
  `history /save|/load` persists recall.

---

## 15. Files, data, and guardrails (what apps may assume)

- DOS verbs with wildcards: `cd dir copy move del ren md rd type write
  append touch attrib label tree xcopy` (full DOS 6.x switch set), `find`
  (text `/I /N /C /V` + discovery `/NAME: /SIZE: /NEWER: /OLDER: /DIRS /B`
  with `N-M`/`N-`/`-M`/`N` size ranges — never bare `>`/`<`, they redirect),
  `findstr`, `more` (`Q` quits), `fc`, `comp`, `sort` (all set ERRORLEVEL
  0/1/2 for `if errorlevel` and `&&`/`||`).
- Every write prechecks free space (in-place overwrites credited),
  refuses self-copy, and removes partial destinations on failure. Use
  `shell_fs_copy_file()` semantics via the verbs — never hand-roll loops.
- `del`/`erase` and `rd /s` recycle into hidden `.trash` (recover with
  `undelete`/`restore`, manage with `trash list|info|restore|purge|empty`);
  `/p`/`/f` delete permanently. Recursive/volume-destructive ops demand the
  exact confirmation word and refuse headless.
- Records: `db` (Palm-OS-style `k=v;k=v` payloads, `/field:` `/sort:`),
  `csv rows|cols|cell|eval` (`=EXPR` via `calc`, `R<row>C<col>` refs),
  `export`/`import` (`csv|json|txt`, plus `vcf`/`ics`), `archive`/`backup`
   (USTAR `.p4a` + CRC), `crypt lock|unlock` (AES-256-GCM/PBKDF2, 512-byte
   chunks + `components/swgcm/` software fallback, zeroed secrets, `/p:` masked),
   `alarm`/`cal` (persisted events + `/run:` batch hook), `timer`/`stopwatch`, `gfind` (global find over db + alarms, plus an
  opt-in `/files` text scan, with secret/conceal
  policy), `crc32`, `asset`, `pkg` (CRC-verified `PKGS/<APP>/` bundles;
  install verifies before copying, remove trashes).
- Paths: always resolve user input (cwd-relative, `.`/`..` collapsing);
  never pass raw arguments to `fopen`. Keep one heap block per recursion
  level on the 8 KB worker stack; never a line-sized stack local (see §17).

---

## 16. Display, TUI, graphics, audio (game kit)

- **TUI grid** (`draw`, `tui`, `color`, `locate`, `anchor`): a logical
  80×25 cell buffer mapped onto the live transcript region (rotation- and
  keyboard-aware; never pixels). Verbs: `box` (single/double/rounded + title
  + nested window stack), `line`, `fill`, `text`, `bar`, `table`, `list`,
  `window`, `cursor`, `hold` (coalesce flushes), `alt-screen`, `save`,
  `restore`, `close`, `refresh`, `fullscreen`. `tui stats` reports frame
  pacing. Refused in `start` background jobs and while a gfx canvas is open
  (read-only `plot status` / text-only `plot table` excepted).
- **GFX canvas** (`gfx ...`, exclusive RGB565 `lv_canvas` + one `gfx show`
  present per frame; never blocking I/O inside a frame loop):
  `init <w> <h>` (≤320×240) / `close` / `status` / `stats` / `clear` /
  `pixel` / `line` / `rect` / `circle` / `hline` / `vline` / `triangle` /
  `ellipse` / `polygon` / `fill` / `text` (8×8 bitmap font) / `show` /
  `image <bmp> [x y [w h]]` / `load <slot> <bmp>` (8 slots, ≤64×64) /
  `blit <slot> <x> <y> [transparent]` / **`blitmany <slot> [/s:N] [/r:deg]
  <x1> <y1> [<x2> <y2> ...] [transparent]`** (one command stamps up to
  `P4_CONFIG_GFX_BLITMANY_MAX` = 64 positions — a whole formation per line;
  `/s:1..4` upscales every stamp, `/r:0|90|180|270` rotates clockwise first,
  refused when the transformed sprite exceeds the canvas) /
  `free` / `slots` / `save`. Close any `draw` surface before `gfx init`
  (a canvas and TUI never compose).
- **Plot** (`plot func|polar|para|data|bar|table|line|point|axes|...`):
  world-coordinate graphs onto canvas or TUI through one shared viewport;
  function sampling binds `calc` over `X`/`T` (restored afterwards). Canvas
  plots never auto-show (compose, then one `gfx show`).
- **Images:** one 24/32-bit BI_RGB decoder; `.bmp`/`.dib` route by
  `filetype`; `view`/`open` (fullscreen viewer), `draw image` (16-color
  glyph ramp), `gfx image` (canvas blit), `image info` (scriptable WxH/bpp).
  Bounded by `P4_CONFIG_IMAGE_MAX_BYTES`.
- **Game-loop pattern** (BOUNCE.BAT shape): `gfx init` once → per frame
  (`set /a` physics in array slots → `gfx clear` → `gfx blitmany` the
  formation → ONE `gfx show`) → `gfx stats` after. Mid-loop `screenshot`
  freezes the loop for seconds — capture once after.
- **Audio:** `beep`, `tone <freq> [ms]`, `wavplay <file>`
  (16-bit PCM mono/stereo 22050/44100 Hz), `audio status|stop`,
  `volume <0-100>`. Single background slot (`audio stop` cuts it); batch
  files never block. First `tone` after boot logs a benign `i2s_common`
  error while still playing (managed-codec open path).
- **Idle/power:** `power idle`, `sleep`, `deepsleep` (timer/GPIO wake —
  touch wake is honestly unavailable), `shutdown`/`poweroff` (PMIC cut on
  Tab5, deep sleep on reference). New input sources must notify activity so
  the idle clock resets; idle-off toggles only the backlight.

---

## 17. App packaging, distribution, testing

- `pkg list|info|verify|check|install <app> [/signed]|remove <app>` over
  `PKGS/<APP>/` bundles (`APPINFO` + `.ASSETS` manifests + payloads). Install
  CRC-checks everything before touching installed files and can refuse on a
  bad/missing signature; remove recycles. `pkg key ...` manages the trusted
  key; sign bundles host-side with `tools/pkg_sign.py`.
- Hybrid apps (`type=hybrid`) run a `.BAT` shim into a linked C `entry=`;
  native-only payloads are verified data (no SD execution — MCUs have no
  `dlopen`). See [`ABI.md`](ABI.md) and `docs/native_packaging.md`.
  `tools/newapp.py` scaffolds an `applib` component from `samples/whoami/`.
- **Test your app:** `python tools/p4test_run.py <COM> --only s06_batch`
  (109 language checks) and `--only s14_apps` (reference apps, incl. the
  `appdiff` golden gate — app transcript output must stay byte-stable).
  New language coverage belongs in `tools/suites/s06_batch.py` plus the
  on-board Unity pure-helper tests (`test/main/test_*.c`, run with
  `python tools/unit_run.py <COM>`).
- **Background loops:** compute-only `while`/`for /L` bodies run fine under
  `start` (own batch context per worker; `taskkill` stops them cooperatively
  — `delay: stopped`). Display verbs refuse there by design (one shared
  canvas/TUI writer): poll sensors, log to `db`, or crunch arrays in the
  background; keep every pixel foreground.

---

## 18. Budgets and prohibitions (read before clever ideas)

- **Stacks:** the worker task is 8 KB and `batch → pipeline → batch` is
  recursive. Never a command/line-sized stack local on that path —
  heap-allocate transient buffers (the expansion/chain/frame/line buffers
  all are). The checker task (`alarm`) keeps 8 KB for `snprintf` frames.
- **Heap:** internal DMA RAM is scarce (PSRAM is not DMA here). Large
  buffers prefer PSRAM with internal fallback (`applib` policy: ≥512 B →
  PSRAM first). SD reads never DMA into PSRAM (bounce through internal).
- **Transcript:** up to 64 KB; per-line repaint is O(transcript) — skipped
  while hidden (canvas/TUI/app covers it) with one repaint on reveal.
  `draw hold` coalesces TUI flushes; `for`/`while` defer repaints per loop.
- **Never:** a second dispatcher or dispatch table; reaching past the ops
  tables (`shell_command_ops_t`, `batch_command_ops_t`, `applib_*_ops_t`);
  `shell_bridge_*` trampolines; including `command.h`/`batch.h`/`networking.h`
  from below the layer; a second BMP parser or CRC-32; hardcoded tunables
  (new literal → `P4_CONFIG_*` + YAML twin); private modal loops (new
  surfaces are `modal_surface_t`); unbounded key waits; `portMAX_DELAY`
  modal waits without `abort_cancels`.
- **DOS-shape rule:** new language surface must look like DOS/BASIC
  (`for /L`, `%VAR:~%`, `INSTR`, `blitmany`, `switch`, `!VAR!`) and reuse the
  existing engines (set/a evaluator, calc parser, substitution runner, BMP
  decoder, modal verbs). Nothing is removed: every addition is a new switch,
  function, or verb.

---

## 19. Quick game sketch (arrays + `/L` + `while` + `blitmany`)

```bat
@echo off
rem --- init a 4-alien formation in array slots
for /L %%i in (0,1,3) do set /a AX[%%i]=%%i*40+20
for /L %%i in (0,1,3) do set /a AY[%%i]=30
gfx init 320 240
set /a frame=0
while frame LSS 200 do call :step
gfx save FORM.BMP
gfx close
exit /b 0

:step
set /a frame=frame+1
gfx clear 0
rem (move + stamp the formation; one blitmany per frame)
set /a AX[0]=AX[0]+2
gfx blitmany 0 %AX[0]% %AY[0]% %AX[1]% %AY[1]% %AX[2]% %AY[2]% %AX[3]% %AY[3]%
gfx show
return 0
```

Notes: `%AX[0]%` in the `blitmany` line expands once per `:step` call (fresh
pipeline pass per `call`), so it tracks the array; the `while` condition
uses bare names (§10.5). Cap frames so the loop always terminates; `delay`
paces it deterministically.
