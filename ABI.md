# P4MiniShell Application ABI & Package Trust (firmware v1.6.0)

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
  shims with ` [hybrid]` so scripts and users can tell them apart; it stays
  escape-free for `for /f` parsing, while the human `launch` menu prints
  every app name as a tap-confirm hyperlink.
- A **hybrid** shim is ordinary batch: it may set variables, branch, open a
  `screen`, then `call` the C entry name, then act on its ERRORLEVEL and
  environment results. This is how a C app gets a batch "front door"
  (splash, settings, argument processing) with no new runtime.
- A **native** app (no shim) is the developer path for tests and tools;
  users are expected to reach apps through `launch`, so shipping native-only
  is discouraged for end-user apps.
- A shim may also launch an **opt-in signed native payload** (§4.1): either a
  pure native payload, or a hybrid that mixes batch steps, the native payload,
  and a webapp page. The mix is authored in ordinary batch; the payload stays
  one envelope. This is an addition to the shim, not a new app `type=`.

> **Additive opt-in capability (amendment; the `applib-1` tag is unchanged).**
> A `.bat` shim may launch the reserved `NATIVE/<APP>.P4X` blob when that blob
> carries the signed envelope in §4.1 and passes every trust gate. There is
> **no new executable format** and **no arbitrary unsigned SD execution**:
> the reserved blob path, the `.bat` front door, and the one firmware loader
> path are reused unchanged. No second dispatcher, parallel package format,
> or removal of an existing feature is involved. The format-only parser is
> `components/nativepay`; the loader and its launch surface are a later,
> hardware-verified step ([`roadmap.md`](roadmap.md)).

There is **no arbitrary SD code execution**. The firmware has no `dlopen`, no
MMU address spaces, and no relocator. A `.P4X`/`.bin` file on the card stays a
CRC- and signature-covered **data blob**: only the opt-in, signed payload
envelope in §4.1 may be handed to the single loader path, and only when an
ordinary `.bat` shim asks for it. Unsigned blobs, arbitrary `.bin` files, and
any payload that fails a trust gate remain data, never code. "Installing a
native app" still means the blob is verified and stored; the C code that can
run is code linked into the firmware image and registered via `app_register`
or delivered through the one opt-in payload path.

A bundle is either a `PKGS/<APP>/` directory or a single-file `PKGS/<APP>.PKG`
ZIP archive (the `.P4X` extension stays reserved for the one native payload
blob: data unless opted in, signed, and launched by a `.bat` shim — §4.1).
`pkg install <APP>` prefers the directory and falls back to the
archive, unpacking it through the ONE ZIP reader; `pkg unpack <APP>` does only
the unpack. Installs are staged: existing destinations are backed up under
`PKGS/.installing/<APP>/`, each file is replaced atomically (temp + rename), and
the backups are restored if the install fails. See
[`docs/native_packaging.md`](docs/native_packaging.md).

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
| `title`, `description`, `version` | yes | Human metadata; `title` is shown by `launch`/`pkg list`. `version` is compared numerically (`pkg outdated`, downgrade guard). |
| `type` | no | `batch` (default), `hybrid`, or `native`. Any other value aborts `pkg install`. |
| `abi` | hybrid/native | Must equal `P4_CONFIG_NATIVE_ABI` (`applib-1`). |
| `arch` | hybrid/native | Target slug, `esp32p4`. |
| `entry` | hybrid/native | Name of the linked-in `app_main_t` the shim (hybrid) or dispatcher (native) invokes. Hybrid bundles **must** set it. |
| `handles` | no | Comma-separated extensions the app opens (`.txt,.md`); listed by `ftype`, honoured by `assoc`/`open`. Documentary — no app is forced to accept `%1`. |
| `setup` | no | Setup entrypoint label (convention: `:setup`, run via `launch <APP> /setup` with `%1=/setup`); shown by `pkg info`. |
| `autostart` | no | `true` advertises boot-start; `pkg install` suggests `startup add <APP>` (never auto-added). |
| `alias` | no | Short name; `pkg install` suggests `alias X="launch <APP>"` (never auto-registered, never a builtin name). |
| `data` | no | Comma-separated uninstall-manifest paths (saves, stores); `pkg remove` keeps them with a warning, `pkg purge` deletes them. |
| `requires` | no | Dependency (`APP>=1.2`, `APP=1.2`, `APP>1.2`); enforced at install. |
| `services` | no | Comma-separated dependency list (`APP>=1.2`, `APP=2`, `APP>3`, or a bare `APP` = any version); enforced at install (every declared service must be installed and satisfy its version). Additive metadata — no ABI bump. |
| `min_fw` | no | Minimum firmware (`1.4.0`); enforced at install, never bypassed by `/force`. |
| `alpha` | no | Opt-in TUI/modal transparency, 0-255 or `0-100%` (255 = opaque). `launch` applies it to the whole TUI surface and every modal panel for the app's lifetime, then restores the default. (v1.4.3) |
| `scrim` | no | Opt-in modal backdrop dimming, 0-255 or `0-100%` (0 = none). Black at that opacity behind modal panels; only meaningful with a translucent panel. (v1.4.3) |
| `payload` | no | Opt-in path to the signed native payload blob (`NATIVE/<APP>.P4X`), relative to the bundle. Additive metadata only — no ABI bump. Launch is exclusively through the app's `.bat` shim and the one loader path (§4.1). |

New keys are additive metadata only — they never change the entry ABI, so
no `P4_CONFIG_NATIVE_ABI` bump is required to adopt them.

