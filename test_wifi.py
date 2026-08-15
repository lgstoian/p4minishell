import serial
import sys
import time

PORT = 'COM11'
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=1)
ser.dtr = False
ser.rts = False
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
