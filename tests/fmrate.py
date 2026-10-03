"""Measures rtl_fm's real output rate and audio health in wbfm mode (no playback).
usage: fmrate.py MHz [seconds] [extra rtl_fm args...]"""
import os, subprocess, sys, time
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
f, secs = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 10
extra = sys.argv[3:]
p = subprocess.Popen([os.path.join(BIN, "rtl_fm.exe"), "-f", f + "M", "-M", "wbfm", "-g", "20.7", *extra, "-"],
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
buf = bytearray(); t0 = None
while True:
    c = p.stdout.read(4096)
    if not c:
        break
    if t0 is None:
        t0 = time.time(); start = len(buf) + len(c)
    buf += c
    if time.time() - t0 > secs:
        break
el = time.time() - t0
p.kill(); _, err = p.communicate()
x = np.frombuffer(bytes(buf[:len(buf) // 2 * 2]), np.int16).astype(np.float32)
rate = (len(buf) - start) / 2 / el
print(f"output rate {rate:.0f} samples/s over {el:.1f}s (expected 32000)")
print(f"peak {np.abs(x).max():.0f}, rms {np.sqrt(np.mean(x ** 2)):.0f}, clipped {np.mean(np.abs(x) >= 32767) * 100:.3f}%")
print("rtl_fm said:", " | ".join(l for l in err.decode(errors='replace').splitlines() if l.strip())[:600])
