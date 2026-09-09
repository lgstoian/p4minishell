import sys
import time
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port

PORT = default_port()
BAUD = 115200

ser = open_port(PORT, BAUD, timeout=1)
ser.reset_input_buffer()

print('Waiting 10 seconds for boot to complete...')
time.sleep(10)
ser.reset_input_buffer()

def drain(timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()

print('\n>> wifi connect 4G-CPE_5542 1234567890')
ser.write(b'wifi connect 4G-CPE_5542 1234567890\r\n')
ser.flush()

print('Waiting 30 seconds for connection...')
drain(30)
ser.close()
print('\nDone.')
