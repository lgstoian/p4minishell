#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_audio_gate.py - hardware-free audio engine / PCM render gate.

The gate generates a C harness from
``tools/host/fixtures/audio/scenarios.json`` and compiles it with the REAL
pure translation unit the firmware mixes with (``components/audio/
audio_engine.c``). Python never re-implements the mixer, ADSR, sequencer or
meter: every number is a literal fixture expectation transcribed into C and
asserted against shipped code, natively, with no board, codec or FreeRTOS.

The harness also dumps the rendered stereo PCM so the gate can compare it
byte-for-byte with the committed ``baseline/*.hex`` files. Those baselines are
then cross-checked against WAV fixtures decoded by the vendored, HOST-ONLY
miniaudio library (``tools/thirdparty/miniaudio``); a mismatch is a gate
failure. The same miniaudio helper also runs an end-to-end asset-preparation
check: a 48 kHz/24-bit source is resampled/converted to the exact format the
firmware's one SD WAV player accepts, then re-decoded and validated against
``audio_wav_params_ok``; invalid input must fail loudly. miniaudio never enters
the firmware or ESP-IDF build. A wiring audit rejects a second codec handle, a
duplicated speaker init, a second audio task and any firmware reference to
miniaudio; the engine must feed the ONE existing playback path.

A pass is a hardware-free pre-gate only. It says nothing about on-target
audibility; ``harness.md`` §0 still governs hardware sign-off.

Usage:
    python tools/host_audio_gate.py              # --check (default)
    python tools/host_audio_gate.py --check
    python tools/host_audio_gate.py --fixture PATH
    python tools/host_audio_gate.py --cc gcc
    python tools/host_audio_gate.py --self-test  # red canaries
"""
from __future__ import annotations

import argparse
import atexit
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tools" / "host" / "fixtures" / "audio" / "scenarios.json"
AUDIO_DIR = ROOT / "components" / "audio"
PURE_SRC = AUDIO_DIR / "audio_engine.c"
EXTRA_INCLUDES = [AUDIO_DIR, ROOT]
MIN_CHECKS = 40
RUN_TIMEOUT = 60
MINIAUDIO_COMPILE_TIMEOUT = 180

THIRDPARTY_DIR = ROOT / "tools" / "thirdparty" / "miniaudio"
MINIAUDIO_SRC = THIRDPARTY_DIR / "ma_audio_host.c"
ASSET_FIXTURES = FIXTURE.parent / "assets"
MINIAUDIO_HEADER = THIRDPARTY_DIR / "miniaudio.h"
FIXTURE_WAVS = FIXTURE.parent / "wav"

BEGIN = "audio.gate.begin"
END = "audio.gate.end"
SUMMARY = "audio.gate.summary"
CHECK_RE = re.compile(r"^audio\.check\s+([A-Za-z0-9_.]+)\s+(ok|FAIL)$")
SUMMARY_RE = re.compile(r"^audio\.gate\.summary\s+checks=(\d+)\s+fail=(\d+)$")
DECODER_RE = re.compile(
    r"^ma\.decoder channels=(\d+) rate=(\d+) frames=(\d+) bytes=(\d+)$")

BUSES = {"music": "AUDIO_ENGINE_BUS_MUSIC", "sfx": "AUDIO_ENGINE_BUS_SFX"}


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


def cstr(value: str) -> str:
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def ident(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", value)


def load_fixture(path: Path) -> Dict[str, Any]:
    if not path.is_file():
        raise GateError("fixture is missing: %s" % path)
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError("fixture unreadable: %s" % exc)
    tag = data.get("tag")
    if not isinstance(tag, str) or not tag:
        raise GateError("fixture has no tag")
    for group in ("parsers", "mixes", "sequence", "bounded", "meter",
                  "offline"):
        if not isinstance(data.get(group), (list if group in
                                            ("parsers", "mixes") else dict)):
            raise GateError("fixture group %r is missing" % group)
        value = data[group]
        if isinstance(value, (list, dict)) and not value:
            raise GateError("fixture group %r is vacuous" % group)
    envs = data.get("envs")
    if not isinstance(envs, dict) or not envs:
        raise GateError("fixture has no envelopes")
    for name, env in envs.items():
        for field in ("attack_ms", "decay_ms", "sustain_pct", "release_ms"):
            if field not in env:
                raise GateError("env %s lacks %r" % (name, field))
    for i, case in enumerate(data["parsers"]):
        for field in ("name", "kind", "input", "expect_ok"):
            if field not in case:
                raise GateError("parser case %d lacks %r" % (i, field))
        if case["kind"] not in ("env", "seq"):
            raise GateError("parser case %d has unknown kind %r"
                            % (i, case["kind"]))
    for i, case in enumerate(data["mixes"]):
        for field in ("name", "frames", "channels"):
            if field not in case:
                raise GateError("mix case %d lacks %r" % (i, field))
        if not case["channels"]:
            raise GateError("mix case %d has no channels" % i)
        env = case["channels"][0].get("env")
        if env not in envs:
            raise GateError("mix case %d names unknown env %r" % (i, env))
        if bool(case.get("baseline")) != bool(case.get("wav")):
            raise GateError("mix case %d must pair baseline and wav" % i)
        wav = case.get("wav")
        if wav is not None and (not isinstance(wav, str) or "/" not in wav):
            raise GateError("mix case %d has a bad wav path" % i)
    return data


def check(name: str, condition: str) -> str:
    return "    CHECK(%s, %s);" % (cstr(name), condition)


def render_env_literal(env: Dict[str, Any]) -> str:
    return "{" + "%du, %du, %d, %du" % (int(env["attack_ms"]),
                                        int(env["decay_ms"]),
                                        int(env["sustain_pct"]),
                                        int(env["release_ms"])) + "}"


def render_parsers(cases: List[dict], rate: int) -> List[str]:
    out: List[str] = []
    for case in cases:
        name = ident(case["name"])
        tag = "parser." + name
        text = cstr(str(case["input"]))
        out.extend([
            "    {",
            ("        audio_engine_env_t got_env;" if case["kind"] == "env"
             else "        audio_engine_seq_event_t got_seq;"),
        ])
        if case["kind"] == "env":
            out.append("        bool ok = audio_engine_env_parse(%s, &got_env);"
                       % text)
        else:
            out.append("        bool ok = audio_engine_seq_parse(%s, %du, &got_seq);"
                       % (text, rate))
        out.append(check(tag + ".ok", "ok == %s"
                         % ("true" if case["expect_ok"] else "false")))
        if case["expect_ok"]:
            expect = case.get("expect")
            if not isinstance(expect, dict):
                raise GateError("parser %s has no expectations" % name)
            fields = (("attack_ms", "got_env.attack_ms"),
                      ("decay_ms", "got_env.decay_ms"),
                      ("sustain_pct", "got_env.sustain_pct"),
                      ("release_ms", "got_env.release_ms")) \
                     if case["kind"] == "env" else \
                     (("start_sample", "got_seq.start_sample"),
                      ("length_samples", "got_seq.length_samples"),
                      ("freq_hz", "got_seq.freq_hz"),
                      ("gain_pct", "got_seq.gain_pct"))
            for field, actual in fields:
                if field not in expect:
                    raise GateError("parser %s lacks %r" % (name, field))
                out.append(check(tag + "." + field,
                                 "%s == %du" % (actual, int(expect[field]))))
        out.append("    }")
    return out


def render_mix(case: dict, data: Dict[str, Any], rate: int) -> List[str]:
    name = ident(case["name"])
    tag = "mix." + name
    frames = int(case["frames"])
    if frames <= 0 or frames > 4096:
        raise GateError("mix %s has an invalid frame count" % name)
    env_name = case["channels"][0]["env"]
    env = data["envs"][env_name]
    out: List[str] = [
        "    {",
        "        audio_engine_t eng;",
        "        int16_t out[%d];" % (2 * frames),
        "        audio_engine_env_t env = %s;" % render_env_literal(env),
        "        int added;",
        "        audio_engine_init(&eng, %du);" % rate,
        "        eng.master_gain_q16 = audio_engine_gain_q16(%d);"
        % int(case.get("gain_pct", 100)),
        "        eng.duck_gain_q16 = audio_engine_gain_q16(%d);"
        % int(case.get("duck_pct", 100)),
        "        eng.duck_attack_ms = %du;"
        % int(case.get("duck_attack_ms", 0)),
        "        eng.duck_release_ms = %du;"
        % int(case.get("duck_release_ms", 0)),
        "/* AUDIO_GATE_INJECT */",
        "#if AUDIO_GATE_LEAK",
        "        eng.master_gain_q16 = 131072;  /* leaked gain canary */",
        "#endif",
    ]
    for idx, channel in enumerate(case["channels"]):
        values = channel.get("values")
        if not isinstance(values, list) or not values:
            raise GateError("mix %s channel %d has no PCM" % (name, idx))
        bus = channel.get("bus")
        if bus not in BUSES:
            raise GateError("mix %s channel %d has unknown bus %r"
                            % (name, idx, bus))
        envn = channel.get("env")
        if envn not in data["envs"]:
            raise GateError("mix %s channel %d has unknown env %r"
                            % (name, idx, envn))
        env_lit = render_env_literal(data["envs"][envn])
        out.extend([
            "        {",
            "            const int16_t pcm_%d[%d] = {%s};"
            % (idx, len(values), ",".join(str(int(v)) for v in values)),
            "            audio_engine_env_t env_%d = %s;" % (idx, env_lit),
            "            added = audio_engine_add_pcm(&eng, %du, pcm_%d, %du, %du, %d, %s, &env_%d);"
            % (int(channel.get("start", 0)), idx, len(values),
               int(channel.get("repeat", 1)), int(channel.get("gain", 100)),
               BUSES[bus], idx),
            check(tag + ".add%d" % idx, "added == %d" % idx),
            "        }",
        ])
    out.extend([
        "        audio_engine_render(&eng, out, %du, 0u);" % frames,
        "#if AUDIO_GATE_SILENT",
        "        memset(out, 0, sizeof(out));  /* silent mix canary */",
        "#endif",
        check(tag + ".stereo", "stereo_ok(out, %du)" % frames),
    ])
    if "expect_left" in case:
        expected = case["expect_left"]
        if not isinstance(expected, list) or len(expected) != frames:
            raise GateError("mix %s expectation/count mismatch" % name)
        for frame, value in enumerate(expected):
            out.append(check("%s.f%d" % (tag, frame),
                             "out[%d] == %d" % (2 * frame, int(value))))
    for probe in case.get("probes", []):
        frame = int(probe["frame"])
        if frame >= frames:
            raise GateError("mix %s probe frame out of range" % name)
        out.append(check("%s.p%d" % (tag, frame),
                         "out[%d] == %d" % (2 * frame, int(probe["expect"]))))
    baseline = case.get("baseline")
    if baseline:
        if not isinstance(baseline, str) or "/" not in baseline:
            raise GateError("mix %s has a bad baseline path" % name)
        dump = ident(name) + ".pcm"
        out.extend([
            "        {",
            "            char path[512];",
            "            FILE *dump;",
            "            size_t wrote;",
            '            snprintf(path, sizeof(path), "%s/%s", dump_dir, '
            + cstr(dump) + ");",
            "            dump = fopen(path, \"wb\");",
            check(tag + ".dump_open", "dump != NULL"),
            "            if (dump != NULL) {",
            "                wrote = fwrite(out, sizeof(int16_t), %du, dump);"
            % (2 * frames),
            check(tag + ".dump_bytes", "wrote == %du" % (2 * frames)),
            "                fclose(dump);",
            "            }",
            "        }",
        ])
    out.append("    }")
    return out


