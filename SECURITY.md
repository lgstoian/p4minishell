# Security Policy

How P4MiniShell protects secrets, what the device lock actually enforces,
and what is still missing. Read this before exposing a device to an
untrusted network.

- **Version:** v1.3.1 · **License:** MIT (see [`licence.md`](licence.md))
- Related docs: [`command.md`](command.md) (`security`, `crypt`, `httpd`,
  `pkg`), [`ABI.md`](ABI.md) (package signing), [`readme.md`](readme.md),
  [`roadmap.md`](roadmap.md) (platform/security row).

## Threat model

P4MiniShell is a single-user hobby firmware. It assumes:

- the **owner holds the hardware** (USB serial = full control by design);
- the **SD card is portable storage**, readable on any host — there is no
  full-disk encryption;
- the **LAN may be hostile** once Wi-Fi is connected (`httpd`, `tcpterm`,
  `net` MQTT without `net tls on`).

It defends against casual snooping (shoulder surfing, shared photos of the
screen, a borrowed device) and against remote LAN access to the file server.
It does **not** defend against a lab attacker with the SD card in hand.

## Reporting a vulnerability

Open a GitHub issue on this repository with `[security]` in the title,
including the firmware version (`version` command), steps to reproduce, and
the impact you see. Do not include real credentials or private data in the
report. There is no bug-bounty program.

## Device lock (`security` / `owner`)

- No lock exists until you set one: there is **no default passcode** and
  the dispatcher is open on first boot.
- `security setpass` stores only a **PBKDF2-SHA256 salted hash (hex)** in
  `sd:/CONFIG.SYS` (`SECURITY_*` lines) — never cleartext
  (`components/command/security_commands.c`).
- `SECURITY_BOOTLOCK=on` locks the dispatcher at boot and on auto-lock.
  While locked, only `security`, `unlock`, `help`, `cls`, `clear`,
  `version`/`ver`, `about` run; everything else is refused with
  `Device locked - run 'security unlock' first`.
- `security conceal` controls private-record visibility; `db /reveal`,
  `gfind`, `export`, `edit db`, and `pim get db` refuse secret payloads
  while locked
  (`security_can_reveal_private()` in `security_commands.h`). Once unlocked,
  `pim get` includes secrets like `export` does; `pim put` preserves each
  record's existing secret flag and never clears it.
- **Recovery is physical by design:** delete the `SECURITY_*` lines from
  `CONFIG.SYS` on any host, or run `config factory` (interactive `YES`
  confirmation). Anyone holding the SD card can do this — that is the
  documented recovery path, not a bypass bug.

## Secret handling

- Wi-Fi passwords are **never echoed**: masked in the transcript, excluded
  from command history (`shell_command_should_store_history()`), and never
  written to the debug log.
- `ask /p`, `set /p /P`, and `app_read_password()` read without echo.
- `crypt lock|unlock` is AES-256-GCM under a PBKDF2-SHA256 key, streamed in
  512-byte chunks (hardware esp-aes over cache-aligned DMA buffers, with a
  self-contained software fallback in `components/swgcm/` in the identical
  format — bugs.md F23, fixed in v1.2.1); key and password buffers are
  **zeroed after every run**, and `/p:` passwords are masked like Wi-Fi
  credentials (`components/command/crypt_commands.c`).
- Per-file encryption only: `crypt` protects chosen files, not the card.
- The MQTT broker password (`net password`) is read hidden, never echoed,
  and its buffer is **zeroed after every run**; like the Wi-Fi known list it
  is stored on the SD card (`sd:/NET.INI`), readable on any host — the
  documented recovery trade, not a bypass. The default transport is plaintext
  MQTT (trusted LAN only); `net tls on` switches to MQTT over TLS using the
  `httpget` CA policy (user CA store when loaded, Mozilla bundle otherwise).
  CardDAV passwords (`pim net ... /p:`) are history-masked and zeroed after
  the run the same way.

## Package signing (`pkg` / `pkg key`)

- Installed bundles are integrity-checked by CRC-32 and may be
  authenticity-checked by an ECDSA P-256 `SIGN=` line (`ABI.md` §4).
- The **private key never touches the device**: sign on the host with
  `tools/pkg_sign.py`; the device trusts only a raw public key installed via
  `pkg key install` (stored in the `p4sign` NVS namespace). `pkg key install`
  rejects an off-curve point; `pkg key clear` confirms first.
