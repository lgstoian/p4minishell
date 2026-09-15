#!/usr/bin/env python3
"""led_watch.py - webcam verification of the WS2812 status-LED feature set.

Locates the LED in the camera view by cycling colours, calibrates an
exposure-invariant colour reference table, then exercises the full feature set
and classifies each observed frame:

  * solid colours (`rgb off`, `rgb r g b`, `rgb #RRGGBB`)
  * effects (`rainbow`, `breath`, `pulse`, `blink`) animate
  * the auto-status layer (`rgb auto on|off`)
  * transient notifications (`httpd start` -> blue pulse, then back to base)
  * the status colour is live after `rgb auto on` (no stale colour)
  * the boot flash (BOOT_OK green) and the CONFIG.SYS `RGB=` directive

The camera must see the back-panel LED. Uses the Lorgar camera by default
(DirectShow index 1). Run on the *system* Python (cv2), like
display_glitch_watch.py.

Usage: python tools/led_watch.py [COMx] [--camera N]
"""
import argparse
import time
from collections import Counter

import cv2
import numpy as np
import serial

RES = (640, 480)
REF_CMDS = {
    'off': '0 0 0', 'red': '255 0 0', 'green': '0 255 0', 'blue': '0 0 255',
    'amber': '#FF9900', 'cyan': '#00FFFF', 'white': '#FFFFFF',
}


class Led:
    def __init__(self, port, camera):
        self.s = serial.Serial(port, 115200, timeout=0.5)
        time.sleep(0.5)
        self.s.reset_input_buffer()
        self.cap = cv2.VideoCapture(camera, cv2.CAP_DSHOW)
        self.cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, RES[0])
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, RES[1])
        self.cap.set(cv2.CAP_PROP_FPS, 30)
        self.cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.25)
        self.cap.set(cv2.CAP_PROP_AUTO_WB, 0)
        for _ in range(15):
            self.cap.read()
        self.table = {}
        self.roi = (0, 0, 1, 1)

    def send(self, cmd, wait=0.4):
        self.s.reset_input_buffer()
        self.s.write((cmd + '\r\n').encode())
        time.sleep(wait)
        return self.s.read(8192).decode('utf-8', 'replace')

    def frame(self, avg=2):
        acc = None
        for _ in range(avg):
            ok, f = self.cap.read()
            if ok:
                acc = f.astype(np.float32) if acc is None else acc + f.astype(np.float32)
        return (acc / avg).astype(np.float32)

    def sample(self, avg=2):
        f = self.frame(avg)
        x0, y0, x1, y1 = self.roi
        return f[y0:y1, x0:x1].reshape(-1, 3).mean(axis=0)

    def set(self, cmd):
        self.send('rgb ' + cmd, 0.5)

    def reset(self):
        self.s.setDTR(True); self.s.setRTS(True); time.sleep(0.15)
        self.s.setDTR(False); self.s.setRTS(False); time.sleep(0.25)

    def locate(self):
        self.set('off'); off = self.frame(5)
        self.set('255 0 0'); red = self.frame(5)
        self.set('0 255 0'); grn = self.frame(5)
        self.set('0 0 255'); blu = self.frame(5)
        self.set('off')
        d = (np.abs(red - off) + np.abs(grn - off) + np.abs(blu - off)).sum(axis=2)
        ys, xs = np.where(d > d.max() * 0.5)
        self.roi = (int(xs.min() - 5), int(ys.min() - 5), int(xs.max() + 6), int(ys.max() + 6))

    def calibrate(self):
        for name, cmd in REF_CMDS.items():
            self.set(cmd)
            self.table[name] = self.sample(5)
        self.set('off')
        time.sleep(1.2)   # let the sensor AGC settle before the dark check

    def cls(self, b):
        lit_min = min(float(v.sum()) for k, v in self.table.items() if k != 'off')
        if b.sum() < 0.3 * lit_min:
            return 'off'
        n = b / b.sum()
        best, bestd = '?', 1e18
        for k, ref in self.table.items():
            if k == 'off':
                continue
            r = ref / max(1.0, ref.sum())
            dd = float(np.sum((n - r) ** 2))
            if dd < bestd:
                best, bestd = k, dd
        return best

    def lit(self, b):
        return b.sum() >= 0.3 * min(float(v.sum()) for k, v in self.table.items() if k != 'off')

    def dominant(self, b):
        return 'BGR'[int(np.argmax(b))]

    def track(self, sec):
        out = []; t0 = time.time(); last = None
        while time.time() - t0 < sec:
            c = self.cls(self.sample(1))
            if c != last:
                out.append((round(time.time() - t0, 2), c)); last = c
        return out

    def track_raw(self, sec):
        out = []; t0 = time.time()
        while time.time() - t0 < sec:
            out.append((time.time() - t0, self.sample(1)))
        return out

    def connected(self):
        return 'yes' in self.send('wifi status', 0.8).split('connected:')[-1].splitlines()[0]

    def close(self):
        self.cap.release()
        self.s.close()


