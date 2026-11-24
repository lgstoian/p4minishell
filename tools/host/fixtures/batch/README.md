# Batch host fixtures

Real `.bat` apps run on the host through the **real interpreter**
(`components/batch/batch.c`), not a Python or C reimplementation. The harness is
`test/host/main/host_batch_run.c`; the gate is `tools/host_batch_run.py`.

The ELF maps the board's SD mount point (`/sdcard`, from `board_identity.h`)
onto this directory through the additive `batch_io_ops_t` file/stat seam. Set
the root with `--root DIR` (or env `P4_HOST_SD_ROOT`); the default is this
directory. A missing root is never faked as a mounted card.

## Fixtures

| Name | Covers |
|------|--------|
| `HELLO.BAT` | `@echo off`, `set NAME=...`, `set /a N=1+2`, `if "%NAME%"=="p4"`, `call :sub`, `return`, `goto :eof`, and the `[M-HOST-BATCH-DONE]` marker |
| `FEATURES.BAT` | the additive v1.6 surfaces through the same one interpreter: `str /rev|/count|/pad`, `map /set|/values|/incr|/get`, `for /K` (key **and** live value binding), `func NAME`/`return <expr>` publishing `%RESULT%`, `trap /on errorlevel` and `trap /on exit`, plus `[M-HOST-STR]` / `[M-HOST-FORK]` / `[M-HOST-MAP]` / `[M-HOST-FUNC]` / `[M-HOST-TRAP]` / `[M-HOST-EXIT]` markers |

Both fixtures use only the firmware's own `shell_command_*` implementations in
`batch.c` / `batch_expr.c` / `batch_env.c` / `batch_str.c` / `batch_map.c` /
`batch_trap.c`. The host runner only routes lines to those verbs; it does not
duplicate their parsing, evaluation or output. Unsupported verbs are reported
honestly as `batch: unsupported command on this host lane: <verb>` with a
nonzero ERRORLEVEL, and the gate rejects that string (never a false green).
The `trap /on int` break path is covered by the Unity suite: a headless host
has no injected `^C`, so no fixture pretends to fire it.

## Run

Build both host ELFs in WSL (fresh `build-wsl`):

```bash
cd test/host
idf.py -B build-wsl build
```

Then:

```bash
python tools/host_batch_run.py --check          # default fixture: HELLO
python tools/host_batch_run.py --update         # regenerate baseline/*.txt
python tools/host_batch_run.py --self-test      # red canary for the gate itself
```

The gate locates the ELF by searching the build directories under the repo
rather than hardcoding a path; if the ELF is a POSIX binary it is run through
WSL. Baselines live in `baseline/`.

A host pass is a pre-gate only. It says the shared interpreter behaves as
baselined on the host; it says nothing about hardware sign-off (see
`harness.md`).