def render_sequence(case: dict, rate: int) -> List[str]:
    events = case.get("events")
    expect = case.get("expect")
    if not isinstance(events, list) or not events or \
            not isinstance(expect, list) or len(events) != len(expect):
        raise GateError("sequence events/expectations disagree")
    out: List[str] = [
        "    {",
        "        audio_engine_t eng;",
        "        audio_engine_env_t env = {0u, 0u, 100, 0u};",
        "        audio_engine_seq_event_t event;",
        "        int16_t out[16];",
        "        int added;",
        "        audio_engine_init(&eng, %du);" % rate,
    ]
    for idx, (text, want) in enumerate(zip(events, expect)):
        out.extend([
            "        added = 0;",
            "        CHECK(%s, audio_engine_seq_parse(%s, %du, &event));"
            % (cstr("seq.parse%d" % idx), cstr(str(text)), rate),
        ])
        for field in ("start_sample", "length_samples", "freq_hz", "gain_pct"):
            if field not in want:
                raise GateError("sequence expect %d lacks %r" % (idx, field))
            out.append(check("seq.e%d.%s" % (idx, field),
                             "event.%s == %du" % (field, int(want[field]))))
        out.extend([
            "        added = audio_engine_add_event(&eng, &event, "
            "AUDIO_ENGINE_BUS_MUSIC, &env);",
            check("seq.add%d" % idx, "added == %d" % idx),
        ])
    out.extend([
        check("seq.length", "audio_engine_length_samples(&eng) == %du"
              % int(case["expect_length_samples"])),
        "        audio_engine_render(&eng, out, %du, 0u);"
        % int(case["silent_prefix_frames"]),
        check("seq.prefix_silent", "peak_i16(out, %du) == 0"
              % int(case["silent_prefix_frames"])),
        "        audio_engine_render(&eng, out, %du, %du);"
        % (int(case["active_probe_frames"]),
           int(case["active_probe_start"])),
        check("seq.active_nonzero", "peak_i16(out, %du) > 0"
              % int(case["active_probe_frames"])),
        "    }",
    ])
    return out


