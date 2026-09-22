# Native-App Packaging Spec (v1.2 — hybrid launch + signed manifests)

How an SD-deployable app is described, verified, installed, and launched with
the `pkg` system. **Every** app launches through a `*.bat` file; the work
behind the shim is pure batch, a linked-in C entry (hybrid), or a
native-linked entry reached directly (native). There is still **no SD code
execution** — the `.P4X`/`.bin` payload is a verified data blob (see
[`ABI.md`](../ABI.md) §1). This document is the packaging walk-through; the
frozen contract is `ABI.md`.

- **Packaging spec:** v1.2 (firmware v1.2.1) · **Target:** ESP32-P4 + ESP32-C6
- Related: [`ABI.md`](../ABI.md) (the contract), [`command.md`](../command.md)
  (`pkg`, `asset`, `launch`), [`SDK.md`](../SDK.md) (applib ABI),
  `tools/pkg_sign.py`, `apps/push_pkgs.py`.

## 1. Bundle layout

```
PKGS/<APP>/
  <APP>.APPINFO      INI metadata (see §2)
  <APP>.ASSETS       path=HEXCRC lines + optional SIGN= line (see §4)
  <APP>.BAT          the launch shim (batch and hybrid apps)
  ...                optional payloads (data, native blob, docs)
```

Payload convention: an optional native blob lives at `NATIVE/<APP>.P4X`
(uppercase app name, matching `asset_app_ok`). Every file must be
manifest-listed or install aborts. `pkg install <APP>` copies each payload to
its install-relative path and the `.APPINFO`/`.ASSETS` into `APPS/`.

## 2. APPINFO keys

| Key | Required | Meaning |
|---|---|---|
| `title`, `description`, `version` | yes | As for batch apps |
| `type` | no | `batch` (default when absent), `hybrid`, or `native`; anything else aborts `pkg install` loudly |
| `abi` | hybrid/native | `applib-1`; must equal `P4_CONFIG_NATIVE_ABI` (mismatch warns at install, refuses at execution) |
| `arch` | hybrid/native | `esp32p4` |
| `entry` | hybrid/native | C entry name the shim/dispatcher invokes; hybrid bundles MUST set it |

```ini
title=Hello Native
description=Batch shim driving the linked hello entry
version=1.0
type=hybrid
abi=applib-1
arch=esp32p4
entry=hello
```

## 3. Install / verify / remove semantics (v1.2)

- `pkg install <app> [/signed]`: two-pass flow (verify-then-copy over
  `shell_fs_copy_file`). Pass 1 checks CRCs **and** the manifest trust verdict
  (see §4); a BAD signature aborts before any copy. Pass 2 copies payloads,
  then metadata + manifest.
  - `type=batch` installs as before.
  - `type=hybrid` requires `entry=`; install reports
    `pkg: <app> is hybrid (launch <app> runs <app>.BAT into <entry>)`.
  - `type=native` reports the store-only note; the blob is verified and in
    place but runs only if linked into the firmware.
  - Unknown `type=` aborts before any copy (ERRORLEVEL 1).
- `pkg verify|check|info|list`: unchanged mechanics; `list`/`info` show the
  `type` and the `signed:` line; hybrid/native are flagged.
- `pkg remove <app>`: trashes payloads + metadata like batch apps.
- `launch`: discovers `*.bat`, so batch and hybrid apps appear; hybrid shims
  are tagged ` [hybrid]` in `/list` and the menu. Native-only entries are
  reached by name (dev/test) and are not offered by `launch`.

## 4. Trust model (CRC + ECDSA P-256)

The manifest is integrity-checked by CRC and may be authenticity-checked by a
`SIGN=<128 hex>` line (raw `r||s`) over the canonical manifest bytes with a
trusted raw P-256 public key in the `p4sign` NVS store (`pkg key ...`). Bad
signatures always refuse; unsigned bundles keep the CRC-only contract unless
`P4_CONFIG_PKG_REQUIRE_SIGN=1` or `/signed` demands otherwise. Full rules,
canonicalization, and the enforcement matrix are in [`ABI.md`](../ABI.md) §4.

Host tooling:

```powershell
python tools/pkg_sign.py keygen --out mykey     # mykey.priv.pem + mykey.pub.bin
python apps/push_pkgs.py COM3 --sign mykey.priv.pem
# push mykey.pub.bin and, on the device:
pkg key install mykey.pub.bin
pkg install MYAPP /signed
```

## 5. Future: true native execution

Running unmodified SD code needs a dynamic linker/relocator and per-app
memory protection, which this bare-metal ESP-IDF target does not provide
(`ABI.md` §5). Until then, C apps are linked into the firmware and reach users
through a `.BAT` shim (hybrid). The `NATIVE/<APP>.P4X` path and the metadata
keys are chosen so today's bundles stay valid if such a loader ever lands.
