"""Where does an FM station's centre actually land? Captures at 960 kS/s tuned `offset` above the
station (like wfm) and reports the spectral centroid relative to where the station should be.
usage: fmoffset.py MHz [offset_kHz]"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fc = float(sys.argv[1]) * 1e6
off = float(sys.argv[2]) * 1e3 if len(sys.argv) > 2 else 240e3
fs, nfft = 960000, 8192
tmp = os.path.join(tempfile.gettempdir(), "fmoffset.cu8")
subprocess.run([os.path.join(BIN, "rtl_sdr.exe"), "-f", str(int(fc + off)), "-s", str(fs), "-g", "20.7",
                "-n", str(fs), tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
raw = np.fromfile(tmp, np.uint8)[nfft * 8:]
iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
n = len(iq) // nfft
s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:n * nfft].reshape(n, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
f = (np.arange(nfft) - nfft / 2) * fs / nfft + off      # relative to the station's nominal frequency
sel = np.abs(f) < 150e3
p = s[sel] - np.median(s)
p[p < 0] = 0
centroid = np.sum(f[sel] * p) / np.sum(p)
peak = f[sel][np.argmax(s[sel])]
print(f"{sys.argv[1]} MHz: energy centred {centroid / 1e3:+.1f} kHz from nominal, strongest bin {peak / 1e3:+.1f} kHz")
