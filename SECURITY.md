# Security Policy

How P4MiniShell protects secrets, what the device lock actually enforces,
and what is still missing. Read this before exposing a device to an
untrusted network.

- **Version:** v1.1.0 · **License:** MIT (see [`licence.md`](licence.md))
- Related docs: [`command.md`](command.md) (`security`, `crypt`, `httpd`),
  [`readme.md`](readme.md), [`roadmap.md`](roadmap.md) (platform/security row).

## Threat model

P4MiniShell is a single-user hobby firmware. It assumes:

- the **owner holds the hardware** (USB serial = full control by design);
- the **SD card is portable storage**, readable on any host — there is no
  full-disk encryption;
- the **LAN may be hostile** once Wi-Fi is connected (`httpd`, `tcpterm`).

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
  `gfind`, and `export` refuse secret payloads while locked
  (`security_can_reveal_private()` in `security_commands.h`).
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
  4 KB chunks; key and password buffers are **zeroed after every run**, and
  `/p:` passwords are masked like Wi-Fi credentials
  (`components/command/crypt_commands.c`).
- Per-file encryption only: `crypt` protects chosen files, not the card.

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

## Transport notes

- HTTPS client (`httpget https://…`) uses mbedTLS. There is no SD
  certificate store yet (see `roadmap.md` TLS row); pinning custom CAs is
  not supported in v1.1.0.
- C6 OTA requires the explicit `YES` confirmation, refuses while background
  jobs run, and restores Wi-Fi afterwards.
- SoftAP is compiled out (station-only); there is no open AP by default.
- Secure boot / flash encryption: no provisioned profile ships in v1.1.0
  (see `roadmap.md` platform row). Enable them via your own ESP-IDF signing
  flow if your deployment needs them.

## Known gaps (not bugs)

- Physical SD access defeats the device lock (recovery path above).
- Default `httpd` credential until you rebuild with your own.
- No audit log of unlock attempts; no wipe-after-N-failures.
- `spi` transactions and touch-wake are unavailable on this board and fail
  loudly rather than silently (see `roadmap.md` open gaps).
