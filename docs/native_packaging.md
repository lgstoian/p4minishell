# Native-App Packaging Spec (v1.1.0 — store-only)

How an SD-deployable **native** app is described, verified, and installed
with the existing `pkg` system. In v1.1.0 native payloads are **stored and
verified only** — the shell cannot execute them yet (bare-metal ESP-IDF has
no `dlopen`; a loader is v1.2+ work). Storing them now means bundles built
today keep working when execution lands.

- **Version:** v1.1.0 · **Target:** ESP32-P4 + ESP32-C6
- Related: [`command.md`](../command.md) (`pkg`, `asset`, `launch`),
  [`SDK.md`](../SDK.md) (applib ABI), `tools/push_pkgs.py`.

## 1. Bundle layout

Same shape as a batch bundle, plus two APPINFO keys:

```
PKGS/<APP>/
  <APP>.APPINFO      INI metadata (see §2)
  <APP>.ASSETS       path=HEXCRC manifest (unchanged, shared with `asset`)
  NATIVE/<APP>.P4X   the opaque native payload (any bytes, CRC-covered)
  ...                optional batch shims / docs (normal payloads)
```

Payload convention: the executable blob lives at `NATIVE/<APP>.P4X`
(uppercase app name, matching `asset_app_ok`). Extra data files may sit
beside it; every file must be manifest-listed or install aborts.

## 2. APPINFO keys

| Key | Required | Meaning |
|---|---|---|
| `title`, `description`, `version` | yes | As for batch apps |
| `type` | yes for native | `batch` (default when absent) or `native`; anything else aborts `pkg install` loudly |
| `abi` | native only | applib ABI the blob was built against, e.g. `applib-1` (must equal `P4_CONFIG_NATIVE_ABI`); mismatch warns at install, refuses at future execution |
| `arch` | native only | `esp32p4` today; a future loader refuses anything else |
| `entry` | native only | C symbol the loader will resolve, e.g. `myapp_main` (an `app_main_t`) |

```ini
title=Hello Native
description=applib sample as a native package
version=1.0
type=native
abi=applib-1
arch=esp32p4
entry=hello_main
```

## 3. Install / verify / remove semantics (v1.1.0)

- `pkg install <app>`: identical two-pass flow (verify-then-copy over
  `shell_fs_copy_file`), including `.APPINFO`/`.ASSETS`. Afterwards, for
  `type=native`, it prints
  `pkg: <app> is a native package (stored only, execution needs a v1.2+ loader)`
  and succeeds (ERRORLEVEL 0). Unknown `type=` aborts before any copy
  (ERRORLEVEL 1). An `abi` mismatch with `P4_CONFIG_NATIVE_ABI` warns but
  still stores.
- `pkg verify|check|info|list`: unchanged mechanics; `list`/`info` display
  the `type` (and `abi` when present).
- `pkg remove <app>`: trashes payloads + metadata like batch apps.
- `launch`: native apps are never offered — discovery scans `*.bat`, and a
  v1.1 native bundle must not ship a same-name `.BAT` shim (reserved for the
  future loader entry point).

## 4. Trust model

Same as batch bundles today: per-file CRC-32 against the manifest, verified
before anything is copied. No signatures in v1.1.0 — do not install native
payloads from untrusted sources; a malicious blob is inert on v1.1 firmware
but must still be treated as untrusted bytes. Signed manifests are v1.2+
work (see `roadmap.md` platform/security rows).

## 5. Future loader sketch (v1.2+, non-normative)

Position-independent blob (`-fPIC`, single `app_main_t` entry) relocated by
a firmware loader into PSRAM, `abi`/`arch` checked, then registered through
the existing `app_register` table so dispatch, ERRORLEVEL, and redirection
behave exactly like linked-in apps. The `NATIVE/*.P4X` path and this spec's
keys are chosen so v1.1 bundles load unmodified.
