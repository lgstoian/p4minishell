# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
import time
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port

port = default_port()
baud = 115200

print(f"Opening {port} at {baud} baud...")
with open_port(port, baud, timeout=1) as ser:
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    
    print("Waiting for shell prompt...")
    deadline = time.time() + 30
    buf = b""
    while time.time() < deadline:
        data = ser.read(1024)
        if data:
            buf += data
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
            if b"> " in buf:
                print("\n[PROMPT FOUND]")
                break
    else:
        print("\n[TIMEOUT waiting for prompt]")
        sys.exit(1)
    
    cmd = b"wifi connect <P4_WIFI_SSID> <P4_WIFI_PASSWORD>\r\n"
    print(f"Sending: {cmd.strip().decode()}")
    ser.write(cmd)
    ser.flush()
    
    print("Reading response for 45 seconds...")
    deadline = time.time() + 45
    while time.time() < deadline:
        data = ser.read(1024)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
    
    print("\n[TEST COMPLETE]")
