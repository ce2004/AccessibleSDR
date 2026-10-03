"""Fine spectrum of both HD sidebands of a station, mirrored side by side, to spot interference.
usage: sideshape.py MHz [gain] [bw]"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fc = float(sys.argv[1]) * 1e6
g = sys.argv[2] if len(sys.argv) > 2 else "16.6"
bw = sys.argv[3] if len(sys.argv) > 3 else "600000"
fs, nfft, off = 1488375, 16384, 400e3          # tune off-station so neither sideband sits on DC
tmp = os.path.join(tempfile.gettempdir(), "sideshape.cu8")
subprocess.run([os.path.join(BIN, "iqcap.exe"), str(int(fc + off)), str(fs), g, bw, str(fs * 2), tmp],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
raw = np.fromfile(tmp, np.uint8)
iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
k = len(iq) // nfft
s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:k * nfft].reshape(k, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
f = (np.arange(nfft) - nfft / 2) * fs / nfft - off
db = 10 * np.log10(s + 1e-12)
ref = np.median(db[(np.abs(f) > 210e3) & (np.abs(f) < 260e3)])
print(" offset    lower   upper   (dB above the empty space at 210-260 kHz)")
for o in range(100, 265, 5):
    lo = db[np.abs(f + o * 1e3) < 2.5e3].mean() - ref
    hi = db[np.abs(f - o * 1e3) < 2.5e3].mean() - ref
    print(f" {o:4d} kHz  {lo:6.1f}  {hi:6.1f}  {'#' * max(0, int(lo)):<20}|{'#' * max(0, int(hi))}")
