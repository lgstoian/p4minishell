# `whoami` — sample native app component

A minimal applib app in its own component directory, shaped like an external
(out-of-tree) app: `whoami.c` + `whoami.h` + `CMakeLists.txt` (REQUIRES
`applib` only — never shell/batch/storage/networking headers).

- Command: `whoami` (argv/env/cwd report plus a persistent `visits`
  counter in `sd:/APPS/WHOAMI.INI`); `whoami reset` clears the counter.
- Registration: call `whoami_register()` once beside
  `native_apps_register()` (see `../../main/native_apps.c`). It lands in the same
  `app_register` table as `hello`, so dispatch and `>`/`>>`
  redirection behave identically. (`whoami` always returns ERRORLEVEL 0 —
  `whoami reset` included — so it does not demonstrate failure levels; see
  `hello fail` in `../../main/native_apps.c` for ERRORLEVEL 1.)
- Scaffold your own from this directory: `python ../../tools/newapp.py <name>`.

Rules that matter here (see `../../tutorial_native.md`, `../../SDK.md`): heap (never
command-sized stack locals), bounded input, no direct LVGL
calls from the worker task.