def render_bounded(case: dict) -> List[str]:
    indices = case.get("expect_indices")
    if not isinstance(indices, list) or not indices:
        raise GateError("bounded case has no expected indices")
    out: List[str] = [
        "    {",
        "        audio_engine_t eng;",
        "        int added;",
        "        audio_engine_init(&eng, 48000u);",
    ]
    for idx, want in enumerate(indices):
        out.extend([
            "        added = audio_engine_add_tone(&eng, %du, 480u, 440, 100, "
            "AUDIO_ENGINE_BUS_MUSIC, NULL);" % (idx * 480),
            check("bounded.add%d" % idx, "added == %d" % int(want)),
        ])
    out.extend([
        check("bounded.count", "audio_engine_count(&eng) == %du"
              % int(case["expect_count"])),
        check("bounded.full", "audio_engine_full(&eng) == true"),
        "        audio_engine_reset(&eng);",
        check("bounded.reset", "audio_engine_count(&eng) == 0u"),
        "    }",
    ])
    return out


def render_meter(case: dict) -> List[str]:
    values = case.get("values")
    if not isinstance(values, list) or not values:
        raise GateError("meter case has no values")
    frames = int(case["frames"])
    out: List[str] = [
        "    {",
        "        const int16_t block[%d] = {%s};"
        % (len(values), ",".join(str(int(v)) for v in values)),
        "        int64_t sum_sq = 0;",
        "        int peak = 0;",
        "        audio_engine_meter_accumulate(block, %du, &sum_sq, &peak);"
        % frames,
        check("meter.peak", "peak == %d" % int(case["expect_peak"])),
        check("meter.rms", "audio_engine_meter_finish(sum_sq, %du) == %d"
              % (int(case["sample_count"]), int(case["expect_rms"]))),
        check("meter.empty", "audio_engine_meter_finish(0, 0) == 0"),
        "    }",
    ]
    return out


