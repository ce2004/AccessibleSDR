"""Runs a cu8 recording through tuner.c's half-band (47-tap Blackman, cutoff 372 kHz, /2) and
writes cs16 at 744187.5 Hz for `nrsc5 --iq-input-format cs16`. usage: hbtest.py in.iq out.cs16"""
import sys
import numpy as np
raw = np.fromfile(sys.argv[1], np.uint8)
x = ((raw[0::2].astype(np.float32) - 127.5) + 1j * (raw[1::2].astype(np.float32) - 127.5)) / 127.5
n = 47; fc = 372000 / 1488375; m = np.arange(n) - (n - 1) / 2
h = np.where(m == 0, 2 * fc, np.sin(2 * np.pi * fc * m) / (np.pi * np.where(m == 0, 1, m)))
h = h * (0.42 - 0.5 * np.cos(2 * np.pi * np.arange(n) / (n - 1)) + 0.08 * np.cos(4 * np.pi * np.arange(n) / (n - 1)))
h /= h.sum()
y = np.convolve(x, h, mode="valid")[::2]
out = np.empty(2 * len(y), np.int16)
out[0::2] = np.clip(y.real * 32767, -32768, 32767)
out[1::2] = np.clip(y.imag * 32767, -32768, 32767)
out.tofile(sys.argv[2])
print(f"{len(y)} samples written")
