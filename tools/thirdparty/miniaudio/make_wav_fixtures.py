#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Regenerate the miniaudio cross-check WAV fixtures for the host audio gate.

The WAVs are a SECOND, independently stored representation of the literal
``expect_left`` PCM in tools/host/fixtures/audio/scenarios.json: stereo s16,
both channels equal (the mixer is mono-to-stereo duplicated). They are NOT
produced from ``baseline/*.hex`` — the gate's miniaudio decoder must agree with
those baselines at run time, and a mismatch is a gate failure.

Usage: python tools/thirdparty/miniaudio/make_wav_fixtures.py
"""
from __future__ import annotations

import json
import re
import struct
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = ROOT / "tools" / "host" / "fixtures" / "audio"
SCENARIOS = FIXTURES / "scenarios.json"


def ident(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", value)


def write_conversion_fixtures() -> None:
    """Source material for the end-to-end asset-preparation gate check.

    48 kHz / 24-bit / stereo is deliberately NOT firmware-playable, so the
    gate must prove miniaudio resamples and converts it before the WAV reaches
    the existing SD player. The invalid file is a loud-failure canary.
    """
    assets = FIXTURES / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    frames = 480
    pcm = bytearray()
    for i in range(frames):
        left = int(6000000 * (1.0 if (i // 30) % 2 == 0 else -1.0))
        right = int(3000000 * (1.0 if (i // 60) % 2 == 0 else -1.0))
        for sample in (left, right):
            pcm += (sample & 0xFFFFFF).to_bytes(3, "little")
    with wave.open(str(assets / "src_48k_24b_stereo.wav"), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(3)
        wav.setframerate(48000)
        wav.writeframes(bytes(pcm))
    (assets / "invalid.wav").write_text(
        "this is not an audio file\n", encoding="utf-8", newline="\n")


def main() -> int:
    data = json.loads(SCENARIOS.read_text(encoding="utf-8-sig"))
    rate = int(data["sample_rate"])
    written = []
    for case in data["mixes"]:
        wav_rel = case.get("wav")
        if not wav_rel:
            continue
        left = case.get("expect_left")
        if not isinstance(left, list) or len(left) != int(case["frames"]):
            raise SystemExit("mix %s has no usable expect_left" % case["name"])
        pcm = b"".join(struct.pack("<hh", int(v), int(v)) for v in left)
        dest = FIXTURES / wav_rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(dest), "wb") as wav:
            wav.setnchannels(2)
            wav.setsampwidth(2)
            wav.setframerate(rate)
            wav.writeframes(pcm)
        written.append((str(dest.relative_to(ROOT)), len(pcm)))
    for path, size in written:
        print("wrote %s (%d PCM bytes)" % (path, size))
    if not written:
        raise SystemExit("no mix case declares a 'wav' fixture")
    write_conversion_fixtures()
    print("wrote conversion fixtures in tools/host/fixtures/audio/assets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
