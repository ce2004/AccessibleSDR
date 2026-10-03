"""Clipping and signal level vs tuner gain at one frequency. usage: gainsweep.py MHz [rate]"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
f = float(sys.argv[1]) * 1e6
fs = sys.argv[2] if len(sys.argv) > 2 else "1488375"
tmp = os.path.join(tempfile.gettempdir(), "gainsweep.cu8")
for g in ["0", "4.4", "8.7", "12.5", "16.6", "20.7", "25.4", "29.7", "33.8"]:
    if os.path.exists(tmp):
        os.remove(tmp)
    subprocess.run([os.path.join(BIN, "rtl_sdr.exe"), "-f", str(int(f)), "-s", fs, "-g", g, "-n", "400000", tmp],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw = np.fromfile(tmp, np.uint8)[65536:]
    clip = np.mean((raw <= 1) | (raw >= 254)) * 100
    rms = np.sqrt(np.mean((raw.astype(np.float32) - 127.4) ** 2))
    print(f"gain {g:>5} dB: rms {rms:5.1f} of 127, clipped {clip:6.3f}%")
