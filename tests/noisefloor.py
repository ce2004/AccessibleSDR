"""Absolute noise floor at a frequency for several gains: median FFT bin power (dB, arbitrary but
consistent units) and total rms. Run once with the antenna off and once with it on, then compare.
usage: noisefloor.py MHz label"""
import os, subprocess, sys, tempfile, json
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fc = float(sys.argv[1]) * 1e6
label = sys.argv[2]
fs, nfft = 1488375, 4096
tmp = os.path.join(tempfile.gettempdir(), "noisefloor.cu8")
out = {}
for g in ["16.6", "29.7", "42.1", "49.6"]:
    subprocess.run([os.path.join(BIN, "iqcap.exe"), str(int(fc)), str(fs), g, "0", str(fs // 2), tmp],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw = np.fromfile(tmp, np.uint8)
    iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
    k = len(iq) // nfft
    s = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:k * nfft].reshape(k, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
    f = (np.arange(nfft) - nfft / 2) * fs / nfft
    keep = np.abs(f) > 5e3
    med = 10 * np.log10(np.median(s[keep]))
    top = 10 * np.log10(s[keep].max()) - med
    out[g] = med
    print(f"{label:>10} gain {g:>4}: noise floor {med:6.1f} dB, strongest bin {top:5.1f} dB above floor, rms {iq.std():5.1f}")
path = os.path.join(tempfile.gettempdir(), "noisefloor.json")
d = json.load(open(path)) if os.path.exists(path) else {}
d[label] = out
json.dump(d, open(path, "w"))
if len(d) >= 2 and "off" in d and "on" in d:
    for g in out:
        if g in d["off"] and g in d["on"]:
            print(f"gain {g:>4}: antenna raises the floor by {d['on'][g] - d['off'][g]:5.1f} dB")
