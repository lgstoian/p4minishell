#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""push_dicts.py - push spellcheck wordlists to sd:/DICTS/.

Wordlists back the `edit` spellcheck underlines (writerdeck). Each
apps/dicts/<NAME>.words lands at sd:/DICTS/<NAME>.words (one lower-case
word per line; the default list is `en`).

Usage: python push_dicts.py [COMx]
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import push_file, wait_shell, open_port  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DICTS = os.path.join(HERE, "dicts")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    names = [n for n in sorted(os.listdir(DICTS)) if n.endswith(".words")]
    if not names:
        print("FAIL: no wordlists in %s" % DICTS)
        return 1

    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: shell not responding on %s" % port)
        ser.close()
        return 1
    # The open above usually reboots the board (DTR transition); let it settle.
    time.sleep(20.0)
    ser.reset_input_buffer()
    ser.write(b"md DICTS\n")
    time.sleep(0.5)
    ser.read(ser.in_waiting or 1)

    ok = True
    for name in names:
        with open(os.path.join(DICTS, name), "rb") as f:
            data = f.read()
        target = "DICTS/" + name
        pushed = False
        for attempt in range(3):
            if push_file(ser, target, data):
                print("PUSH ok   %s (%d bytes)" % (target, len(data)))
                pushed = True
                break
            print("  retry %s (%d/3)" % (target, attempt + 1))
            time.sleep(2.0)
        if not pushed:
            print("PUSH FAIL %s" % target)
            ok = False
    ser.close()
    print("RESULT %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
