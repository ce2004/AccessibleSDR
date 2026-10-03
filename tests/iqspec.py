"""Coarse spectrum of a cu8 IQ file: power every 25 kHz across the band, relative to the median.
usage: iqspec.py file sample_rate"""
import sys
import numpy as np
fs = float(sys.argv[2]); nfft = 4096
raw = np.fromfile(sys.argv[1], np.uint8, count=int(fs * 2 * 2))[nfft * 8:]
iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
print(f"I mean {raw[0::2].mean():.1f}, Q mean {raw[1::2].mean():.1f}, I std {raw[0::2].std():.1f}, clipped {np.mean((raw == 0) | (raw == 255)) * 100:.3f}%")
n = len(iq) // nfft
s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:n * nfft].reshape(n, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
f = (np.arange(nfft) - nfft / 2) * fs / nfft
db = 10 * np.log10(s + 1e-12); med = np.median(db)
for c in np.arange(-fs / 2 + 25e3, fs / 2, 25e3):
    sel = np.abs(f - c) < 12.5e3
    v = db[sel].mean() - med
    print(f"{c / 1e3:+7.0f} kHz {v:5.1f} {'#' * max(0, int(v))}")
