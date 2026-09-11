#!/usr/bin/env python3
"""perf_transcript_test.py - transcript length-tracking sanity.

Usage: python perf_transcript_test.py [COMx]
1. Emit many lines; newest line is intact (no truncation corruption).
2. `clip copy 3` then `clip` shows the transcript tail (get_text_from path).
3. `screenshot` writes (label path uses the ansi buffer).
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    # 40 lines; the last must be present verbatim.
    sh.run("for %v in (1 2 3 4 5 6 7 8 9 10) do echo LINE-%v", timeout=30)
    out = run_quiet(sh, "echo MARKER-END-98765")
    print("marker intact:", "MARKER-END-98765" in out, flush=True)
    out = run_quiet(sh, "clip copy 3")
    print("clip copy:", out.strip().splitlines()[-1][:80] if out.strip() else "(none)", flush=True)
    out = run_quiet(sh, "clip")
    print("clip shows MARKER-END-98765:", "MARKER-END-98765" in out, flush=True)
    out = run_quiet(sh, "screenshot SPIKE.BMP")
    print("screenshot ok:", "saved" in out, flush=True)
    run_quiet(sh, "del /p SPIKE.BMP")
    sh.close()


main()
