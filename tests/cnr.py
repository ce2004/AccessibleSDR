"""Carrier-to-noise ratio of FM stations: channel power (+-100 kHz) vs the receiver's noise floor
measured in the emptiest parts of the same capture, both scaled to the same bandwidth.
usage: cnr.py MHz [MHz ...]"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fs, nfft, off = 2400000, 8192, 600e3
tmp = os.path.join(tempfile.gettempdir(), "cnr.cu8")
for mhz in sys.argv[1:]:
    fc = float(mhz) * 1e6
    subprocess.run([os.path.join(BIN, "rtl_sdr.exe"), "-f", str(int(fc + off)), "-s", str(fs), "-g", "20.7",
                    "-n", str(fs // 2), tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw = np.fromfile(tmp, np.uint8)[nfft * 8:]
    iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
    n = len(iq) // nfft
    s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:n * nfft].reshape(n, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
    f = (np.arange(nfft) - nfft / 2) * fs / nfft - off
    ch = s[np.abs(f) <= 100e3]
    usable = (np.abs(f + off) < 1.0e6) & (np.abs(f + off) > 20e3)     # flat part of the capture, no DC
    noise_bin = np.percentile(s[usable], 10)                              # emptiest 10% of bins = noise floor
    cnr = 10 * np.log10(ch.sum() / (noise_bin * len(ch)))
    verdict = "clean stereo" if cnr > 40 else "clean mono, some stereo hiss" if cnr > 25 else "noisy" if cnr > 12 else "very weak"
    print(f"{float(mhz):6.1f} MHz: carrier-to-noise {cnr:5.1f} dB -> {verdict}")
