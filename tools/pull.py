#!/usr/bin/env python3
"""pull.py - download an SD file via `send` (SDFX framing).

Usage: python pull.py <remote-path> <local-out> [COMx]
"""
import struct
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell


def main():
    remote = sys.argv[1]
    local = sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else "COM3"
    sh = Shell(port)
    time.sleep(12)
    sh.s.timeout = 1
    sh.s.reset_input_buffer()
    sh.s.write(("send %s\r\n" % remote).encode())
    data = b""
    end = time.time() + 120
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            i = data.find(b"SDFX")
            if i >= 0 and len(data) >= i + 8:
                size = struct.unpack("<I", data[i + 4:i + 8])[0]
                print("frame size=%d" % size, flush=True)
                while len(data) < i + 8 + size and time.time() < end:
                    more = sh.s.read(65536)
                    if more:
                        data += more
                    else:
                        time.sleep(0.2)
                with open(local, "wb") as f:
                    f.write(data[i + 8:i + 8 + size])
                print("saved %s" % local, flush=True)
                break
    sh.close()


main()
