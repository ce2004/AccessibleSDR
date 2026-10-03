"""Clipping at wfm's exact settings (960 kS/s, tuned +240 kHz) for a station across gains.
usage: wfmclip.py MHz"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fc = float(sys.argv[1]) * 1e6
tmp = os.path.join(tempfile.gettempdir(), "wfmclip.cu8")
row = []
for g in ["4.4", "8.7", "12.5", "16.6", "20.7", "25.4"]:
    subprocess.run([os.path.join(BIN, "rtl_sdr.exe"), "-f", str(int(fc + 240e3)), "-s", "960000", "-g", g,
                    "-n", "300000", tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw = np.fromfile(tmp, np.uint8)[65536:]
    row.append(f"{g}dB {np.mean((raw <= 1) | (raw >= 254)) * 100:.2f}%")
print(f"{sys.argv[1]} MHz clipping: " + ", ".join(row))
