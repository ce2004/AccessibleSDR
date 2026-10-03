"""Detects HD Radio (IBOC) digital sidebands: hybrid stations carry flat OFDM blocks
~129-198 kHz either side of the analog carrier. Tunes each station at nrsc5's rate and a
gain that doesn't clip. usage: hdscan.py f1 f2 ... (MHz)"""
import os, subprocess, sys, tempfile
import numpy as np

BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fs, nfft = 1488375, 4096
tmp = os.path.join(tempfile.gettempdir(), "hdscan.cu8")
found = []
for mhz in sys.argv[1:]:
    fc = float(mhz) * 1e6
    for _ in range(3):
        if os.path.exists(tmp):
            os.remove(tmp)
        subprocess.run([os.path.join(BIN, "rtl_sdr.exe"), "-f", str(int(fc)), "-s", str(fs), "-g", "16.6",
                        "-n", str(fs // 2), tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if os.path.exists(tmp) and os.path.getsize(tmp) >= fs:
            break
    raw = np.fromfile(tmp, np.uint8)[nfft * 16:]
    iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
    n = len(iq) // nfft
    s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:n * nfft].reshape(n, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
    f = (np.arange(nfft) - nfft / 2) * fs / nfft
    db = 10 * np.log10(s + 1e-12)
    def m(a, b): return np.mean(db[(f >= a) & (f < b)])
    lo, hi = m(-190e3, -140e3), m(140e3, 190e3)
    ref = (m(-280e3, -230e3) + m(230e3, 280e3)) / 2
    hd = lo - ref > 5 and hi - ref > 5
    print(f"{float(mhz):6.1f} MHz: lower sideband {lo - ref:5.1f} dB, upper {hi - ref:5.1f} dB above reference -> {'HD' if hd else 'analog'}")
    if hd:
        found.append(mhz)
print("HD stations:", " ".join(found) if found else "none")
