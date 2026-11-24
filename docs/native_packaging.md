# Native-App Packaging Spec (v1.3 — ZIP bundles + staged installs)

How an SD-deployable app is described, verified, installed, and launched with
the `pkg` system. **Every** app launches through a `*.bat` file; the work
behind the shim is pure batch, a linked-in C entry (hybrid), or a
native-linked entry reached directly (native). There is still **no arbitrary
SD code execution** — the `.P4X`/`.bin` payload is a verified data blob, with
one additive opt-in exception: a signed envelope may be launched by a `.bat`
shim through the single loader path (see [`ABI.md`](../ABI.md) §1 and §4.1).
This document is the packaging walk-through; the frozen contract is `ABI.md`.

- **Packaging spec:** v1.3 (firmware v1.5.0) · **Target:** ESP32-P4 + ESP32-C6
- Related: [`ABI.md`](../ABI.md) (the contract), [`command.md`](../command.md)
  (`pkg`, `asset`, `store`, `launch`), [`SDK.md`](../SDK.md) (applib ABI),
  `tools/pkg_sign.py`, `tools/make_pkg.py`, `apps/push_pkgs.py`.

## 1. Bundle layout

A bundle is either a **directory** or a **single-file archive**:

```
PKGS/<APP>/                 (directory bundle)
  <APP>.APPINFO      INI metadata (see §2)
  <APP>.ASSETS       path=HEXCRC lines + optional SIGN= line (see §4)
  <APP>.BAT          the launch shim (batch and hybrid apps)
  ...                optional payloads (data, native blob, docs)

PKGS/<APP>.PKG              (archive bundle: a ZIP container)
  <APP>/<APP>.APPINFO
  <APP>/<APP>.ASSETS
  <APP>/<path>       each payload at its install-relative path
```

The `.PKG` entries mirror the directory layout; the leading `<APP>/` is
optional (an archive whose entries sit at the root unpacks the same way).
`pkg install <APP>` prefers the directory bundle and falls back to the archive,
unpacking it into `PKGS/<APP>/` through the **ONE** ZIP reader
(`components/zip`); `pkg unpack <APP>` does only the unpack. `pkg scan [dir]`
lists both forms. Host tool: `tools/make_pkg.py`.

Payload convention: an optional native blob lives at `NATIVE/<APP>.P4X`
(uppercase app name, matching `asset_app_ok`). Every file must be
manifest-listed or install aborts. `pkg install <APP>` copies each payload to
its install-relative path and the `.APPINFO`/`.ASSETS` into `APPS/`.

## 2. APPINFO keys

| Key | Required | Meaning |
|---|---|---|
| `title`, `description`, `version` | yes | As for batch apps (`version` compares numerically for `outdated`/downgrade) |
| `type` | no | `batch` (default when absent), `hybrid`, or `native`; anything else aborts `pkg install` loudly |
| `abi` | hybrid/native | `applib-1`; must equal `P4_CONFIG_NATIVE_ABI` (mismatch warns at install, refuses at execution) |
| `arch` | hybrid/native | `esp32p4` |
| `entry` | hybrid/native | C entry name the shim/dispatcher invokes; hybrid bundles MUST set it |
| `handles`, `setup`, `autostart`, `alias`, `data`, `requires`, `min_fw` | no | Registration keys, same as batch apps (see [`ABI.md`](../ABI.md) §3) |
| `payload` | no | Opt-in native payload blob path (`NATIVE/<APP>.P4X`), relative to the bundle. Additive metadata; launch only by the `.bat` shim via the one loader path (§2.1) |
| `alpha`, `scrim` | no | Opt-in TUI/modal transparency (v1.4.3), 0-255 or `0-100%`; applied by `launch`, carried by `pkg` as ordinary metadata |
| `services` | no | Comma-separated dependency list (`APP>=1.2`, `APP=2`, `APP>3`, or a bare `APP` = any version). Enforced at install: every declared service must be installed and satisfy its version. Reported by `pkg info`. Additive metadata — no ABI bump. |

```ini
title=Hello Native
description=Batch shim driving the linked hello entry
version=1.0
type=hybrid
abi=applib-1
arch=esp32p4
entry=hello
```

## 2.1 Opt-in signed payload workflow (additive, no ABI bump)

The reserved `NATIVE/<APP>.P4X` path stays the one payload path. An app that
needs native code keeps the same single front door — its `.bat` shim:

1. `payload=NATIVE/<APP>.P4X` names the blob. It is manifest-listed and
   CRC-covered like every other payload, and installs into `APPS/`.
2. The blob carries the envelope in [`ABI.md`](../ABI.md) §4.1: a fixed header
   (magic, version, `esp32p4`, `applib-1`, size, CRC-32, SHA-256, `r||s`
   signature, entry ordinal, required API set) followed by the payload bytes.
   The host signer signs SHA-256 over the header without the signature field
   plus the payload; the firmware's format-only core
   (`components/nativepay`) checks the header and CRC and delegates SHA-256
   and ECDSA to hooks installed at boot.