Bundle layout under `PKGS/<APP>/` mirrors the install tree; every file must
be CRC-listed in `<APP>.ASSETS`. The optional native payload lives at
`NATIVE/<APP>.P4X` (opaque data blob, CRC-covered; never executed as arbitrary
code — only the opted-in signed envelope of §4.1 may be launched by a `.bat`
shim). See [`docs/native_packaging.md`](docs/native_packaging.md) for the
packaging walk-through.

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

### 4.1 Opt-in signed native payload envelope (additive)

The existing reserved `NATIVE/<APP>.P4X` path is the **only** payload path.
An opted-in blob wraps its data in a fixed, little-endian envelope parsed by
the format-only core in `components/nativepay`. The tag stays `applib-1`; the
envelope is a new kind of *content* for the same blob, not a new executable
format or ABI version.

| Offset | Bytes | Field |
|--------|-------|-------|
| 0 | 8 | Magic `P4NATPAY` |
| 8 | 4 | Format version (u32, currently 1) |
| 12 | 8 | Arch id, NUL-padded: `esp32p4` |
| 20 | 16 | ABI tag, NUL-padded: `applib-1` (`P4_CONFIG_NATIVE_ABI`) |
| 36 | 4 | Payload size (u32, capped by `P4_CONFIG_NATPAY_MAX_PAYLOAD_BYTES`) |
| 40 | 4 | Payload CRC-32 (integrity check, the one shared `strutil_crc32_update`) |
| 44 | 32 | Payload SHA-256 |
| 76 | 64 | ECDSA P-256 signature, raw `r || s` |
| 140 | 2 | Entry ordinal (u16, 0-based index in the payload's export table) |
| 142 | 2 | Reserved, must be zero |
| 144 | 4 | Required API set (u32 bitmask: console, env, state, input, TUI, modal, db, time, webapp) |
| 148 | N | Payload bytes (`N` = the size field) |

**Canonical signed bytes.** The fixed header with **both** the 64-byte
signature field and the 32-byte SHA-256 field omitted, immediately followed by
the payload bytes (header prefix bytes 0..43 plus header suffix bytes 76..147
plus the payload). The 32-byte digest is SHA-256 over exactly that stream; the
publisher signs the digest. Magic, version, arch, ABI tag, size, CRC, entry
ordinal, and required API set are therefore bound by the signature; the SHA-256
and signature fields themselves are not. Including the SHA field in the signed
stream was circular and made signatures unverifiable; the verifier and
`tools/make_native_payload.py` now agree on the corrected stream. CRC-32 is a
cheap integrity check only and never substitutes for the signature.

**Trust gates are unchanged.** Install-time manifest CRC and ECDSA verdicts,
the single `p4sign` public key, and `P4_CONFIG_PKG_REQUIRE_SIGN` / `/signed`
keep their exact §4 semantics. The envelope signature is an additional
in-blob binding checked through hooks installed by the firmware; "no
verifier" or "no trusted key" is a distinct, non-success verdict, never an
implicit pass. A manifest BAD signature still refuses the bundle regardless
of the envelope.

**Launch.** The only front door is the ordinary `.bat` shim: it asks the one
loader path for the verified envelope, optionally around batch setup and a
webapp page (hybrid). There is no new verb, no second dispatcher, and no
execution of unsigned blobs. The pure parser never reads a file or transfers
control; the loader is a later hardware-verified step.

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
- Additive native payloads: a `.bat` shim may launch one signed envelope
  (pure native, or hybrid batch + native + webapp) while all existing batch,
  hybrid, and webapp flows keep working unchanged.
- Explicit degradation: an envelope with no verifier or no trust key is never
  reported as trusted.

**Explicitly not provided**

- Arbitrary SD/flash code execution (`dlopen`, dynamic linking, ELF relocation,
  per-app memory isolation). C apps are linked into the firmware; the `.BAT`
  shim is their front door. The opt-in payload is the sole, signed exception
  and still has no new executable format.
- A second dispatcher, second runtime/loader, or second package format —
  `app_register` + `launch`/`.bat` and the one payload loader path are the
  only model. No existing feature is removed to make room for it.
- Execution of unsigned `.P4X`/`.bin` blobs, or launch of a payload that fails
  the CRC, SHA-256, arch/ABI, or signature gates.
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
| Native payload envelope parse + verify (format only, no loader) | `components/nativepay/native_payload.h`, `native_payload.c` |
| Entry ABI + registry | `components/applib/applib_app.h`, `applib.c` |
| Registration at boot | `main/native_apps.c` (`native_apps_register`) |
| Dispatch + launch discovery | `components/command/command.c` (`shell_execute_command_core`, `shell_launch_*`) |
| Signature/trust core | `components/command/pkg_commands.c` (`pkg_sign_*`) |
| Manifest CRC verify | `components/command/asset_commands.c` (`asset_verify_app`) |
| Archive unpack (`.PKG`) | `components/command/pkg_commands.c` (`pkg_unpack_archive`, via `components/zip`) |
| Staged install + rollback | `components/command/pkg_commands.c` (pass 2, `PKGS/.installing/<APP>/`) |
| App-store client | `components/command/store_commands.c` (`store url|list|install`) |
| Host signer | `tools/pkg_sign.py` |
| Host bundle builder (`.PKG`) | `tools/make_pkg.py` |
| Host bundle push | `apps/push_pkgs.py` |
| Build-time guard | `_Static_assert` on the SDIO mirror (see `PORTING.md`) |

**Changing the ABI:** update this file, `P4_CONFIG_NATIVE_ABI`, the applib
headers, `docs/native_packaging.md`, and the changelog together; bump the tag
on any breaking change and say why in the changelog.
