#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""audio_asset_prepare.py - make host audio files playable by the ONE firmware decoder.

The firmware's `wavplay` accepts only 16-bit PCM mono/stereo WAV at 22050 or
44100 Hz (see `audio_wav_params_ok` in components/audio/audio.c). Real audio
assets are rarely in that exact format. This tool uses the host-only vendored
miniaudio decoder to decode its built-in formats (WAV/MP3/FLAC), resample it
and convert it to s16 at a firmware-accepted rate and
channel count, and write a canonical RIFF/WAVE that the existing SD player can
stream. It validates the result against the firmware's own acceptance rules
and fails loudly on bad input. It does not touch the firmware audio path.

Usage:
    python tools/audio_asset_prepare.py INPUT.wav OUTPUT.wav \
        [--rate 22050|44100] [--channels 1|2] [--cc gcc]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path
from typing import Dict, Optional

ROOT = Path(__file__).resolve().parent.parent
THIRDPARTY = ROOT / "tools" / "thirdparty" / "miniaudio"
SOURCE = THIRDPARTY / "ma_audio_host.c"
ALLOWED_RATES = (22050, 44100)
RESULT_RE = re.compile(
    r"^ma\.prepare channels=(\d+) rate=(\d+) frames=(\d+) bytes=(\d+)$")


class PrepareError(RuntimeError):
    """Any condition that must make asset preparation fail."""


def compile_tool(cc: str, work: Path) -> Path:
    """Build the host-only miniaudio helper with strict warnings."""
    if not SOURCE.is_file() or not (THIRDPARTY / "miniaudio.h").is_file():
        raise PrepareError("vendored miniaudio is missing")
    binary = work / ("ma_audio_host.exe" if os.name == "nt"
                     else "ma_audio_host")
    cmd = [cc, "-std=gnu99", "-Wall", "-Wextra", "-Werror",
           "-I" + str(THIRDPARTY), str(SOURCE), "-o", str(binary)]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          cwd=str(ROOT), timeout=180)
    if proc.returncode != 0 or not binary.is_file():
        raise PrepareError("miniaudio helper compile failed (rc=%d): %s"
                           % (proc.returncode,
                              (proc.stdout + proc.stderr)[-1200:]))
    return binary


def validate_wav(path: Path, channels: int, rate: int,
                 frames: int) -> Dict[str, object]:
    """Parse the RIFF/WAVE exactly like the firmware chunk scanner and apply
    the same acceptance predicate as audio_wav_params_ok()."""
    raw = path.read_bytes()
    if len(raw) < 44 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise PrepareError("output is not RIFF/WAVE")
    if struct.unpack_from("<I", raw, 4)[0] + 8 != len(raw):
        raise PrepareError("RIFF size field disagrees with file length")

    fmt: Optional[tuple] = None
    data: Optional[bytes] = None
    pos = 12
    while pos + 8 <= len(raw):
        tag = raw[pos:pos + 4]
        size = struct.unpack_from("<I", raw, pos + 4)[0]
        body = raw[pos + 8:pos + 8 + size]
        if len(body) != size:
            raise PrepareError("truncated %r chunk" % tag)
        if tag == b"fmt " and size >= 16 and fmt is None:
            fmt = struct.unpack_from("<HHIIHH", body, 0)
        elif tag == b"data" and data is None:
            data = body
        pos += 8 + size + (size & 1)

    if fmt is None or data is None:
        raise PrepareError("output has no fmt/data chunk")
    audio_format, nch, srate, _byte_rate, _align, bits = fmt
    if not (audio_format == 1 and bits == 16 and nch in (1, 2) and
            srate in ALLOWED_RATES and len(data) != 0):
        raise PrepareError("output violates firmware audio_wav_params_ok")
    if (nch, srate) != (channels, rate):
        raise PrepareError("output format drifted from request")
    if len(data) != frames * channels * 2:
        raise PrepareError("data chunk size disagrees with decoded frames")
    return {"format": audio_format, "channels": nch, "rate": srate,
            "bits": bits, "frames": frames,
            "bytes": len(data), "sha256": hashlib.sha256(raw).hexdigest()}


def prepare(source: Path, dest: Path, rate: int, channels: int,
            cc: Optional[str] = None, work: Optional[Path] = None,
            tool: Optional[Path] = None) -> Dict[str, object]:
    """Decode/convert `source` into a firmware-playable WAV at `dest`."""
    source = Path(source).resolve()
    dest = Path(dest).resolve()
    if not source.is_file():
        raise PrepareError("input is missing: %s" % source)
    if rate not in ALLOWED_RATES or channels not in (1, 2):
        raise PrepareError("unsupported target format: %d Hz / %d ch"
                           % (rate, channels))

    temp = tempfile.TemporaryDirectory(prefix="audio-prepare-") if work is None else None
    folder = Path(temp.name) if temp is not None else Path(work)
    try:
        binary = Path(tool) if tool is not None else compile_tool(
            cc or os.environ.get("CC") or shutil.which("gcc") or "gcc", folder)
        raw_pcm = folder / "prepared.pcm"
        proc = subprocess.run(
            [str(binary), "prepare", str(source), str(rate), str(channels),
             str(raw_pcm)],
            capture_output=True, text=True, cwd=str(folder), timeout=60)
        if proc.returncode != 0:
            raise PrepareError("miniaudio could not prepare %s (rc=%d): %s"
                               % (source.name, proc.returncode,
                                  (proc.stdout + proc.stderr)[-600:]))
        lines = [line.strip() for line in proc.stdout.splitlines()
                 if line.strip()]
        match = RESULT_RE.match(lines[-1]) if lines else None
        if match is None:
            raise PrepareError("malformed miniaudio prepare result: %s"
                               % (proc.stdout + proc.stderr)[-400:])
        got_ch, got_rate, frames, size = (int(part) for part in match.groups())
        if (got_ch, got_rate) != (channels, rate) or frames == 0:
            raise PrepareError("miniaudio returned a wrong format")
        if size != frames * channels * 2:
            raise PrepareError("miniaudio PCM byte count is inconsistent")
        pcm = raw_pcm.read_bytes()
        if len(pcm) != size or not any(pcm):
            raise PrepareError("decoded PCM is empty or all silent")

        dest.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(dest), "wb") as wav:
            wav.setnchannels(channels)
            wav.setsampwidth(2)
            wav.setframerate(rate)
            wav.writeframes(pcm)
        info = validate_wav(dest, channels, rate, frames)
        info["pcm"] = pcm
        return info
    finally:
        if temp is not None:
            temp.cleanup()


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--rate", type=int, default=22050,
                        choices=ALLOWED_RATES)
    parser.add_argument("--channels", type=int, default=2, choices=(1, 2))
    parser.add_argument("--cc", default=None)
    args = parser.parse_args(argv)
    try:
        info = prepare(args.input, args.output, args.rate, args.channels,
                       cc=args.cc)
    except (PrepareError, OSError) as exc:
        print("AUDIO PREPARE FAIL: %s" % exc, file=sys.stderr)
        return 1
    print("audio.prepare ok: %s -> %s (%d Hz, %d ch, %d frames, %d bytes, "
          "sha256=%s)" % (args.input.name, args.output.name, info["rate"],
                          info["channels"], info["frames"], info["bytes"],
                          info["sha256"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
