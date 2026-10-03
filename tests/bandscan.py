"""Power scan using raw rtl_sdr captures + numpy FFT (rtl_power's MSVC build emits NaNs).
usage: bandscan.py start_MHz stop_MHz channel_kHz [gain] [min_dB]
Prints channels whose power stands above the scan's median by min_dB."""
import os, subprocess, sys, tempfile
import numpy as np

BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
start, stop, chan = float(sys.argv[1]) * 1e6, float(sys.argv[2]) * 1e6, float(sys.argv[3]) * 1e3
gain = sys.argv[4] if len(sys.argv) > 4 else "30"
min_db = float(sys.argv[5]) if len(sys.argv) > 5 else 10
fs, nfft, use = 2.4e6, 4096, 1.6e6          # keep the flat middle 1.6 MHz of each tuning
tmp = os.path.join(tempfile.gettempdir(), "bandscan.cu8")
freqs, power = [], []
center = start + use / 2
while center - use / 2 < stop:
    want = int(fs * 0.25)
    for attempt in range(3):
        # rtl_sdr's Windows exit path can report a cancel race (exit 5) or crash after the
        # capture is complete, so judge by the file, not the exit code
        if os.path.exists(tmp):
            os.remove(tmp)
        subprocess.run([os.path.join(BIN, "rtl_sdr.exe"), "-f", str(int(center)), "-s", str(int(fs)),
                        "-g", gain, "-n", str(want), tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if os.path.exists(tmp) and os.path.getsize(tmp) >= want * 2:
            break
    else:
        sys.exit(f"capture at {center / 1e6:.3f} MHz failed 3 times")
    raw = np.fromfile(tmp, np.uint8)[nfft * 2 * 8:]   # drop settling samples
    iq = (raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4)
    n = len(iq) // nfft
    spec = np.mean(np.abs(np.fft.fftshift(np.fft.fft(iq[:n * nfft].reshape(n, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
    f = center + (np.arange(nfft) - nfft / 2) * fs / nfft
    keep = np.abs(f - center) <= use / 2
    keep &= np.abs(f - center) > 3 * fs / nfft      # skip the DC spike
    freqs.append(f[keep]); power.append(10 * np.log10(spec[keep] + 1e-12))
    center += use
freqs, power = np.concatenate(freqs), np.concatenate(power)
floor = np.median(power)
out = []
for c in np.arange(start, stop + 1, chan):
    sel = np.abs(freqs - c) <= chan * 0.3
    if sel.any():
        p = power[sel].max() - floor
        if p >= min_db:
            out.append((c / 1e6, p))
print(f"floor {floor:.1f} dB, {len(out)} channels >= {min_db} dB above it")
for c, p in sorted(out, key=lambda x: -x[1]):
    print(f"  {c:10.4f} MHz  {p:5.1f} dB")
