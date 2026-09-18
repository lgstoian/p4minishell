# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Performance and graphical frame-smoothness measurement.

Two complementary sources:

* :func:`measure_markers` times a stream of in-app frame markers on the host
  (works with the existing ``[M-*-FRAME]`` markers the demo apps emit). It
  gives a wall-clock FPS, interval spread, and a jitter figure, but is bounded
  by the 115200 serial link so it is best for <= ~40 fps.
* :func:`parse_perf_report` parses the firmware ``perf`` command, which
  measures the true present-to-present timing on the device itself (no serial
  bias) and reports count/min/avg/max/p95 and dropped-frame counts.
"""
from __future__ import annotations

import math
import re
import time
from dataclasses import dataclass
from typing import List, Optional

from .session import DeviceSession


@dataclass
class FrameStats:
    frames: int = 0
    duration_s: float = 0.0
    interval_ms_min: float = 0.0
    interval_ms_avg: float = 0.0
    interval_ms_max: float = 0.0
    jitter_ms: float = 0.0
    dropped: int = 0

    @property
    def fps(self) -> float:
        return (self.frames / self.duration_s) if self.duration_s > 0 else 0.0

    def smoothness_score(self) -> float:
        """0..100. Penalises average interval and jitter relative to the mean."""
        if self.frames < 2 or self.interval_ms_avg <= 0:
            return 0.0
        jitter_penalty = min(1.0, self.jitter_ms / self.interval_ms_avg)
        drop_penalty = min(1.0, self.dropped / max(1, self.frames) * 4.0)
        return max(0.0, 100.0 * (1.0 - 0.6 * jitter_penalty - 0.4 * drop_penalty))

    def as_text(self) -> str:
        return ("frames=%d fps=%.1f min=%.1fms avg=%.1fms max=%.1fms "
                "jitter=%.1fms dropped=%d score=%.0f" % (
                    self.frames, self.fps, self.interval_ms_min,
                    self.interval_ms_avg, self.interval_ms_max,
                    self.jitter_ms, self.dropped, self.smoothness_score()))


# The firmware reports integer microseconds (newlib-nano has no printf float),
# e.g. "gfx stats: frames=120 min_us=27862 avg_us=33333 max_us=41000
# jitter_us=1800 dropped=1 fps10=300 target_fps=30". Also accept the older
# millisecond spelling so hand-written reports still parse.
_FIELD = r"%s[=: ]\s*([0-9.]+)"


def _field(text: str, key: str) -> Optional[float]:
    m = re.search(_FIELD % re.escape(key), text, re.IGNORECASE)
    return float(m.group(1)) if m else None


def parse_perf_report(text: str) -> Optional[FrameStats]:
    """Parse a `gfx stats` / `tui stats` / `perf frame` line, or None."""
    frames = _field(text, "frames")
    if frames is None:
        return None
    stats = FrameStats()
    stats.frames = int(frames)
    scale = 1.0
    if _field(text, "avg_us") is not None:
        stats.interval_ms_avg = _field(text, "avg_us") / 1000.0
        stats.interval_ms_min = (_field(text, "min_us") or 0.0) / 1000.0
        stats.interval_ms_max = (_field(text, "max_us") or 0.0) / 1000.0
        stats.jitter_ms = (_field(text, "jitter_us") or 0.0) / 1000.0
    else:
        stats.interval_ms_avg = _field(text, "avg") or 0.0
        stats.interval_ms_min = _field(text, "min") or 0.0
        stats.interval_ms_max = _field(text, "max") or 0.0
        stats.jitter_ms = _field(text, "jitter") or 0.0
    dropped = _field(text, "dropped")
    if dropped is not None:
        stats.dropped = int(dropped)
    duration = _field(text, "duration_s") or _field(text, "elapsed_s")
    if duration:
        stats.duration_s = duration
    elif stats.interval_ms_avg > 0 and stats.frames > 1:
        stats.duration_s = (stats.interval_ms_avg * (stats.frames - 1)) / 1000.0
    _ = scale
    return stats


def measure_markers(dev: DeviceSession, launch_cmd: str,
                    marker: str = "[M-FRAME]",
                    duration: float = 8.0,
                    target_fps: Optional[float] = None) -> FrameStats:
    """Run ``launch_cmd`` and time successive ``marker`` lines on the wire."""
    dev.reset_input()
    dev.write_line(launch_cmd)
    stamps: List[float] = []
    end = time.time() + duration
    buf = b""
    while time.time() < end:
        chunk = dev.ser.read(4096) if dev.ser else b""
        if not chunk:
            time.sleep(0.002)
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if marker.encode() in line:
                stamps.append(time.time())
    stats = FrameStats()
    if len(stamps) >= 2:
        intervals = [(stamps[i + 1] - stamps[i]) * 1000.0
                     for i in range(len(stamps) - 1)]
        stats.frames = len(stamps)
        stats.duration_s = stamps[-1] - stamps[0]
        stats.interval_ms_min = min(intervals)
        stats.interval_ms_max = max(intervals)
        stats.interval_ms_avg = sum(intervals) / len(intervals)
        mean = stats.interval_ms_avg
        stats.jitter_ms = math.sqrt(
            sum((iv - mean) ** 2 for iv in intervals) / len(intervals))
        limit = (1000.0 / target_fps) * 1.6 if target_fps else mean * 1.6
        stats.dropped = sum(1 for iv in intervals if iv > limit)
    elif stamps:
        stats.frames = len(stamps)
    return stats
