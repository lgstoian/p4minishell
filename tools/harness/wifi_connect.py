import time
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port

port = default_port()
baud = 115200

print(f"Opening {port} at {baud} baud...")
ser = open_port(port, baud, timeout=1)
ser.reset_input_buffer()

print("Waiting for shell prompt...")
deadline = time.time() + 40
buf = b""
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        buf += data
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
        if b"> " in buf:
            print("\n[PROMPT FOUND]")
            break
else:
    print("\n[TIMEOUT waiting for prompt]")
    ser.close()
    sys.exit(1)

# Give a moment for prompt to settle
time.sleep(0.5)

cmd = b"wifi connect 4G-CPE_5542 1234567890\r\n"
print(f"Sending: {cmd.strip().decode()}")
ser.write(cmd)
ser.flush()

print("Reading response for 60 seconds...")
deadline = time.time() + 60
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

ser.close()
print("\n[TEST COMPLETE]")
