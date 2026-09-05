import serial
import sys
import time

PORT = 'COM11'
BAUD = 115200
CYCLES = 5


def open_port():
    ser = serial.Serial(PORT, BAUD, timeout=1)
    ser.dtr = False
    ser.rts = False
    return ser


def wait_for_marker(ser, marker, timeout_s):
    deadline = time.time() + timeout_s
    buffer = b''
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buffer += chunk
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()
            if marker in buffer:
                return True
    return False


def send_command(ser, cmd):
    print(f'\n>> {cmd}')
    ser.write((cmd + '\r\n').encode())
    ser.flush()


# Open once; opening may reset the board. Wait for full Wi-Fi runtime.
ser = open_port()
print('Waiting for Wi-Fi runtime to start...')
if not wait_for_marker(ser, b'Wi-Fi runtime started in STA mode', 45):
    print('ERROR: Wi-Fi runtime did not start on initial boot')
    ser.close()
    sys.exit(1)

results = []
for cycle in range(1, CYCLES + 1):
    print(f'\n\n=== Cycle {cycle}/{CYCLES} ===')
    time.sleep(1)
    send_command(ser, 'wifi connect 4G-CPE_5542 1234567890')
    print('Waiting for IP...')
    got_ip = wait_for_marker(ser, b'got IP', 45)
    results.append(got_ip)
    # Small settle so the crash (if any) surfaces while we can still read it
    time.sleep(3)

    if cycle < CYCLES:
        print('Rebooting for next cycle...')
        send_command(ser, 'reboot')
        # After reboot the board prints boot log then restarts wifi
        if not wait_for_marker(ser, b'Wi-Fi runtime started in STA mode', 45):
            print('ERROR: did not see wifi runtime restart')
            break

print('\n\n=== Results ===')
for i, r in enumerate(results, 1):
    print(f'Cycle {i}: {"PASS" if r else "FAIL"}')
print(f'Success: {sum(results)}/{CYCLES}')
ser.close()
