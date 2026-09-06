"""BSOD watcher: timestamped serial capture for 25 minutes."""
import serial
import time

s = serial.Serial("COM11", 115200, timeout=1)
s.dtr = False
s.rts = False
t0 = time.time()
with open("bsod_watch.log", "wb") as f:
    while time.time() - t0 < 25 * 60:
        d = s.read(4096)
        if d:
            f.write(("[%.1f] " % (time.time() - t0)).encode() + d + b"\n")
            f.flush()
s.close()
print("watch done")