def render_offline(case: dict) -> List[str]:
    values = case.get("values")
    if not isinstance(values, list) or not values:
        raise GateError("offline case has no values")
    frames = int(case["frames"])
    split = int(case["split"])
    if split <= 0 or split >= frames:
        raise GateError("offline split must be inside the buffer")
    out: List[str] = [
        "    {",
        "        audio_engine_t eng;",
        "        const int16_t src[%d] = {%s};"
        % (len(values), ",".join(str(int(v)) for v in values)),
        "        int16_t whole[%d];" % (2 * frames),
        "        int16_t parts[%d];" % (2 * frames),
        "        int added;",
        "        audio_engine_init(&eng, 1000u);",
        "        added = audio_engine_add_pcm(&eng, 0u, src, %du, 1, 100, "
        "AUDIO_ENGINE_BUS_MUSIC, NULL);" % len(values),
        check("offline.add", "added == 0"),
        "        audio_engine_render(&eng, whole, %du, 0u);" % frames,
        "        audio_engine_render(&eng, parts, %du, 0u);" % split,
        "        audio_engine_render(&eng, parts + %d, %du, %du);"
        % (2 * split, frames - split, split),
        check("offline.chunked",
              "memcmp(whole, parts, sizeof(whole)) == 0"),
        check("offline.sample", "whole[0] == %d && whole[%d] == %d"
              % (int(case["expect_sample"]), 2 * (frames - 1),
                 int(case["expect_sample"]))),
        "    }",
    ]
    return out


def render_harness(data: Dict[str, Any]) -> str:
    rate = int(data["sample_rate"])
    body: List[str] = []
    body.extend(render_parsers(data["parsers"], rate))
    for case in data["mixes"]:
        body.extend(render_mix(case, data, rate))
    body.extend(render_sequence(data["sequence"], rate))
    body.extend(render_bounded(data["bounded"]))
    body.extend(render_meter(data["meter"]))
    body.extend(render_offline(data["offline"]))
    return "\n".join([
        "/* GENERATED by tools/host_audio_gate.py - do not edit. */",
        '#include "audio_engine.h"',
        "#include <stdio.h>",
        "#include <string.h>",
        "",
        "static int s_checks = 0;",
        "static int s_failures = 0;",
        '#define CHECK(name, cond) do { s_checks++; if (cond) {'
        ' printf("audio.check %s ok\\n", (name)); } else { s_failures++;'
        ' printf("audio.check %s FAIL\\n", (name)); } } while (0)',
        "",
        "static bool stereo_ok(const int16_t *out, size_t frames)",
        "{",
        "    size_t i;",
        "    for (i = 0; i < frames; i++) {",
        "        if (out[2 * i] != out[2 * i + 1]) return false;",
        "    }",
        "    return true;",
        "}",
        "",
        "static int peak_i16(const int16_t *out, size_t frames)",
        "{",
        "    size_t i;",
        "    int peak = 0;",
        "    for (i = 0; i < 2 * frames; i++) {",
        "        int mag = out[i] < 0 ? -out[i] : out[i];",
        "        if (mag > peak) peak = mag;",
        "    }",
        "    return peak;",
        "}",
        "",
        "int main(int argc, char **argv)",
        "{",
        "    const char *dump_dir = (argc > 1) ? argv[1] : \".\";",
        '    printf("' + BEGIN + ' tag=' + data["tag"] + '\\n");',
    ] + body + [
        '    printf("' + SUMMARY + ' checks=%d fail=%d\\n", s_checks, s_failures);',
        '    printf("' + END + '\\n");',
        "    return s_failures == 0 ? 0 : 1;",
        "}",
        "",
    ])


def parse_output(raw: str, returncode: int) -> Tuple[int, int]:
    text = raw.replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.strip() for line in text.split("\n")]
    begins = [line for line in lines if line.startswith(BEGIN + " ")]
    ends = [line for line in lines if line == END]
    summaries = [SUMMARY_RE.match(line) for line in lines
                 if line.startswith(SUMMARY + " ")]
    summaries = [m for m in summaries if m]
    checks = [CHECK_RE.match(line) for line in lines
              if line.startswith("audio.check ")]
    checks = [m for m in checks if m]

    if len(begins) != 1:
        raise GateError("expected exactly one begin marker, got %d"
                        % len(begins))
    if len(ends) != 1:
        raise GateError("expected exactly one end marker, got %d" % len(ends))
    if len(summaries) != 1:
        raise GateError("expected exactly one summary marker, got %d"
                        % len(summaries))
    checks_count = int(summaries[0].group(1))
    fail_count = int(summaries[0].group(2))

    names = [m.group(1) for m in checks]
    if len(names) != len(set(names)):
        raise GateError("duplicate audio.check name in harness output")
    fails = [m.group(1) for m in checks if m.group(2) == "FAIL"]
    if checks_count != len(checks):
        raise GateError("summary checks=%d but %d check lines"
                        % (checks_count, len(checks)))
    if fail_count != len(fails):
        raise GateError("summary fail=%d but %d FAIL lines"
                        % (fail_count, len(fails)))
    if len(checks) < MIN_CHECKS:
        raise GateError("vacuous harness: %d checks, need >= %d"
                        % (len(checks), MIN_CHECKS))
    if returncode != 0 or fails:
        raise GateError("harness red: rc=%d, failing checks: %s"
                        % (returncode, ", ".join(fails[:6]) or "(summary only)"))
    return checks_count, len(fails)


