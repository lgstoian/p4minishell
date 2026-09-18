#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""push_templates.py - push document templates to sd:/TEMPLATES/.

Templates are new-file seeds used by `edit <file> /template <name>`
(writerdeck). Each apps/templates/<NAME>.MD lands at sd:/TEMPLATES/<NAME>.MD.

Usage: python push_templates.py [COMx]
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import push_file, wait_shell, open_port  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = os.path.join(HERE, "templates")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    names = [n for n in sorted(os.listdir(TEMPLATES)) if n.endswith(".MD")]
    if not names:
        print("FAIL: no templates in %s" % TEMPLATES)
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
    ser.write(b"md TEMPLATES\n")
    time.sleep(0.5)
    ser.read(ser.in_waiting or 1)

    ok = True
    for name in names:
        with open(os.path.join(TEMPLATES, name), "rb") as f:
            data = f.read()
        target = "TEMPLATES/" + name
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