- A **bad signature always refuses** the install, even when every CRC matches
  (CRCs are attacker-recomputable). Unsigned bundles keep the CRC-only
  contract unless `P4_CONFIG_PKG_REQUIRE_SIGN=1` or `pkg install /signed`
  enforces signing.
- There is no revocation list and no online trust: rotating the key means
  re-signing bundles and running `pkg key install` again. Anyone with USB
  serial control (which equals full control by design) can replace the trusted
  key.

## HTTP file server (`httpd`)

- `httpd` serves SD files over the LAN and **auto-starts when the station
  gets an IP** (`P4_CONFIG_HTTPD_AUTOSTART=1`).
- Basic auth is **on by default** with the compile-time credential
  `admin` / `p4mini` (`P4_CONFIG_HTTPD_AUTH_USERNAME` /
  `P4_CONFIG_HTTPD_AUTH_PASSWORD` in `p4minishell_config.h`). There is no
  runtime command to change it: edit the header (empty username disables
  auth) and rebuild before putting a device on an untrusted network.
- `httpd status` reports whether auth is on.

## Destructive actions are gated

`format`, `disk clean`/`delete`, recursive `del`/`rd /s`, `trash empty`,
`config factory`, and `c6ota` require the exact confirmation word
(`P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD`, default `YES`) collected through the
key queue, and **refuse when no interactive input source is attached**, so a
batch file can never trigger them unattended. `chkdsk` is read-only by
design — the firmware never rewrites FAT structures.

Taps never execute anything directly: transcript hyperlinks (`link`) only
arm a preview naming the exact target, and the second tap on the same target
runs it. `anchor` regions (batch-authored, immediate-run) predate this rule
and keep their contract; prefer `link` for anything destructive.

## Transport notes

- HTTPS client (`httpget https://…`) uses mbedTLS against the user CA store
  (`sd:/CERTS/`, managed by `certs`) plus the compiled-in Mozilla bundle.
  Custom CAs go in via `certs add` (PEM, or DER validated + converted).
- C6 OTA requires the explicit `YES` confirmation, refuses while background
  jobs run, and restores Wi-Fi afterwards.
- SoftAP is station-only except the captive portal (`wifi setup on`, open AP
  `P4MiniShell-Setup` by config, TX-capped) and **board-to-board chat** (`chat host on`, open AP `P4MiniShell-Chat` by config, TX-capped); there is no open AP by default.
- Secure boot / flash encryption: no provisioned profile ships in v1.3.0
  (see `roadmap.md` platform row). Enable them via your own ESP-IDF signing
  flow if your deployment needs them.

## Chat transport notes

- `chat host on` starts an **open SoftAP** (`P4MiniShell-Chat`, TX-capped to
  13 dBm / 52 qdBm). Any nearby device can join and send HTTP `POST /chat`
  messages. **No authentication or encryption** — the link is plain HTTP over
  the local AP link. Do not use on untrusted physical premises.
- `chat peer <host-ip>` joins the host AP via `wifi connect`; the peer is
  also on an open network.
- **Read-only scope:** the protocol has no remote command execution. Only
  text messages (`chat send` / `chat poll` / `chat log`) and the optional
  `/onmsg` hook (runs a local batch line with `CHAT_FROM`, `CHAT_TEXT`,
  `CHAT_LEN` env vars) are supported. No reverse shell, no `exec`.
- **Opt-in exception:** `chat host on` temporarily lifts the STA-only Wi-Fi
  rule. `chat host off` restores STA-only mode immediately.
- `chat onmsg <line>` runs a local batch line on message receive; the line
  should be audited like any batch code. `chat onmsg off` disables it.

## EPUB transport notes

- `epub open <file.epub>` opens a **ZIP-based EPUB archive** with an internal
  SoftAP-free reader. No network exposure.
- **Read-only scope:** the protocol has no remote command execution. Only
  local rendering (`epub read`), navigation (`next`/`prev`/`goto`/`toc`),
  and resource extraction (`epub extract`) are supported.
- **No network exposure:** EPUB files are local ZIP archives; no network
  interfaces are opened or exposed.
- **Opt-in extraction:** `epub extract <n> <file>` writes resources (images,
  CSS, fonts) to SD card; the source archive is never modified.

## Known gaps (not bugs)

- Physical SD access defeats the device lock (recovery path above).
- Default `httpd` credential until you rebuild with your own.
- No audit log of unlock attempts; no wipe-after-N-failures.
- `spi` transactions and touch-wake are unavailable on this board and fail
  loudly rather than silently (see `roadmap.md` open gaps).
