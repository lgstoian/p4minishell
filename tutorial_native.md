# Writing a Native (C) App

Most P4MiniShell apps should be **batch files** — see
[`tutorial_batch.md`](tutorial_batch.md). A native C app is for work batch
cannot do: timing-critical loops, large data handling, direct use of a
component API, or a polished LVGL surface. Native apps are linked into the
firmware and run through the `applib` runtime and the native-app ABI.

This tutorial covers the ABI and the runtime groups. For the full function
reference see [`API.md`](API.md) and [`SDK.md`](SDK.md); for the invariants you
must obey see [`ai-context.md`](ai-context.md).

---

## 1. The contract

A native app is a C function with the `app_main_t` signature:

```c
typedef int (*app_main_t)(int argc, char **argv);
```

Registering it makes its name a shell command:

```c
app_register("myapp", "one-line description", myapp_main);
```

Once registered:

- typing `myapp a b` (or calling it from a batch file) runs `myapp_main(3,
  {"myapp", "a", "b"})`;
- `argv[0]` is the app name;
- the return value becomes the command **ERRORLEVEL**, so a batch file can
  branch on it with `if errorlevel N`;
- apps are dispatched **after** every built-in and on-SD `.bat` lookup, so an
  app name can never shadow a built-in or a batch file.

`app_register()` is defined in `applib_app.h`; the `apps` command lists every
registered app, and `app_get()` iterates them.

## 2. A complete example

The reference sample is `hello` in `main/native_apps.c`:

```c
#include "p4minishell.h"
#include "applib.h"

static int hello_main(int argc, char **argv)
{
    const char *cwd  = app_get_cwd();
    const char *path = app_env_get("PATH");

    app_printf("hello: %d argument(s)\n", argc);
    for (int i = 0; i < argc; i++) {
        app_printf("  argv[%d] = \"%s\"\n", i, argv[i] ? argv[i] : "(null)");
    }
    app_printf("cwd  = \"%s\"\n", cwd  ? cwd  : "(unavailable)");
    app_printf("PATH = \"%s\"\n", path ? path : "(unset)");

    if (argc == 2 && strcmp(argv[1], "test") == 0) {
        app_env_set("HELLO_RESULT", "ok");
        return 0;
    }
    return argc == 2 && strcmp(argv[1], "fail") == 0 ? 1 : 0;
}

void native_apps_register(void)
{
    (void)app_register("hello",
                       "native-app ABI sample: show argv, env, and cwd",
                       hello_main);
}
```

`native_apps_register()` is called from `command_init()` (see
`main/main.c` / `main/native_apps.c`). Add your registration there (or in a new
`*_register()` function called alongside it).

## 3. Console output (the redirection layer)

Include `applib_console.h` (or the `applib.h` umbrella):

```c
app_printf("plain line\n");
app_printf_ansi("@Gdone@R\n");          // ANSI @-specifier colours
app_print_heading("Status");
app_print_field("SSID", "%s", ssid);
app_print_ok("connected");
app_print_warning("low battery");
app_print_error("write failed");
app_print_usage("myapp <file>");
app_print_styled("7", "inverse video");  // raw SGR codes
```

An app's stdout **is the transcript**, which is also the redirection layer:
`myapp > out.txt` is captured exactly like a built-in command. App output is
mirrored to the serial console too.

Colour output goes through the shell palette; use the `app_print_*` helpers or
`@`-specifiers rather than raw escape bytes.

## 4. Environment and current directory

Include `applib_env.h`:

```c
const char *mode = app_env_get("MODE");   // NULL when unset
app_env_set("RESULT", "42");              // shared env table
const char *cwd  = app_get_cwd();         // current shell directory
```

The environment table and cwd are read through a registered ops table, so
`applib` never includes `batch.h` or `storage.h`. This is the native equivalent
of batch's `%VAR%`, `%1..%9`, and `%CD%`.

## 5. Input with timeouts

Include `applib_input.h`:

