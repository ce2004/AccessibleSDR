"""Turn acarsdec's test.wav (12 kHz, one AM-demodulated ACARS channel per WAV channel) into the
radio app's feed for acarsdec --stdin: raw s16le mono at 48000 Hz, one file per channel."""
import sys, wave
import numpy as np

src, prefix = sys.argv[1], sys.argv[2]
rate = int(sys.argv[3]) if len(sys.argv) > 3 else 48000
w = wave.open(src)
nch, fs = w.getnchannels(), w.getframerate()
assert w.getsampwidth() == 2
d = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, nch).astype(np.float64)
for c in range(nch):
    x = d[:, c]
    n = len(x)
    n_out = n * rate // fs
    X = np.fft.rfft(x)
    Y = np.zeros(n_out // 2 + 1, dtype=complex)
    m = min(len(X), len(Y))
    Y[:m] = X[:m]
    y = np.fft.irfft(Y, n_out) * (n_out / n)
    y = np.clip(np.round(y), -32768, 32767).astype('<i2')
    y.tofile(f"{prefix}{c}.s16")
    print(f"ch{c}: {n_out} samples at {rate} Hz -> {prefix}{c}.s16")
