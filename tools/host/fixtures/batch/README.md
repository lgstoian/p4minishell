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

`HELLO.BAT` is deliberately small: every verb is the firmware's own
`shell_command_*` implementation in `batch.c` / `batch_expr.c` /
`batch_env.c`. The host runner only routes lines to those verbs; it does not
duplicate their parsing, evaluation or output.

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
