"""Convert sigidwiki 'AIS IQ.wav' (48 kHz complex, one AIS channel at baseband) into the
radio app's feed: cs16 LE at 248062.5 S/s centred on 162.000 MHz, with the recording placed
on channel A (161.975, -25 kHz) and again on channel B (162.025, +25 kHz) after it."""
import sys, wave
import numpy as np


def fft_resample(z, n_out):
    """Band-limited upsampling by zero-padding the spectrum (no scipy here)."""
    n = len(z)
    Z = np.fft.fft(z)
    P = np.zeros(n_out, dtype=complex)
    h = n // 2
    P[:h] = Z[:h]
    P[-(n - h):] = Z[h:]
    return np.fft.ifft(P) * (n_out / n)


src, dst = sys.argv[1], sys.argv[2]
w = wave.open(src)
assert w.getnchannels() == 2 and w.getsampwidth() == 2
fs_in = w.getframerate()
d = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2).astype(np.float64)
z = d[:, 0] + 1j * d[:, 1]
# 48000 * 1323 / 256 = 248062.5
assert fs_in == 48000
y = fft_resample(z, len(z) * 1323 // 256)
fs = 48000 * 1323 / 256
n = np.arange(len(y))
a = y * np.exp(-2j * np.pi * 25000 * n / fs)   # channel A
b = y * np.exp(+2j * np.pi * 25000 * n / fs)   # channel B
out = np.concatenate([a, b])
out = out / np.max(np.abs(out)) * 12000
iq = np.empty(2 * len(out), dtype='<i2')
iq[0::2] = np.round(out.real)
iq[1::2] = np.round(out.imag)
iq.tofile(dst)
print(f"{len(out)} samples, {len(out)/fs:.1f} s at {fs} S/s -> {dst}")