def audit_wiring(overrides: Optional[Dict[str, str]] = None) -> List[str]:
    """Reject a second driver and confirm the six new verbs are additive.

    This is a wiring audit, not a runtime claim. It reads the same source
    files the firmware builds; ``overrides`` exists only for the red canary.
    """
    problems: List[str] = []
    overrides = overrides or {}

    def text(rel: str) -> str:
        if rel in overrides:
            return overrides[rel]
        path = ROOT / rel
        return path.read_text(encoding="utf-8", errors="replace") \
            if path.is_file() else ""

    codec = text("components/audio/audio.c")
    pure = text("components/audio/audio_engine.c")
    commands = text("components/command/audioengine_commands.c")
    existing = text("components/command/audio_commands.c")
    audio_cmake = text("components/audio/CMakeLists.txt")
    tests = text("test/main/test_audio.c")

    if codec.count("bsp_audio_codec_speaker_init(") != 1:
        problems.append("speaker codec must be initialized in exactly one place")
    if codec.count("static esp_codec_dev_handle_t s_speaker_dev") != 1:
        problems.append("exactly one speaker codec handle is required")
    if codec.count("xTaskCreateWithCaps(") != 1:
        problems.append("exactly one audio playback task is allowed")
    for marker in ("AUDIO_PLAY_MIX", "audio_task_play_mix(&req)",
                   "audio_engine_render(&s_mix", "audio_amp_apply_playback()",
                   "audio_amp_mute();\n    esp_codec_dev_close(s_speaker_dev);"):
        if marker not in codec:
            problems.append("audio.c lost the shared mixer path: %s" % marker)
    for forbidden in ("esp_codec_dev_handle_t", "bsp_audio_", "xTaskCreate",
                      "SemaphoreHandle_t", "heap_caps_"):
        if forbidden in pure:
            problems.append("pure engine leaked hardware dependency: %s"
                            % forbidden)
    if "audio_engine.c" not in audio_cmake:
        problems.append("audio component does not compile audio_engine.c")
    for verb in ("mix", "sfx", "seq", "env", "gain", "meter"):
        if 'shell_text_equals_ignore_case(argv[1], "%s")' % verb not in commands:
            problems.append("new command source lost verb: %s" % verb)
    for verb in ("shell_command_audio(", "shell_command_mic(",
                 "shell_command_beep(", "shell_command_tone(",
                 "shell_command_wavplay(", "shell_command_volume("):
        if verb in commands:
            problems.append("new command source duplicated existing verb: %s"
                            % verb)
    for verb in ("shell_command_audio(", "shell_command_mic(",
                 "shell_command_beep(", "shell_command_tone(",
                 "shell_command_wavplay(", "shell_command_volume("):
        if verb not in existing:
            problems.append("existing audio command handler lost: %s" % verb)
    for call in ("audio_mix_play(", "audio_engine_seq_parse(",
                 "audio_engine_env_parse(", "audio_mix_meter("):
        if call not in commands:
            problems.append("new command source does not route to %s" % call)
    for call in ("audio_engine_regression_checks();",
                 "audio_engine_bound_checks();"):
        if call not in tests:
            problems.append("on-target audio suite lost %s" % call)

    # miniaudio is a host-only cross-check: it must never reach firmware.
    firmware = [ROOT / "CMakeLists.txt"]
    firmware += list((ROOT / "components").rglob("CMakeLists.txt"))
    firmware += list((ROOT / "main").rglob("CMakeLists.txt"))
    for src_root in (ROOT / "components", ROOT / "main"):
        firmware += [p for p in src_root.rglob("*")
                     if p.is_file() and p.suffix in (".c", ".h", ".cpp")]
    for path in firmware:
        if not path.is_file():
            continue
        try:
            body = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "miniaudio" in body:
            problems.append("miniaudio must stay host-only: %s"
                            % path.relative_to(ROOT).as_posix())
    return problems


def read_baseline(rel: str) -> bytes:
    expected_path = FIXTURE.parent / rel
    if not expected_path.is_file():
        raise GateError("baseline is missing: %s" % rel)
    try:
        raw_hex = expected_path.read_text(encoding="utf-8").strip()
        return bytes.fromhex(raw_hex)
    except (OSError, ValueError) as exc:
        raise GateError("baseline unreadable %s: %s" % (rel, exc))


def verify_baselines(data: Dict[str, Any], dump_dir: Path) -> None:
    for case in data["mixes"]:
        rel = case.get("baseline")
        if not rel:
            continue
        expected = read_baseline(rel)
        actual_path = dump_dir / (ident(case["name"]) + ".pcm")
        if not actual_path.is_file():
            raise GateError("harness did not dump PCM for %s" % case["name"])
        actual = actual_path.read_bytes()
        count = 2 * int(case["frames"])
        if expected != actual:
            raise GateError("PCM baseline mismatch for %s (want %d bytes, "
                            "got %d)" % (case["name"], len(expected),
                                         len(actual)))
        try:
            decoded = struct.unpack("<%dh" % count, actual)
        except struct.error as exc:
            raise GateError("PCM dump is malformed for %s: %s"
                            % (case["name"], exc))
        if any(v not in range(-32768, 32768) for v in decoded):
            raise GateError("PCM dump has an out-of-range sample for %s"
                            % case["name"])


