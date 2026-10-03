"""Are the bursts in a cu8 capture LoRa? Finds bursts, then for each measures its bandwidth and
whether its instantaneous frequency sweeps linearly (chirps) across ~250 kHz.
usage: lora_check.py file.cu8 sample_rate centre_offset_hz"""
import sys
import numpy as np
fs = float(sys.argv[2]); off = float(sys.argv[3])
raw = np.fromfile(sys.argv[1], np.uint8)
x = ((raw[0::2].astype(np.float32) - 127.4) + 1j * (raw[1::2].astype(np.float32) - 127.4))
x *= np.exp(-2j * np.pi * off / fs * np.arange(len(x)))          # put the channel at 0 Hz
# low-pass to +-150 kHz and decimate by 4
n = 129; fc = 150e3 / fs; m = np.arange(n) - (n - 1) / 2
h = np.where(m == 0, 2 * fc, np.sin(2 * np.pi * fc * m) / (np.pi * np.where(m == 0, 1, m))) * np.blackman(n)
y = np.convolve(x, h / h.sum(), mode="same")[::4]; fs2 = fs / 4
p = np.abs(y) ** 2
blk = int(fs2 * 0.005)
e = p[:len(p) // blk * blk].reshape(-1, blk).mean(axis=1)
noise = np.median(e)
on = e > noise * 8
bursts = []
i = 0
while i < len(on):
    if on[i]:
        j = i
        while j < len(on) and on[j]: j += 1
        if j - i >= 4: bursts.append((i * blk, j * blk))
        i = j
    else: i += 1
print(f"{len(bursts)} bursts over {len(y) / fs2:.0f} s")
for a, b in bursts[:12]:
    seg = y[a:b]
    f_inst = np.angle(seg[1:] * np.conj(seg[:-1])) * fs2 / (2 * np.pi)
    spec = np.abs(np.fft.fftshift(np.fft.fft(seg, 8192))) ** 2
    fr = (np.arange(8192) - 4096) * fs2 / 8192
    occupied = fr[spec > spec.max() / 20]
    bw = occupied.max() - occupied.min() if len(occupied) else 0
    # chirps: inst. frequency ramps steadily; measure how often it changes direction slowly vs jumps
    sm = np.convolve(f_inst, np.ones(16) / 16, mode="valid")
    span = np.percentile(sm, 98) - np.percentile(sm, 2)
    steps = np.diff(sm[::16])
    rising = np.mean(steps > 0)
    print(f"  burst at {a / fs2:6.2f}s, {1000 * (b - a) / fs2:6.0f} ms, occupied width {bw / 1e3:5.0f} kHz, "
          f"frequency span {span / 1e3:5.0f} kHz, rising {100 * rising:3.0f}% of the time")
