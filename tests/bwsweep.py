"""HD sideband quality vs tuner IF bandwidth and gain for one station (silent captures).
Reports how far each HD sideband stands above the noise just outside it, plus clipping.
usage: bwsweep.py MHz"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fc = float(sys.argv[1]) * 1e6
fs, nfft = 1488375, 8192
tmp = os.path.join(tempfile.gettempdir(), "bwsweep.cu8")
for bw in [0, 1500000, 1000000, 600000, 400000]:
    row = []
    for g in ["8.7", "16.6", "25.4", "33.8", "42.1"]:
        r = subprocess.run([os.path.join(BIN, "iqcap.exe"), str(int(fc)), str(fs), g, str(bw), str(fs // 2), tmp],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        raw = np.fromfile(tmp, np.uint8)
        clip = np.mean((raw <= 1) | (raw >= 254)) * 100
        iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
        k = len(iq) // nfft
        s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:k * nfft].reshape(k, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
        f = (np.arange(nfft) - nfft / 2) * fs / nfft
        db = 10 * np.log10(s + 1e-12)
        def m(a, b): return np.mean(db[(f >= a) & (f < b)])
        floor = (m(-250e3, -215e3) + m(215e3, 250e3)) / 2
        lo, hi = m(-190e3, -140e3) - floor, m(140e3, 190e3) - floor
        row.append(f"g{g}: {lo:4.1f}/{hi:4.1f}{' CLIP' if clip > 0.05 else ''}")
    print(f"bw {'auto' if bw == 0 else f'{bw // 1000}k':>5}  " + "  ".join(row))
print("(numbers = lower/upper HD sideband dB above adjacent noise; higher is better)")
