"""Summarises 48 kHz stereo s16 audio captured with wfm --tee. usage: audiostats.py file.raw"""
import sys
import numpy as np
fs = 48000
x = np.fromfile(sys.argv[1], np.int16).astype(np.float32).reshape(-1, 2)[fs:]
l, r = x[:, 0], x[:, 1]
def db(v): return 20 * np.log10(v + 1e-9)
print(f"{len(x) / fs:.1f}s  L rms {db(np.sqrt(np.mean(l**2)) / 32768):.1f} dBFS, R rms {db(np.sqrt(np.mean(r**2)) / 32768):.1f} dBFS, "
      f"peak {np.abs(x).max():.0f}, L/R correlation {np.corrcoef(l, r)[0, 1]:.2f}")
m = (l + r) / 2
n = 8192; k = len(m) // n
s = 10 * np.log10(np.mean(np.abs(np.fft.rfft(m[:k * n].reshape(k, n) * np.hanning(n), axis=1)) ** 2, axis=0) + 1e-9)
fr = np.fft.rfftfreq(n, 1 / fs)
ref = np.mean(s[(fr > 300) & (fr < 3000)])
for a, b in [(50, 300), (300, 3000), (3000, 8000), (8000, 15000), (15500, 18500), (18800, 19200), (19500, 23000)]:
    print(f"  {a:>5}-{b:<5} Hz: {np.mean(s[(fr >= a) & (fr < b)]) - ref:6.1f} dB vs voice band")
