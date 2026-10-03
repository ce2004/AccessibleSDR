"""Looks for the 19 kHz stereo pilot folded to 13 kHz in rtl_fm's 32 kHz wbfm output (no playback)."""
import os, subprocess, sys
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fs = 32000
p = subprocess.Popen([os.path.join(BIN, "rtl_fm.exe"), "-f", sys.argv[1] + "M", "-M", "wbfm", "-g", "20.7", "-"],
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
raw = p.stdout.read(fs * 2 * 6); p.kill(); p.wait()
x = np.frombuffer(raw[:len(raw) // 2 * 2], np.int16).astype(np.float32)[fs:]
n = 8192; k = len(x) // n
s = 10 * np.log10(np.mean(np.abs(np.fft.rfft(x[:k * n].reshape(k, n) * np.hanning(n), axis=1)) ** 2, axis=0) + 1e-9)
fr = np.fft.rfftfreq(n, 1 / fs)
peak = s[np.abs(fr - 13000) < 30].max(); around = np.median(s[(fr > 12000) & (fr < 14000)])
print(f"{sys.argv[1]} MHz: tone at 13 kHz is {peak - around:.1f} dB above its surroundings")
