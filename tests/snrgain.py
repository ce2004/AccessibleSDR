"""Station power over the noise floor vs tuner gain, at wfm's settings.
Noise floor = 10th percentile of spectrum bins in the capture. usage: snrgain.py MHz"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fc = float(sys.argv[1]) * 1e6
fs, nfft, off = 960000, 4096, 240e3
tmp = os.path.join(tempfile.gettempdir(), "snrgain.cu8")
for g in ["12.5", "20.7", "29.7", "38.6", "44.5", "49.6"]:
    subprocess.run([os.path.join(BIN, "rtl_sdr.exe"), "-f", str(int(fc + off)), "-s", str(fs), "-g", g,
                    "-n", str(fs // 2), tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw = np.fromfile(tmp, np.uint8)[65536:]
    clip = np.mean((raw <= 1) | (raw >= 254)) * 100
    iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
    k = len(iq) // nfft
    s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:k * nfft].reshape(k, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
    f = (np.arange(nfft) - nfft / 2) * fs / nfft - off
    ch = s[np.abs(f) <= 100e3]
    floor = np.percentile(s[np.abs(f + off) > 10e3], 10)
    print(f"gain {g:>4}: station {10 * np.log10(ch.sum() / (floor * len(ch))):5.1f} dB over noise floor, clipped {clip:.2f}%")
