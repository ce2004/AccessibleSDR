"""Looks for sudden events in an IQ recording: per-50ms total power, power in the station's
channel (+-200 kHz) and outside it. Prints only the moments that jump. usage: envelope.py file.iq"""
import sys
import numpy as np
fs = 1488375
raw = np.fromfile(sys.argv[1], np.uint8)
blk = int(fs * 0.05)
n = len(raw) // 2 // blk
nfft = 2048
rows = []
for b in range(n):
    seg = raw[b * blk * 2:(b + 1) * blk * 2]
    iq = (seg[0::2].astype(np.float32) - 127.4) + 1j * (seg[1::2].astype(np.float32) - 127.4)
    k = len(iq) // nfft
    s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:k * nfft].reshape(k, nfft), axis=1), axes=1)) ** 2, axis=0)
    f = (np.arange(nfft) - nfft / 2) * fs / nfft
    inside = 10 * np.log10(s[np.abs(f) < 200e3].sum())
    sb = 10 * np.log10(s[(np.abs(f) > 135e3) & (np.abs(f) < 195e3)].sum())
    outside = 10 * np.log10(s[np.abs(f) > 250e3].sum())
    clip = np.mean((seg <= 1) | (seg >= 254)) * 100
    rows.append((b * 0.05, inside, sb, outside, clip))
a = np.array(rows)
med = np.median(a[:, 1:4], axis=0)
print(f"{n * 0.05:.1f}s recorded. medians: channel {med[0]:.1f}, HD sidebands {med[1]:.1f}, outside {med[2]:.1f} dB")
print("time   channel  sidebands  outside  clip%   (dB vs median; only moments that move > 3 dB)")
for t, i, sb, o, c in rows:
    d = (i - med[0], sb - med[1], o - med[2])
    if max(abs(x) for x in d) > 3 or c > 0.01:
        print(f"{t:5.2f}  {d[0]:+6.1f}  {d[1]:+8.1f}  {d[2]:+7.1f}  {c:5.2f}")
