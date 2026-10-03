"""Verifies a real FM broadcast is being demodulated: runs rtl_fm with no de-emphasis at a
200 kHz output rate and measures the 19 kHz stereo pilot plus program audio energy.
usage: fmcheck.py freq_MHz [seconds]"""
import os, subprocess, sys
import numpy as np

BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
f, secs = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 2
fs = 200000
p = subprocess.Popen([os.path.join(BIN, "rtl_fm.exe"), "-f", f + "M", "-M", "fm", "-s", str(fs), "-g", "30", "-"],
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
want = int(fs * secs) * 2
buf = bytearray()
while len(buf) < want:
    chunk = p.stdout.read(min(65536, want - len(buf)))
    if not chunk:
        break
    buf += chunk
p.kill(); p.wait()
x = np.frombuffer(bytes(buf[:len(buf) // 2 * 2]), np.int16).astype(np.float32)[fs // 4:]
if len(x) < fs // 2:
    print(f"{f} MHz: FAIL only {len(x)} samples")
    sys.exit(1)
n = 8192
segs = len(x) // n
spec = np.mean(np.abs(np.fft.rfft(x[:segs * n].reshape(segs, n) * np.hanning(n), axis=1)) ** 2, axis=0)
fr = np.fft.rfftfreq(n, 1 / fs)
def band(a, b): return 10 * np.log10(spec[(fr >= a) & (fr < b)].mean() + 1e-9)
pilot = 10 * np.log10(spec[np.abs(fr - 19000) < 50].max() + 1e-9)
around = band(17000, 18500)
# 75-88 kHz sits between the SCA subcarriers (67 / 92 kHz) that public stations often carry
audio, quiet = band(200, 15000), band(75000, 88000)
stereo = pilot - around > 15
ok = stereo or audio - quiet > 10
print(f"{f} MHz: {'stereo' if stereo else 'mono  '} pilot {pilot - around:5.1f} dB, program audio {audio - quiet:5.1f} dB above empty spectrum -> {'PASS' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
