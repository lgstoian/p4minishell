# miniaudio (host-only vendored copy)

This directory vendors the upstream **miniaudio** single-header library for two
concrete, hardware-free jobs in the shipped audio gate `tools/host_audio_gate.py`:

1. an independent WAV decode cross-check of the gate's committed PCM baselines;
2. an end-to-end **audio asset preparation** check: a 48 kHz / 24-bit source is
   decoded, resampled and converted into the exact WAV format the firmware's
   one SD WAV player accepts, then re-decoded and validated. miniaudio's
   built-in decoders cover WAV, MP3 and FLAC (Vorbis/Ogg needs an external
   backend and is deliberately not claimed).

The second job is exposed as a usable host CLI, `tools/audio_asset_prepare.py`.

## Origin and pinned version

- Upstream: <https://github.com/mackron/miniaudio>
- Version: **v0.11.26** (2026-10-31), tag `0.11.26`
- Pinned commit: `877ea4a7c2601c7b0f2a44e83dfac42e6470fea6`
- Files fetched verbatim from
  `https://raw.githubusercontent.com/mackron/miniaudio/877ea4a7c2601c7b0f2a44e83dfac42e6470fea6/`
  on 2026-11-27:
  - `miniaudio.h` — 4,161,918 bytes, SHA-256
    `6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e`
  - `LICENSE` — 2,597 bytes, SHA-256
    `457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb`

`git hash-object` of both files matches the GitHub Contents API blob SHA at the
pinned ref exactly: `miniaudio.h` → `b8f61372d97c88b54b97227a5d1d60902726b8e4`,
`LICENSE` → `2f9423addfe68aa01449ba885d56d832f04b5e8e`.

`miniaudio.h` is byte-for-byte upstream: its opening banner
(`miniaudio - v0.11.26 - 2026-10-31`, David Reid, <mackron@gmail.com>) and the
license block at the end of the file are preserved exactly. Do not edit it.

## Licence — verified from the upstream repository

Upstream offers a **choice of two licences**; both texts ship verbatim here in
`LICENSE` and are repeated at the end of `miniaudio.h`:

1. **Public Domain** (Unlicense, <http://unlicense.org/>), or
2. **MIT No Attribution (MIT-0)** — upstream `LICENSE` states
   `Copyright 2025 David Reid`; the licence block embedded in `miniaudio.h`
   states `Copyright 2026 David Reid`. Both are upstream's own words; we chose
   MIT-0 and preserve both upstream files unchanged rather than normalising
   the year.

There is no copyleft obligation. The exact notice to paste into `licence.md`
is recorded in the parent session's report; `licence.md` is owned by the parent
and is deliberately not edited here.

## Why host-only (measured, not assumed)

The ~290 KB ceiling is `tools/size_budget.py`'s **static DIRAM** budget, not
flash. A decoder-only upstream build for the ESP32-P4 RISC-V target
(`MA_NO_DEVICE_IO`, `MA_NO_THREADING`, `MA_NO_RUNTIME_LINKING`,
`MA_NO_ENCODING`; measured with `riscv32-esp-elf-size -A`) needs:

| Configuration | `.text` + `.rodata` (flash) | static DIRAM (`.data`/`.sbss`/`.sdata`) |
|---------------|----------------------------|------------------------------------------|
| all decoders  | 278,628 B                  | **736 B**                                |
| without WAV (MP3/FLAC/Vorbis only) | 237,624 B | **688 B**                    |

`sizeof(ma_decoder)` is 416 B on target, so static DIRAM and the 8 MB app
partition are **not** blockers in principle. We still keep miniaudio on the
host, for reasons that are not about size:

- The project rule is one audio path: one codec handle, one playback task, one
  mixer. Importing the 4.16 MB single header into firmware would duplicate the
  WAV parsing we already own and add a second decoder engine written in
  upstream style, not ours.
- Runtime behaviour (streaming from SD, PSRAM allocation, decoder stack, DMA
  timing) cannot be validated here: no board or serial port may be touched.
  Flash/DIRAM headroom is not evidence of audible correctness.
- The host tools give real value today without any of that risk: they convert
  assets the firmware cannot currently play into the format it accepts, and
  verify the PCM the shipped mixer produces.

`tools/host_audio_gate.py` enforces the boundary: its wiring audit rejects any
`miniaudio` reference in `components/`, `main/` or a firmware `CMakeLists.txt`.

## How it is wired into the shipped gate

`tools/host_audio_gate.py` compiles `ma_audio_host.c` once per process
(`-Wall -Wextra -Werror`) and uses it for both jobs:

- `decode <wav> <pcm>` decodes `tools/host/fixtures/audio/wav/*.wav` (generated
  from the literal `expect_left` values, never from the baselines) and compares
  the bytes to `baseline/*.hex`;
- `prepare <input> <rate> <channels> <pcm>` decodes any miniaudio-supported
  file, resamples/converts it, and `tools/audio_asset_prepare.py` writes a
  canonical WAV and validates it against `audio_wav_params_ok`'s rules.

A mismatch, a missing/corrupt fixture, a non-audio input or a wrong target rate
makes the gate red. Self-test canaries prove all of those (17 canaries total).

## How to run

From the repository root (Python 3, a C99 compiler such as gcc):

```powershell
python -m py_compile tools/host_audio_gate.py tools/audio_asset_prepare.py tools/thirdparty/miniaudio/make_wav_fixtures.py
python tools/thirdparty/miniaudio/make_wav_fixtures.py           # regenerate fixtures
gcc -std=gnu99 -Wall -Wextra -Werror -I tools/thirdparty/miniaudio tools/thirdparty/miniaudio/ma_audio_host.c -o ma_audio_host
python tools/audio_asset_prepare.py tools/host/fixtures/audio/assets/src_48k_24b_stereo.wav ready.wav
python tools/host_audio_gate.py --check                          # cross-check + conversion
python tools/host_audio_gate.py --self-test                      # 17 canaries
python tools/host_all_gates.py --only audio audio_canary         # shipped gate ids
```

The gate compiles the helper itself into a temporary directory; the standalone
`gcc` line above is only a strict-warning sanity check.

## Files here

| File | Role |
|------|------|
| `miniaudio.h` | Unmodified upstream v0.11.26 single header (implementation + declarations) |
| `LICENSE` | Unmodified upstream dual-licence text |
| `ma_audio_host.c` | Host driver with `decode` and `prepare` modes (project MIT) |
| `make_wav_fixtures.py` | Regenerates WAV + conversion fixtures (project MIT) |
| `README.md` | This file |
