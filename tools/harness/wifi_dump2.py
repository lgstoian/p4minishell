import serial
import time
import sys

port = 'COM11'
baud = 115200

print(f"Opening {port} at {baud} baud...")
ser = serial.Serial(port, baud, timeout=1)
ser.dtr = False
ser.rts = False
ser.reset_input_buffer()

print("Dumping serial output for 40 seconds (no reset)...")
deadline = time.time() + 40
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

ser.close()
print("\n[DUMP COMPLETE]")
