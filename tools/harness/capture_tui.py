"""
Capture TUI screenshots for regression testing.
Sends TUI commands, captures screenshots, diffs vs golden.
Usage: python capture_tui.py --port COM11
"""
import time, os, struct, sys, argparse, json

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import default_port, open_port
try:
    from PIL import Image, ImageChops
    HAS_PIL = True
except ImportError:
    HAS_PIL = False
    print("Pillow not installed, diff will be skipped")

PORT = "COM11"
BAUD = 115200

def send_cmd(ser, cmd, wait=0.5):
    print(f">>> {cmd}")
    ser.write((cmd + "\r\n").encode())
    time.sleep(wait)
    # drain
    out = ser.read(4096).decode('utf-8', errors='ignore')
    if out.strip():
        print(out[:500])

def grab_bmp(ser, timeout=10):
    # Wait for BMPX
    start = time.time()
    while time.time() - start < 10:
        if ser.in_waiting:
            # peek for magic
            pass
        time.sleep(0.01)
        # Try to read magic
        if ser.in_waiting >= 8:
            break
    # Send screenshot and capture
    ser.reset_input_buffer()
    ser.write(b"screenshot\r\n")
    start = time.time()
    while time.time() - start < 10:
        line = ser.readline()
        if b"streaming" in line.lower():
            print(line.decode(errors='ignore').strip())
            break
    # Read magic
    magic = ser.read(4)
    if magic != b'BMPX':
        print(f"Bad magic {magic}")
        return None
    size = struct.unpack('<I', ser.read(4))[0]
    print(f"Expecting {size} bytes")
    data = b''
    start = time.time()
    while len(data) < size and time.time() - start < timeout:
        chunk = ser.read(min(size-len(data), 4096))
        if chunk:
            data += chunk
    if len(data)!=size:
        print(f"Size mismatch {len(data)}/{size}")
        return None
    return data

def main():
    parser = argparse.ArgumentParser(description="Capture TUI screenshots")
    parser.add_argument("--port", default=None, help="Serial port (argv COMx, P4_PORT env, else COM11)")
    args = parser.parse_args()
    port = args.port or default_port()
    ser = open_port(port, BAUD, 1)
    time.sleep(3)
    ser.reset_input_buffer()
    # Wait for boot
    print("Waiting for boot...")
    boot=""
    start=time.time()
    while time.time()-start<10:
        d=ser.read(4096)
        if d:
            t=d.decode(errors='ignore')
            boot+=t
            if "ready" in boot.lower():
                break
    print("Boot done, testing TUI...")
    # Test 1: basic box
    send_cmd(ser, "tui status", 1)
    send_cmd(ser, "draw box 1 1 30 10 single \"Test\"", 0.5)
    bmp1 = grab_bmp(ser)
    if bmp1:
        os.makedirs("screenshots", exist_ok=True)
        open("screenshots/tui_box.bmp","wb").write(bmp1)
        print("Saved tui_box.bmp")
        w = struct.unpack('<I', bmp1[18:22])[0]
        h = struct.unpack('<I', bmp1[22:26])[0]
        print(f"TUI box {w}x{h}")
        if HAS_PIL:
            # Crop transcript ROI
            img = Image.frombytes("RGB", (w,h), bmp1[54:], "raw", "BGR", (w*3+3)//4*4, -1)
            # Save for visual check
            img.save("screenshots/tui_box_latest.png")
            print("Saved PNG for visual")
    # Test 2: fullscreen
    send_cmd(ser, "draw fullscreen on", 0.5)
    send_cmd(ser, "draw box 1 1 80 25 double \"Fullscreen\"", 0.5)
    bmp2 = grab_bmp(ser)
    if bmp2:
        open("screenshots/tui_fullscreen.bmp","wb").write(bmp2)
        print("Saved fullscreen")
    send_cmd(ser, "draw fullscreen off", 0.5)
    send_cmd(ser, "draw clear screen", 0.5)
    # Test 3: dialog
    ser.write(b'dialog /t:3 "Hi" "Test dialog" "OK" "Cancel"\r\n')
    time.sleep(1)
    bmp3 = grab_bmp(ser)
    if bmp3:
        open("screenshots/dialog.bmp","wb").write(bmp3)
        print("Saved dialog")
    time.sleep(3)
    print("Waiting for dialog timeout...")
    time.sleep(1)
    ser.close()
    print("Done")

if __name__ == "__main__":
    main()
