#!/usr/bin/env python3
"""csv_test.py - hardware driver for the csv grid verbs.

Builds a small fixture with quoted fields and =EXPR formulas, checks
rows/cols/cell//v:NAME/eval (aligned and /b), then cleans up.
Prints RESULT OK on success.
Usage: python tools/csv_test.py COMx

Note: lines are burst-written up front; reading between commands would
stall the host on blocking reads and is unnecessary here.
"""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from shell_session import open_port


def run(port):
    ser = open_port(port, timeout=10)
    time.sleep(1)
    ser.reset_input_buffer()

    lines = [
        "write _csv_hw.txt item,qty,price",
        "append _csv_hw.txt apple,3,0.5",
        "append _csv_hw.txt ^\"berry,sweet^\",2,=R2C2*10",
        "append _csv_hw.txt cherry,4,=R3C3+R2C2",
        "csv rows _csv_hw.txt",
        "csv cols _csv_hw.txt",
        "csv cell _csv_hw.txt 3 1",
        "csv cell _csv_hw.txt 3 1 /v:CSVCELL",
        "echo CSVCELL=%CSVCELL%",
        "csv eval _csv_hw.txt",
        "csv eval _csv_hw.txt /b",
        "del _csv_hw.txt",
    ]
    for line in lines:
        ser.write((line + "\r\n").encode())
        time.sleep(0.4)

    deadline = time.time() + 40
    out = ""
    while time.time() < deadline:
        chunk = ser.read(65536).decode("utf-8", "replace")
        out += chunk
        if "moved to" in out and out.count("csv eval") >= 2:
            break
        if len(chunk) == 0:
            time.sleep(0.5)
    out = re.sub(r"\x1b\[[0-9;]*m", "", out)

    assert re.search(r"rows:\s*4", out), "rows failed:\n" + out
    assert re.search(r"cols:\s*3", out), "cols failed:\n" + out
    assert "R3C1 = berry,sweet" in out, "quoted cell failed:\n" + out
    assert "CSVCELL=berry,sweet" in out, "/v store failed:\n" + out
    assert re.search(r"berry,sweet\s*\|\s*2\s*\|\s*30", out), "eval table failed:\n" + out
    assert '"berry,sweet",2,30' in out, "eval /b re-quote failed:\n" + out
    assert "cherry,4,33" in out, "eval chained ref failed:\n" + out
    assert "moved to" in out, "cleanup failed:\n" + out
    print("RESULT OK")


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "COM3")
