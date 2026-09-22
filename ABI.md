# P4MiniShell Application ABI & Package Trust (firmware v1.2.1)

> **The contract between firmware and applications.** Read this before
> shipping an app bundle or changing the native-app entry points. It freezes
> the launch model, the `applib` entry ABI, the package metadata keys, and
> the manifest trust rules. Architecture lives in
> [`documentation.md`](documentation.md); authoring recipes in
> [`batch.md`](batch.md) and [`SDK.md`](SDK.md); the packaging walk-through
> in [`docs/native_packaging.md`](docs/native_packaging.md).

- **Version:** `applib-1` (the `P4_CONFIG_NATIVE_ABI` tag)
- **Target:** ESP32-P4 + ESP32-C6 · **ESP-IDF:** v5.5.5
- **Related:** [`command.md`](command.md) (`pkg`, `launch`, `asset`),
  [`API.md`](API.md) (applib API), [`roadmap.md`](roadmap.md).

---

## 1. One launch model: everything starts from a `*.bat`

Every app on the SD card is launched by a batch file. That is the single
entry contract; the *work* behind the shim can be pure batch, a linked-in C
entry, or a mix of both:

| App kind | `type=` | On the card | What runs |
|----------|---------|-------------|-----------|
| **Batch** | `batch` (or absent) | `<APP>.BAT`, optional `APPS/<APP>.APPINFO` | The `.bat` file itself (the batch engine). |
| **Hybrid** | `hybrid` | `<APP>.BAT` + `APPS/<APP>.APPINFO` + a linked-in C `entry=` | The `.bat` shim, which calls the C entry by name at the chosen moment. |
| **Native** | `native` | `APPS/<APP>.APPINFO` (+ optional data payloads) | The linked-in C entry, dispatched directly by name (development/tests). |

- `launch`, `launch <name>`, and typing a `<name>` all resolve the `.bat`
  first (built-ins → `.bat` → linked native). `launch /list` marks hybrid
  shims with ` [hybrid]` so scripts and users can tell them apart.
- A **hybrid** shim is ordinary batch: it may set variables, branch, open a
  `screen`, then `call` the C entry name, then act on its ERRORLEVEL and
  environment results. This is how a C app gets a batch "front door"
  (splash, settings, argument processing) with no new runtime.
- A **native** app (no shim) is the developer path for tests and tools;
  users are expected to reach apps through `launch`, so shipping native-only
  is discouraged for end-user apps.

There is **no SD code execution**. The firmware has no `dlopen`, no MMU
address spaces, and no relocator; a `.P4X`/`.bin` payload on the card is a
CRC + signature-covered **data blob**, never executed. "Installing a native
app" means the blob is verified and stored; the C code that can run is the
code linked into the firmware image and registered via `app_register`.

---

## 2. Native-app entry ABI (`applib-1`)

Declared in `components/applib/applib_app.h`.

```c
typedef int (*app_main_t)(int argc, char **argv);

bool app_register(const char *name, const char *description, app_main_t entry);
bool app_find(const char *name);
bool app_dispatch(int argc, char **argv, int *errorlevel_out);
bool app_get(int index, char *name_out, size_t name_size,
             char *desc_out, size_t desc_size);
```

- **Signature.** `app_main_t` takes the command words (`argv[0]` = the name
  the user typed) and returns an `int` that becomes **ERRORLEVEL** (branchable
  with `if errorlevel N` and `&&`/`||` from batch).
- **Registration.** A native app calls `app_register()` once at boot (from
  the app's own `*_register()` called by `native_apps_register()` in
  `main/native_apps.c`). Names are case-insensitive, capped by
  `P4_CONFIG_APP_NAME_BYTES`; descriptions by `P4_CONFIG_APP_DESC_BYTES`; the
  table holds `P4_CONFIG_APP_MAX` apps.
- **Dispatch.** The shell dispatcher runs a registered app **after**
  built-ins and `.bat` lookup, so an app can never shadow a command or a
  script (see `command.c` `shell_execute_command_core`). New native apps MUST
  go through `app_register`, never a private dispatcher branch.
- **Process surface.** Inside an entry the app sees the same process contract
  a batch file does: stdout is the transcript (so `myapp > out.txt` captures
  it), env/cwd come through the registered `applib_env_ops_t` table, input
  through `applib_input.h`, persistent state through `applib_state.h`. See
  [`API.md`](API.md) "Applib Module API" for the full surface.

### Versioning rule

`P4_CONFIG_NATIVE_ABI` is the ABI tag (`applib-1`). It changes only on a
**breaking** change to `app_main_t`, `app_register`/`app_dispatch`, or the
meaning of a bundle metadata key. Additive helpers (new `applib_*` functions,
new ops-table hooks) do **not** bump it. A bundle's `abi=` must equal the
firmware's tag to execute; a mismatch is a hard refusal at execution (and a
warning at install so the bundle is still inspectable). `arch=` pins the
target (`esp32p4`).

---

## 3. Bundle metadata (`APPS/<APP>.APPINFO`)

Flat `KEY=VALUE`, the same shape as any INI file (read through the shared
`storage_ini` core). Keys:

| Key | Required | Meaning |
|-----|----------|---------|
| `title`, `description`, `version` | yes | Human metadata; `title` is shown by `launch`/`pkg list`. |
| `type` | no | `batch` (default), `hybrid`, or `native`. Any other value aborts `pkg install`. |
| `abi` | hybrid/native | Must equal `P4_CONFIG_NATIVE_ABI` (`applib-1`). |
| `arch` | hybrid/native | Target slug, `esp32p4`. |
| `entry` | hybrid/native | Name of the linked-in `app_main_t` the shim (hybrid) or dispatcher (native) invokes. Hybrid bundles **must** set it. |