def weighted(track, sec):
    tot = Counter()
    for i, (t, c) in enumerate(track):
        dt = (track[i + 1][0] - t) if i + 1 < len(track) else (sec - t)
        tot[c] += dt
    return {k: round(v, 2) for k, v in tot.most_common()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?', default='COM3')
    ap.add_argument('--camera', type=int, default=1)
    args = ap.parse_args()

    led = Led(args.port, args.camera)
    results = []

    def check(name, ok, detail=''):
        results.append(ok)
        print('%-52s %s  %s' % (name, 'PASS' if ok else 'FAIL', detail), flush=True)

    for _ in range(20):
        if led.connected():
            break
        time.sleep(2)
    time.sleep(3)
    led.locate()
    led.calibrate()
    print('LED ROI =', led.roi)

    for name, cmd in [('red', '255 0 0'), ('green', '0 255 0'), ('blue', '0 0 255'),
                      ('amber', '#FF9900'), ('cyan', '#00FFFF'), ('white', '#FFFFFF')]:
        led.set(cmd)
        got = led.cls(led.sample(5))
        check('solid %-6s' % name, got == name, 'got %s' % got)
    led.set('0 255 0'); lit = float(led.sample(5).sum())
    led.set('off'); time.sleep(0.4); off = float(led.sample(5).sum())
    check('solid off is dark', off < 0.6 * lit, 'green=%.0f off=%.0f' % (lit, off))

    led.set('255 0 0'); led.set('rainbow')
    hues = set(c for _, c in led.track(2.0))
    check('effect rainbow cycles hues', len(hues) >= 3, 'hues=%s' % sorted(hues))
    led.set('255 0 0'); led.set('breath')
    vals = [led.sample(1).sum() for _ in range(40)]
    check('effect breath varies brightness', (max(vals) - min(vals)) > 0.4 * max(vals),
          'min=%.0f max=%.0f' % (min(vals), max(vals)))
    for eff in ('pulse', 'blink'):
        led.set('255 0 0'); led.set(eff)
        dark = litn = 0
        for _ in range(50):
            v = led.sample(1).sum()
            dark += v < 25; litn += v >= 25
        check('effect %s blinks' % eff, dark > 3 and litn > 3, 'dark=%d lit=%d' % (dark, litn))
    led.set('off')

    led.send('rgb auto off', 0.4)
    check('rgb auto off -> manual', 'manual' in led.send('rgb status', 0.5))
    led.send('rgb auto on', 0.4)
    check('rgb auto on -> auto', 'auto status' in led.send('rgb status', 0.5))

    led.send('rgb auto off', 0.4); led.set('0 0 0'); time.sleep(0.3)
    led.send('httpd stop', 0.8); time.sleep(1.0)
    led.send('httpd start', 0.0)
    raw = led.track_raw(2.6)
    blue = sum(1 for _, b in raw if led.lit(b) and led.dominant(b) == 'B')
    end_off = not led.lit(raw[-1][1])
    check('httpd start -> transient blue pulse then base', blue >= 4 and end_off,
          'blue-lit=%d end_off=%s' % (blue, end_off))
    led.send('httpd stop', 0.0); led.track(1.4)

    for _ in range(10):
        if led.connected():
            break
        time.sleep(2)
    led.send('rgb auto off', 0.4); led.set('0 0 0'); time.sleep(3.0)
    led.send('rgb auto on', 0.4); time.sleep(0.4)
    w = weighted(led.track(2.0), 2.0)
    check('auto on while connected -> green (not stale)', w.get('green', 0) > 0.7,
          'time-weighted=%s' % w)
    led.send('httpd start', 0.3)

    led.reset()
    cls = [c for _, c in led.track(22.0)]
    check('boot: BOOT_OK green flash present', 'green' in cls)
    check('boot: CONFIG.SYS RGB= directive applied', any(c in ('blue', 'cyan') for c in cls))
    check('boot: auto status wifi cue present', any(c in ('amber', 'green') for c in cls))

    led.close()
    print('\n== %d/%d checks passed ==' % (sum(results), len(results)))


if __name__ == '__main__':
    main()
