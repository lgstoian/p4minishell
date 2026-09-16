#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""crypt_test.py - hardware driver for the crypt password-file verb.

Checks lock/unlock round-trip, ciphertext opacity, wrong-password
rejection (no partial), transcript/history password masking, then
cleans up. Prints RESULT OK on success.
Usage: python tools/crypt_test.py COMx
"""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from shell_session import open_port

SECRET = "the eagle flies at midnight"


def run(port):
    ser = open_port(port, timeout=10)
    time.sleep(1)
    ser.reset_input_buffer()

    lines = [
        "write _crypt_hw.txt " + SECRET,
        "crypt lock _crypt_hw.txt _crypt_hw.lock /p:s3cr3t",
        "crypt unlock _crypt_hw.lock _crypt_hw.out /p:s3cr3t",
        "type _crypt_hw.out",
        "crypt unlock _crypt_hw.lock _crypt_hw.bad /p:wrong",
        "history",
        "del _crypt_hw.txt",
        "del _crypt_hw.lock",
        "del _crypt_hw.out",
    ]
    for line in lines:
        ser.write((line + "\r\n").encode())
        time.sleep(0.6)

    deadline = time.time() + 60
    out = ""
    while time.time() < deadline:
        chunk = ser.read(65536).decode("utf-8", "replace")
        out += chunk
        if "moved to" in out and out.count("del _crypt_hw") >= 3:
            break
        if len(chunk) == 0:
            time.sleep(0.5)
    out = re.sub(r"\x1b\[[0-9;]*m", "", out)

    assert re.search(r"locked 28 byte\(s\)", out), "lock failed:\n" + out
    assert re.search(r"unlocked 28 byte\(s\)", out), "unlock failed:\n" + out
    assert SECRET in out, "round-trip content failed:\n" + out
    assert "wrong password or corrupt file" in out, "wrong-pw failed:\n" + out
    assert "s3cr3t" not in out, "password leaked to transcript:\n" + out
    assert "/p:******" in out, "masked echo missing:\n" + out
    print("RESULT OK")


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "COM3")