Bundle layout under `PKGS/<APP>/` mirrors the install tree; every file must
be CRC-listed in `<APP>.ASSETS`. The optional native payload lives at
`NATIVE/<APP>.P4X` (opaque, CRC-covered, never executed). See
[`docs/native_packaging.md`](docs/native_packaging.md) for the packaging
walk-through.

---

## 4. Manifest trust (CRC + ECDSA P-256)

Two independent checks gate an install:

1. **Integrity (always).** Every `path=HEXCRC` line is verified against the
   file before any copy — CRC-32, the same primitive as `crc32`/`asset`.
2. **Authenticity (when signed).** A manifest may carry one
   `SIGN=<128 hex>` line: a raw `r||s` ECDSA P-256 signature over the
   *canonical* manifest bytes, made with the publisher's private key.

### Canonical bytes

Exactly every manifest line **except** blank lines, `#`/`;` comment lines,
and any `SIGN=` line, each terminated by one `\n` (a single trailing `\r` on
a CRLF file is stripped first). Comments and the signature line are therefore
not signed; the file list is. The device rebuilds these bytes itself
(`pkg_sign_canonical`), so the signer and verifier agree byte-for-byte.

### Keys and policy

- The private key never touches the device. The signer is
  `tools/pkg_sign.py` (`keygen` / `sign` / `verify`).
- The device trusts one raw 64-byte P-256 public key (`X||Y`) stored in the
  `p4sign` NVS namespace, managed by `pkg key show | install <file> | clear`
  (install rejects an off-curve point). `pkg key show` prints a SHA-256
  fingerprint.
- **Verdicts:** `signed OK` (verified), `unsigned` (CRC-only), `signed but no
  trusted key`, `BAD signature` (tampered/malformed). A **BAD** signature
  always refuses, even if every CRC matches — an attacker can recompute CRCs.
- **Policy:** `P4_CONFIG_PKG_REQUIRE_SIGN` (default `0`) refuses unsigned
  bundles when `1`; `pkg install <app> /signed` enforces per invocation. A
  signed-but-unkeyed bundle follows the same policy.

### Enforcement matrix

| Situation | `pkg install` | `pkg install /signed` or policy=1 |
|-----------|---------------|-----------------------------------|
| Unsigned, CRCs ok | installs (notes `unsigned (CRC-only)`) | refuses (`unsigned (policy refuses)`) |
| Signed, key trusted, valid | installs (`signed OK`) | installs |
| Signed, no trusted key | installs with a warning (CRC-only) | refuses (`no trusted key`) |
| Signed, bad/malformed signature | **refuses** | **refuses** |
| CRC mismatch on any payload | **refuses** | **refuses** |

`pkg verify`/`check`/`info` report the same verdict (`signed: yes/no/...`).

---

## 5. Guarantees and non-goals

**Guaranteed to apps**

- ERRORLEVEL propagation, transcript-as-stdout with `>`/`>>` capture, the
  shared environment table, and the storage-owned cwd — identical for batch
  and native entries.
- Additive compatibility: new applib helpers and new optional metadata keys
  never break an existing `applib-1` bundle.
- Signed/unsigned coexistence: unsigned bundles keep the CRC-only contract;
  signing is opt-in and never breaks an unsigned bundle.

**Explicitly not provided**

- SD/flash code execution (`dlopen`, dynamic linking, per-app memory
  isolation). C apps are linked into the firmware; the `.BAT` shim is their
  front door.
- A second dispatcher or a private app-entry path — `app_register` +
  `launch`/`.bat` is the only model.
- Key revocation lists or online trust: trust is a single installed public
  key; rotating it means re-signing (and a `pkg key install`).

---

## 6. Adding a hybrid app (worked shape)

```
apps/myapp/MYAPP.BAT        # shim: setup, then `call` the C entry
apps/myapp/MYAPP.APPINFO    # type=hybrid, abi=applib-1, arch=esp32p4, entry=myapp
```

```bat
@echo off
rem Front door: prove the environment, then hand off to the C entry.
set MYAPP_MODE=full
myapp %*
if errorlevel 1 echo [M-MYAPP] FAIL
exit /b %ERRORLEVEL%
```

The C side is a normal `applib` component (see `tools/newapp.py`) registering
`myapp` via `app_register`; `packages/mypkg` bundles it with
`python apps/push_pkgs.py COMx --sign mykey.priv.pem`. The full authoring
walk-throughs are [`tutorial_native.md`](tutorial_native.md) (C) and
[`tutorial_batch.md`](tutorial_batch.md) (batch).

---

## 7. Reference implementation map

| Concern | Where |
|---------|-------|
| Entry ABI + registry | `components/applib/applib_app.h`, `applib.c` |
| Registration at boot | `main/native_apps.c` (`native_apps_register`) |
| Dispatch + launch discovery | `components/command/command.c` (`shell_execute_command_core`, `shell_launch_*`) |
| Signature/trust core | `components/command/pkg_commands.c` (`pkg_sign_*`) |
| Manifest CRC verify | `components/command/asset_commands.c` (`asset_verify_app`) |
| Host signer | `tools/pkg_sign.py` |
| Host bundle push | `apps/push_pkgs.py` |
| Build-time guard | `_Static_assert` on the SDIO mirror (see `PORTING.md`) |

**Changing the ABI:** update this file, `P4_CONFIG_NATIVE_ABI`, the applib
headers, `docs/native_packaging.md`, and the changelog together; bump the tag
on any breaking change and say why in the changelog.