3. The shim asks the **one** firmware loader path for the verified envelope.
   A **pure native** app hands off directly; a **hybrid** app runs batch
   setup, the native payload, and a webapp page (or any other batch step) from
   the same shim. There is no new extension, no new `type=` value, and no
   second dispatcher.
4. Trust gates are unchanged: install-time manifest CRC and the `SIGN` verdict
   plus the single `p4sign` key remain authoritative. A missing verifier or
   trust key is an explicit non-success verdict, never a pass. Unsigned blobs
   and arbitrary `.bin` files are never launched.

Only the envelope parser/verifier exists today; the loader and its launch
surface are a later, hardware-verified step ([`roadmap.md`](../roadmap.md)).
Pure-batch, hybrid, and webapp apps are not modified or removed.

### Multitasking and app switching (design credit only)

[BMDS](https://github.com/jthwho/bmds) (MIT, © 2026 Jason Howard) and
[ESP-Brookesia](https://github.com/espressif/esp-brookesia) (Apache-2.0) were
studied for how small handhelds switch between a native app, a document/web
view, and the shell without a desktop-style process model. The ideas are
re-expressed here with facilities we already own: the `start` pool and task
manager for background work, sparse per-job environment overlays for app
context, `himem` quotas for ownership, the shared modal/browser surfaces for
the webapp half of a hybrid, and `.bat` shims for the front door. No code,
scheduler, app manager, or runtime was copied or added; see
[`licence.md`](../licence.md) for the design-credit table.

## 3. Install / verify / remove semantics (v1.3)

- `pkg install <app> [/signed] [/force]`: version/dependency gates, then the
  two-pass flow. Pass 1 checks CRCs **and** the manifest trust verdict (see
  §4); a BAD signature aborts before any copy. Downgrades refuse without
  `/force`; `requires=`/`min_fw=`/`services=` always hold.
- **Staged, atomic install.** Pass 2 builds the full operation list (payloads,
  then `.APPINFO`, then `.ASSETS`), backs up every existing destination under
  `PKGS/.installing/<APP>/`, then installs each file atomically (copy to
  `<dst>.tmp`, then `storage_replace_file()` rename). If any step fails the
  backups are restored and the staging slot is removed, so a failed install
  leaves the previous version intact. Backups are best-effort: a file that
  cannot be backed up is still installed (the backup is a safety net, not a
  gate).
- `pkg unpack <app>`: unpack `PKGS/<APP>.PKG` into `PKGS/<APP>/` only.
- `pkg scan [dir]`: list `.PKG` archives and bundle directories (default
  `PKGS/`).
  - `type=batch` installs as before.
  - `type=hybrid` requires `entry=`; install reports
    `pkg: <app> is hybrid (launch <app> runs <app>.BAT into <entry>)`.
  - `type=native` reports the store-only note; the blob is verified and in
    place but runs only if linked into the firmware.
  - Unknown `type=` aborts before any copy (ERRORLEVEL 1).
- `pkg verify|check|info|list`: unchanged mechanics; `list`/`info` show the
  `type` and the `signed:` line; hybrid/native are flagged.
- `pkg remove <app>`: trashes payloads + metadata like batch apps (app data
  kept with a `purge` hint); `pkg purge <app>` removes those too; `pkg outdated`
  flags staged newer bundles.
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

## 5. App store (`store` verb)

`store url | list | install <id>` is a thin remote-app-store client over the
shared networking stack (`store_commands.c`). The index URL is the compile-time
`P4_CONFIG_STORE_INDEX_URL` (empty disables the verb). The index is a JSON
`apps[]` document:

```json
{ "apps": [ { "package_name": "demo", "latest_version": "1.0",
              "app_name": {"en": "Demo"},
              "bpk_url": "https://host/demo.PKG" } ] }
```

`store list` fetches and prints `id | version | name`; `store install <id>`
streams the entry's `bpk_url` to `PKGS/<id>.PKG` (via
`networking_http_get_stream()`, no second HTTP client) and then installs it
through the ordinary `pkg install` path. There is no second package format, no
second JSON parser (`json_walk()` is the ONE reader), and no second
dispatcher.

## 6. Future: true native execution

The opt-in envelope (§2.1) adds a launch route for one *signed, verified*
payload; it does not make arbitrary SD code executable. Running unmodified
code still needs a dynamic linker/relocator and per-app memory protection,
which this bare-metal ESP-IDF target does not provide (`ABI.md` §5). Until
the single loader path is implemented, C apps are linked into the firmware and
reach users through a `.BAT` shim (hybrid). The `NATIVE/<APP>.P4X` path and the
metadata keys were chosen so today's bundles stay valid when it lands — no
parallel package format, no second loader, and no removal of existing
features.