```c
char key;
if (app_wait_key(5000, &key)) { /* got a key within 5 s */ }

char line[128];
if (app_read_line(line, sizeof line, 10000)) { /* got a line */ }

char pw[64];
app_read_password(pw, sizeof pw, 10000);   // no echo

const char *items[] = { "One", "Two", "Three" };
int choice = app_menu("Pick", items, 3, 15000);  // 1-based index, 0 cancel
```

Every wait is bounded and returns `false` on a headless board rather than
stalling. Input is backed by the shell's key queue, so USB, touch, and serial
all work.

## 6. Persistent state

Include `applib_state.h` — all storage is on the SD card:

```c
char v[64];
app_ini_get("MYAPP.INI", "theme", v, sizeof v);
app_ini_set("MYAPP.INI", "theme", "amber");
app_ini_delete("MYAPP.INI", "theme");

char tmp[128];
app_temp_path(tmp, sizeof tmp, "log");   // a path under sd:/tmp
app_temp_cleanup();
```

For records, `applib_db.h` wraps the same `db` store the shell command uses
(`app_db_*`).

## 7. Memory and reporting

Include `applib_mem.h`:

```c
void *p = app_alloc(n);          // PSRAM-aware allocation policy
char *s = app_strdup(str);
app_free(p);

app_report_error("could not open %s", path);
app_report_warning("retrying");
app_report_info("started");
```

Use the applib allocators (not raw `malloc`) for per-document/large buffers so
allocation stays PSRAM-aware. `app_report_*` goes to the shell debug log
(`debug` command).

## 8. Wi-Fi state (read-only)

Include `applib_net.h`:

```c
if (app_wifi_is_connected()) {
    int rssi = app_wifi_get_rssi();
    const char *s = app_wifi_state_string();
}
```

These route through the registered `applib_net_ops_t` table; `applib` never
includes `networking.h`. If you need to own the network stack, that belongs in
`components/networking/`, not in an app.

## 9. Screen takeover and notifications

Include `applib_ui.h`:

```c
app_mode_enter(true);      // save the screen, take over (optionally full-screen)
/* ... draw or run ... */
app_mode_exit();           // restore the shell

app_notify("build finished");
```

For a full LVGL surface, follow the **modal surface** pattern documented in
[`SDK.md`](SDK.md) ("Modal app surfaces"): a worker session plus an LVGL view
on the shared runtime in `components/modal/`, never a private event loop. The
`edit` editor (`components/editor/`) is the reference implementation.

## 10. Build and register

The fast path is the scaffolder (template: `samples/whoami/`):

```powershell
python tools/newapp.py myapp
```

It creates `samples/myapp/` (`myapp.c/.h`, `CMakeLists.txt`, `README.md`)
and prints the three wiring steps: root `CMakeLists.txt`
`EXTRA_COMPONENT_DIRS`, `main/CMakeLists.txt` `REQUIRES`, and a
`<name>_register()` call from `native_apps_register()`. Then:

1. Put the source in the component that owns the domain (or in `main/` for a
   small sample).
2. Add it to that component's `CMakeLists.txt` `SRCS`.
3. Add the dependency to `REQUIRES` if needed (`applib` plus anything you call
   directly). A new component must be added to the root `CMakeLists.txt`
   `EXTRA_COMPONENT_DIRS` **and** `test/CMakeLists.txt`.
4. Call `app_register()` once at init.
5. Build both projects (`idf.py build` and the `test/` project) with zero
   warnings, flash, and verify on hardware.

## 11. Rules that matter

- Keep heavy buffer allocation on the heap/PSRAM — never a command-sized local
  on the worker stack.
- An app runs on the command worker task; do not block it indefinitely, and do
  not call LVGL APIs from it directly (use the modal runtime or schedule onto
  the LVGL task).
- Return a real ERRORLEVEL; build batch-friendly output (and a `/b`-style bare
  mode if a script will parse it).
- Do not print secrets.
- Update the docs and `changelog.md` with your change.

See [`SDK.md`](SDK.md) for the full "add a command / add a file-writing command
/ add a modal surface" workflows, and [`ai-context.md`](ai-context.md) for the
non-negotiable invariants.
