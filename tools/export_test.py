#!/usr/bin/env python3
"""export_test.py - hardware driver for the export interchange verb.

Builds a two-record database plus one alarm, exports db->csv/json/txt
and alarms->csv/json, checks file contents, then cleans up.
Prints RESULT OK on success.
Usage: python tools/export_test.py COMx
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
        "db create hexp",
        "db add hexp /key:k1 name=Ann;city=Oslo",
        "db add hexp /key:k2 note=plain",
        "alarm add 2099-01-02 03:04 /msg:hw-export-probe",
        "export db hexp csv _ex.csv",
        "export db hexp json _ex.json",
        "export db hexp txt _ex.txt",
        "export alarms csv _exa.csv",
        "export alarms json _exa.json",
        "type _ex.csv",
        "type _ex.json",
        "type _ex.txt",
        "type _exa.csv",
        "del _ex.csv",
        "del _ex.json",
        "del _ex.txt",
        "del _exa.csv",
        "del _exa.json",
        "alarm del all",
        "db drop hexp",
    ]
    for line in lines:
        ser.write((line + "\r\n").encode())
        time.sleep(0.5)

    deadline = time.time() + 60
    out = ""
    while time.time() < deadline:
        chunk = ser.read(65536).decode("utf-8", "replace")
        out += chunk
        if "dropped hexp" in out:
            break
        if len(chunk) == 0:
            time.sleep(0.5)
    out = re.sub(r"\x1b\[[0-9;]*m", "", out)

    assert "export: 2 row(s)" in out, "db export count failed:\n" + out
    assert "1,0,k1,name=Ann;city=Oslo" in out, "db csv content failed:\n" + out
    assert '"key": "k1", "payload": "name=Ann;city=Oslo"' in out, "db json failed:\n" + out
    assert "#1 cat=0 key=k1" in out, "db txt failed:\n" + out
    assert "hw-export-probe" in out, "alarm export failed:\n" + out
    assert "dropped hexp" in out, "cleanup failed:\n" + out
    print("RESULT OK")


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "COM3")