_MINIAUDIO_CACHE: Dict[tuple, Path] = {}


def compile_miniaudio_decoder(cc: str, work: Path) -> Path:
    """Build the host-only miniaudio helper once per process, never in firmware."""
    if not MINIAUDIO_SRC.is_file() or not MINIAUDIO_HEADER.is_file():
        raise GateError("vendored miniaudio is missing from tools/thirdparty")
    key = (cc, MINIAUDIO_SRC.stat().st_mtime_ns,
           MINIAUDIO_HEADER.stat().st_mtime_ns)
    cached = _MINIAUDIO_CACHE.get(key)
    if cached is not None and cached.is_file():
        return cached
    work = Path(tempfile.mkdtemp(prefix="audio-miniaudio-"))
    atexit.register(shutil.rmtree, work, ignore_errors=True)
    binary = work / ("ma_audio_host.exe" if os.name == "nt"
                     else "ma_audio_host")
    cmd = [cc, "-std=gnu99", "-Wall", "-Wextra", "-Werror",
           "-I" + str(THIRDPARTY_DIR), str(MINIAUDIO_SRC),
           "-o", str(binary)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              cwd=str(ROOT),
                              timeout=MINIAUDIO_COMPILE_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise GateError("miniaudio decoder compile timed out")
    if proc.returncode != 0 or not binary.is_file():
        raise GateError("miniaudio decoder compile failed (rc=%d): %s"
                        % (proc.returncode,
                           (proc.stdout + proc.stderr)[-1200:]))
    _MINIAUDIO_CACHE[key] = binary
    return binary


def verify_miniaudio_wav(data: Dict[str, Any], work: Path, cc: str,
                         wav_root: Optional[Path] = None,
                         decoder: Optional[Path] = None) -> int:
    """Independently decode the WAV fixtures and compare to PCM baselines.

    The WAVs are authored from the literal ``expect_left`` values, not from
    the baselines. miniaudio must agree with the baselines that the shipped
    mixer generated; any disagreement makes the gate red.
    """
    wav_root = Path(wav_root) if wav_root is not None else FIXTURE_WAVS
    if decoder is None:
        decoder = compile_miniaudio_decoder(cc, work)
    expected_rate = int(data["sample_rate"])
    checked = 0
    for case in data["mixes"]:
        rel = case.get("wav")
        baseline_rel = case.get("baseline")
        if not rel or not baseline_rel:
            continue
        wav = wav_root / Path(rel).name
        if not wav.is_file():
            raise GateError("WAV fixture is missing: %s" % wav)
        dump = work / (ident(case["name"]) + ".ma.pcm")
        try:
            proc = subprocess.run([str(decoder), "decode", str(wav),
                                   str(dump)],
                                  capture_output=True, text=True,
                                  cwd=str(work), timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            raise GateError("miniaudio decode timed out for %s" % rel)
        if proc.returncode != 0 or not dump.is_file():
            raise GateError("miniaudio decode failed for %s (rc=%d): %s"
                            % (rel, proc.returncode,
                               (proc.stdout + proc.stderr)[-600:]))
        lines = [line.strip() for line in proc.stdout.splitlines()
                 if line.strip()]
        match = DECODER_RE.match(lines[-1]) if lines else None
        if match is None:
            raise GateError("miniaudio summary malformed for %s: %s"
                            % (rel, (proc.stdout + proc.stderr)[-400:]))
        channels, rate, frames, size = (int(part) for part in match.groups())
        frames_want = int(case["frames"])
        if (channels, rate, frames, size) != (2, expected_rate, frames_want,
                                               4 * frames_want):
            raise GateError("miniaudio format mismatch for %s: %s"
                            % (rel, match.group(0)))
        expected = read_baseline(baseline_rel)
        actual = dump.read_bytes()
        if actual != expected:
            raise GateError("miniaudio WAV cross-check mismatch for %s "
                            "(want %d bytes, got %d)"
                            % (case["name"], len(expected), len(actual)))
        checked += 1
    if checked == 0:
        raise GateError("vacuous miniaudio cross-check: no WAV fixtures")
    return checked


def load_asset_preparer():
    """Import the shipped host-side preparation tool by path."""
    import importlib.util

    path = ROOT / "tools" / "audio_asset_prepare.py"
    spec = importlib.util.spec_from_file_location("audio_asset_prepare", path)
    if spec is None or spec.loader is None:
        raise GateError("audio_asset_prepare.py cannot be loaded")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_audio_asset_preparation(work: Path, cc: str, decoder: Path,
                                   source_root: Optional[Path] = None) -> int:
    """End-to-end: convert an unplayable source into the firmware's format."""
    module = load_asset_preparer()
    root = Path(source_root) if source_root is not None else ASSET_FIXTURES
    source = root / "src_48k_24b_stereo.wav"
    if not source.is_file():
        raise GateError("conversion source fixture is missing: %s" % source)
    dest = work / "prepared.wav"
    try:
        info = module.prepare(source, dest, 22050, 2, cc=cc, work=work,
                              tool=decoder)
    except module.PrepareError as exc:
        raise GateError("audio asset preparation failed: %s" % exc)

    frames = int(info["frames"])
    want_frames = round(480 * 22050 / 48000)
    if abs(frames - want_frames) > 2 or (info["rate"], info["channels"]) \
            != (22050, 2):
        raise GateError("no real resample/convert happened: %r" % info)
    # Decoding the prepared WAV again must reproduce the exact PCM we wrote.
    replay = work / "prepared_replay.pcm"
    proc = subprocess.run([str(decoder), "decode", str(dest), str(replay)],
                          capture_output=True, text=True,
                          cwd=str(work), timeout=RUN_TIMEOUT)
    if proc.returncode != 0:
        raise GateError("prepared WAV cannot be decoded: %s" % proc.stderr[-400:])
    lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    match = DECODER_RE.match(lines[-1]) if lines else None
    if match is None or tuple(int(p) for p in match.groups()) != \
            (2, 22050, frames, frames * 4):
        raise GateError("prepared WAV format is wrong: %s" % proc.stdout[-400:])
    if replay.read_bytes() != info["pcm"]:
        raise GateError("prepared WAV PCM changed on re-decode")

    # A non-audio input must be refused loudly, not silently produce silence.
    invalid = root / "invalid.wav"
    if not invalid.is_file():
        raise GateError("invalid audio fixture is missing: %s" % invalid)
    try:
        module.prepare(invalid, work / "invalid.wav", 22050, 2, cc=cc,
                       work=work, tool=decoder)
    except module.PrepareError:
        pass
    else:
        raise GateError("invalid audio input was accepted by preparation")
    return 2


def run_gate(fixture: Path, cc: str, verbose: bool = False,
             source: Optional[str] = None, defines: Sequence[str] = (),
             overrides: Optional[Dict[str, str]] = None) -> Tuple[int, int]:
    data = load_fixture(fixture)
    problems = audit_wiring(overrides)
    if problems:
        raise GateError("wiring audit failed: " + "; ".join(problems))

    with tempfile.TemporaryDirectory(prefix="audio-gate-") as tmp:
        work = Path(tmp)
        harness = work / "audio_harness.c"
        binary = work / ("audio_harness.exe" if os.name == "nt"
                         else "audio_harness")
        harness.write_text(source if source is not None
                           else render_harness(data),
                           encoding="utf-8", newline="\n")
        cmd = [cc, "-std=gnu99", "-Wall"] + list(defines)
        cmd += ["-I" + str(p) for p in EXTRA_INCLUDES]
        cmd += [str(PURE_SRC), str(harness), "-o", str(binary)]
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              cwd=str(ROOT), timeout=RUN_TIMEOUT)
        if proc.returncode != 0 or not binary.is_file():
            raise GateError("harness compile failed (rc=%d): %s"
                            % (proc.returncode,
                               (proc.stdout + proc.stderr)[-1200:]))
        try:
            run = subprocess.run([str(binary), str(work)],
                                 capture_output=True, text=True,
                                 cwd=str(work), timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            raise GateError("harness run timed out after %ds" % RUN_TIMEOUT)
        if verbose:
            print(run.stdout)
        checks, failures = parse_output(run.stdout + run.stderr,
                                        run.returncode)
        if failures == 0:
            verify_baselines(data, work)
            decoder = compile_miniaudio_decoder(cc, work)
            verify_miniaudio_wav(data, work, cc, decoder=decoder)
            verify_audio_asset_preparation(work, cc, decoder)
        return checks, failures


def self_test(cc: str) -> int:
    """Red canaries: silent mix, duplicate driver, leaked gain, wrong order."""
    data = load_fixture(FIXTURE)
    checks: List[Tuple[str, bool]] = []

    def report(count: int, fail: int = 0, names=None):
        names = names or ["c%d" % i for i in range(count)]
        lines = [BEGIN + " tag=selftest"]
        lines += ["audio.check %s %s" % (name, "FAIL" if i < fail else "ok")
                  for i, name in enumerate(names)]
        lines += [SUMMARY + " checks=%d fail=%d" % (count, fail), END]
        return "\n".join(lines) + "\n"

    def must_fail(name: str, raw: str, rc: int = 0) -> None:
        try:
            parse_output(raw, rc)
            ok = False
        except GateError:
            ok = True
        checks.append((name, ok))

    try:
        count, failures = parse_output(report(MIN_CHECKS), 0)
        checks.append(("clean report accepted",
                       count == MIN_CHECKS and failures == 0))
    except GateError as exc:
        print("  clean report unexpectedly failed: %s" % exc)
        checks.append(("clean report accepted", False))
    must_fail("missing begin marker rejected",
              "\n".join(report(MIN_CHECKS).splitlines()[1:]) + "\n")
    must_fail("summary count mismatch rejected",
              report(MIN_CHECKS).replace("checks=%d" % MIN_CHECKS,
                                         "checks=%d" % (MIN_CHECKS + 1)))
    must_fail("failing check rejected", report(MIN_CHECKS, fail=1))
    must_fail("nonzero exit rejected", report(MIN_CHECKS), rc=1)
    must_fail("too few checks rejected", report(MIN_CHECKS - 1))
    must_fail("duplicate check name rejected",
              report(MIN_CHECKS, names=["dup", "dup"] +
                     ["c%d" % i for i in range(MIN_CHECKS - 2)]))

    def gate_red(name: str, **kwargs) -> None:
        try:
            run_gate(FIXTURE, cc, **kwargs)
            caught = False
            detail = "gate went green"
        except GateError as exc:
            caught = True
            detail = str(exc)
        checks.append((name, caught))
        if not caught:
            print("  %s was NOT red: %s" % (name, detail))

    gate_red("silent mix goes red", defines=["-DAUDIO_GATE_SILENT=1"])
    gate_red("leaked gain goes red", defines=["-DAUDIO_GATE_LEAK=1"])
    gate_red("duplicate driver goes red",
             overrides={"components/audio/audio.c":
                        text_of_codec().replace(
                            "bsp_audio_codec_speaker_init()",
                            "bsp_audio_codec_speaker_init(); "
                            "bsp_audio_codec_speaker_init()", 1)})

    shuffled = json.loads(json.dumps(data))
    shuffled["sequence"]["expect"][0]["start_sample"] = \
        shuffled["sequence"]["expect"][1]["start_sample"]
    with tempfile.TemporaryDirectory(prefix="audio-gate-self-") as tmp:
        bad = Path(tmp) / "bad.json"
        bad.write_text(json.dumps(shuffled), encoding="utf-8", newline="\n")
        try:
            run_gate(bad, cc)
            caught = False
        except GateError:
            caught = True
        checks.append(("wrong sequence goes red", caught))

    try:
        count, failures = run_gate(FIXTURE, cc)
        checks.append(("clean fixture goes green",
                       failures == 0 and count >= MIN_CHECKS))
    except GateError as exc:
        print("  clean fixture unexpectedly failed: %s" % exc)
        checks.append(("clean fixture goes green", False))

    # miniaudio cross-check canaries: a corrupt or missing WAV must be red.
    with tempfile.TemporaryDirectory(prefix="audio-gate-miniaudio-") as tmp:
        work = Path(tmp)
        decoder = compile_miniaudio_decoder(cc, work)
        try:
            count_wav = verify_miniaudio_wav(data, work, cc, decoder=decoder)
            checks.append(("miniaudio WAV cross-check green", count_wav > 0))
        except GateError as exc:
            print("  miniaudio cross-check unexpectedly failed: %s" % exc)
            checks.append(("miniaudio WAV cross-check green", False))
        corrupted = work / "corrupt"
        corrupted.mkdir()
        for case in data["mixes"]:
            rel = case.get("wav")
            if not rel:
                continue
            raw = bytearray((FIXTURE.parent / rel).read_bytes())
            raw[-1] ^= 0xFF
            (corrupted / Path(rel).name).write_bytes(raw)
        try:
            verify_miniaudio_wav(data, work, cc, wav_root=corrupted,
                                 decoder=decoder)
            caught = False
        except GateError:
            caught = True
        checks.append(("corrupt WAV goes red", caught))
        missing = work / "missing"
        missing.mkdir()
        try:
            verify_miniaudio_wav(data, work, cc, wav_root=missing,
                                 decoder=decoder)
            caught = False
        except GateError:
            caught = True
        checks.append(("missing WAV goes red", caught))

        try:
            asset_checks = verify_audio_asset_preparation(work, cc, decoder)
            checks.append(("audio asset conversion green", asset_checks == 2))
        except GateError as exc:
            print("  asset conversion unexpectedly failed: %s" % exc)
            checks.append(("audio asset conversion green", False))
        truncated = work / "truncated_assets"
        truncated.mkdir()
        src = (ASSET_FIXTURES / "src_48k_24b_stereo.wav").read_bytes()
        (truncated / "src_48k_24b_stereo.wav").write_bytes(src[:100])
        (truncated / "invalid.wav").write_bytes(b"not audio\n")
        try:
            verify_audio_asset_preparation(work, cc, decoder,
                                           source_root=truncated)
            caught = False
        except GateError:
            caught = True
        checks.append(("truncated source goes red", caught))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    print("host_audio_gate self-test: %s (%d canaries)"
          % ("PASS" if all(ok for _n, ok in checks) else "FAIL", len(checks)))
    return 0 if all(ok for _n, ok in checks) else 1


def text_of_codec() -> str:
    path = ROOT / "components" / "audio" / "audio.c"
    return path.read_text(encoding="utf-8", errors="replace")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Hardware-free audio engine gate")
    parser.add_argument("--check", action="store_true", help="run the gate (default)")
    parser.add_argument("--fixture", type=Path, default=FIXTURE)
    parser.add_argument("--cc", default=os.environ.get("CC") or
                        shutil.which("gcc") or shutil.which("cc") or "gcc",
                        help="C compiler for the pure-source harness")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test(args.cc)
    try:
        count, failures = run_gate(args.fixture, args.cc, args.verbose)
    except GateError as exc:
        print("HOST AUDIO GATE FAIL: %s" % exc)
        return 1
    print("host_audio_gate: PASS (%d checks, %d failures, tag from fixture)"
          % (count, failures))
    print("HOST AUDIO GATE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
