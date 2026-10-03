"""Is the strong line below 93.5 MHz a real signal or a receiver spur? Captures at several tuning
points and lists the strongest narrow lines in absolute RF terms. usage: spur.py MHz [gain]"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fc = float(sys.argv[1]) * 1e6
gain = sys.argv[2] if len(sys.argv) > 2 else "8.7"
fs, nfft = 1488375, 16384
tmp = os.path.join(tempfile.gettempdir(), "spur.cu8")
for tune_off in (0, 100e3, -170e3):
    subprocess.run([os.path.join(BIN, "rtl_sdr.exe"), "-f", str(int(fc + tune_off)), "-s", str(fs), "-g", gain,
                    "-n", str(fs), tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw = np.fromfile(tmp, np.uint8)[65536:]
    iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
    k = len(iq) // nfft
    s = 10 * np.log10(np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:k * nfft].reshape(k, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0) + 1e-12)
    rf = (np.arange(nfft) - nfft / 2) * fs / nfft + fc + tune_off
    # narrow lines: bins far above the local (+-5 kHz) median, inside +-300 kHz of the station
    sel = np.where((np.abs(rf - fc) < 300e3) & (np.abs(rf - fc - tune_off) > 3e3))[0]
    lines = []
    for i in sel:
        lo, hi = max(0, i - 55), min(nfft, i + 55)
        prom = s[i] - np.median(s[lo:hi])
        if prom > 15 and s[i] == s[max(0, i - 3):i + 4].max():
            lines.append((prom, rf[i]))
    lines.sort(reverse=True)
    desc = ", ".join(f"{(f - fc) / 1e3:+.1f} kHz ({p:.0f} dB)" for p, f in lines[:6]) or "none"
    print(f"tuned {tune_off / 1e3:+5.0f} kHz: narrow lines at {desc} (relative to {sys.argv[1]} MHz)")
