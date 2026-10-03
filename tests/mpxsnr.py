"""FM multiplex (MPX) levels: demodulates a capture like wfm does and measures program power vs the
noise density in empty parts of the MPX, giving approximate mono and stereo signal-to-noise.
usage: mpxsnr.py MHz [gain]"""
import os, subprocess, sys, tempfile
import numpy as np
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
fc = float(sys.argv[1]) * 1e6
gain = sys.argv[2] if len(sys.argv) > 2 else "16.6"
fs, off = 960000, 240000
tmp = os.path.join(tempfile.gettempdir(), "mpxsnr.cu8")
subprocess.run([os.path.join(BIN, "iqcap.exe"), str(int(fc + off)), str(fs), gain, "0", str(fs * 2), tmp],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
raw = np.fromfile(tmp, np.uint8)
iq = (raw[0::2].astype(np.float32) - 127.5) + 1j * (raw[1::2].astype(np.float32) - 127.5)
iq *= np.exp(2j * np.pi * off / fs * np.arange(len(iq)))
n = 255; fcut = 105000 / fs; m = np.arange(n) - (n - 1) / 2
h = np.where(m == 0, 2 * fcut, np.sin(2 * np.pi * fcut * m) / (np.pi * np.where(m == 0, 1, m))) * np.blackman(n)
h /= h.sum()
y = np.convolve(iq, h, mode="valid")[::4]
mpx = np.angle(y[1:] * np.conj(y[:-1])) * (240000 / (2 * np.pi * 75000))
f2 = 240000; nfft = 8192; k = len(mpx) // nfft
s = np.mean(np.abs(np.fft.rfft(mpx[:k * nfft].reshape(k, nfft) * np.hanning(nfft), axis=1)) ** 2, axis=0)
fr = np.fft.rfftfreq(nfft, 1 / f2)
def dens(a, b): return np.median(s[(fr >= a) & (fr < b)])
def pw(a, b): return s[(fr >= a) & (fr < b)].sum()
for a, b, name in [(16000, 18500, "guard 16-18.5k"), (53500, 55000, "53.5-55k"), (75000, 90000, "75-90k")]:
    print(f"noise density {name:15}: {10 * np.log10(dens(a, b)):6.1f} dB")
mono_noise = dens(16000, 18500) * np.sum((fr >= 50) & (fr < 15000)) / 3   # triangular noise: lower in the audio band
stereo_noise = dens(53500, 55000) * np.sum((fr >= 23000) & (fr < 53000))
prog = pw(50, 15000)
print(f"program (L+R) power {10 * np.log10(prog):.1f} dB, pilot {10 * np.log10(pw(18900, 19100)):.1f} dB, L-R band {10 * np.log10(pw(23000, 53000)):.1f} dB")
print(f"approx mono SNR {10 * np.log10(prog / mono_noise):.1f} dB, stereo SNR {10 * np.log10(prog / (mono_noise + stereo_noise)):.1f} dB")
