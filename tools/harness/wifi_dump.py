import serial
import time
import sys

port = 'COM11'
baud = 115200

print(f"Opening {port} at {baud} baud...")
with serial.Serial(port, baud, timeout=1) as ser:
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    
    print("Dumping serial output for 40 seconds...")
    deadline = time.time() + 40
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
    
    print("\n[DUMP COMPLETE]")
