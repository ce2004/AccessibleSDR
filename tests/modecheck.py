"""Checks rtl_fm's AM, USB, LSB and narrow-FM demodulators against signals with known content.
  am  : WWV 10 MHz AM carries a continuous 100 Hz time code -> audio line at 100 Hz
  usb : tuned 1 kHz below WWV's carrier in USB -> carrier lands at 1000 Hz
  lsb : tuned 1 kHz above WWV's carrier in LSB -> carrier lands at 1000 Hz
  nfm : NOAA weather 162.550 vs an empty channel -> FM quieting of the 3.5-5.5 kHz hiss band"""
import os, subprocess, sys
import numpy as np

BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
RATE = 12000

def capture(freq_hz, mode, secs=4, gain="40"):
    p = subprocess.Popen([os.path.join(BIN, "rtl_fm.exe"), "-f", str(int(freq_hz)), "-M", mode, "-s", str(RATE),
                          "-g", gain, "-"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    want, buf = int(RATE * secs) * 2, bytearray()
    while len(buf) < want:
        c = p.stdout.read(min(16384, want - len(buf)))
        if not c:
            break
        buf += c
    p.kill(); p.wait()
    return np.frombuffer(bytes(buf[:len(buf) // 2 * 2]), np.int16).astype(np.float32)[RATE // 2:]

def spectrum(x, n=4096):
    segs = len(x) // n
    s = np.mean(np.abs(np.fft.rfft(x[:segs * n].reshape(segs, n) * np.hanning(n), axis=1)) ** 2, axis=0)
    return np.fft.rfftfreq(n, 1 / RATE), 10 * np.log10(s + 1e-9)

def tone_check(name, freq_hz, mode, tone):
    fr, s = spectrum(capture(freq_hz, mode))
    peak = s[np.abs(fr - tone) < 40].max()   # allows ~2 ppm tuning error at 10 MHz
    ref = np.median(s[(np.abs(fr - tone) > 60) & (np.abs(fr - tone) < 400)])
    ok = peak - ref > 15
    print(f"{name:4} {freq_hz / 1e6:.4f} MHz: {tone} Hz line {peak - ref:5.1f} dB above neighbours -> {'PASS' if ok else 'FAIL'}")
    return ok

def nfm_check(station, empty):
    def hiss(f):
        fr, s = spectrum(capture(f, "fm", gain="30"))
        return np.mean(s[(fr > 3500) & (fr < 5500)])
    a, b = hiss(station), hiss(empty)
    ok = b - a > 10
    print(f"nfm  {station / 1e6:.4f} MHz: hiss {b - a:5.1f} dB quieter than empty {empty / 1e6:.4f} MHz -> {'PASS' if ok else 'FAIL'}")
    return ok

if __name__ == "__main__":
    results = [
        tone_check("am", 10.0e6, "am", 100),
        tone_check("usb", 9.999e6, "usb", 1000),
        tone_check("lsb", 10.001e6, "lsb", 1000),
        nfm_check(162.550e6, 162.500e6),
    ]
    print(f"{sum(results)}/{len(results)} mode checks passed")
    sys.exit(0 if all(results) else 1)
