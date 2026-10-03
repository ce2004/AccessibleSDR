"""Finds brief noise bursts in 48 kHz stereo s16 audio: per-5 ms energy above 6 kHz compared with
the surrounding second. Also reports how much of the audio is L-R (stereo) noise.
usage: clicks.py file.raw"""
import sys
import numpy as np
fs = 48000
x = np.fromfile(sys.argv[1], np.int16).astype(np.float32).reshape(-1, 2)[fs // 2:]
m = (x[:, 0] + x[:, 1]) / 2
d = (x[:, 0] - x[:, 1]) / 2
hp = np.diff(np.diff(m))                 # crude high-pass: emphasises clicks and hiss
blk = fs // 200
e = np.array([np.mean(hp[i:i + blk] ** 2) for i in range(0, len(hp) - blk, blk)]) + 1e-6
events = []
for i in range(len(e)):
    lo, hi = max(0, i - 100), min(len(e), i + 100)
    ratio = 10 * np.log10(e[i] / np.median(e[lo:hi]))
    if ratio > 12:
        t = (i * blk + fs // 2) / fs
        if not events or t - events[-1][0] > 0.05:
            events.append((t, ratio))
print(f"{len(m) / fs:.1f}s analysed; {len(events)} bursts more than 12 dB above their surroundings")
for t, r in events[:30]:
    print(f"  at {t:6.2f}s: +{r:.0f} dB")
print(f"stereo difference (L-R) level relative to mono: {10 * np.log10(np.mean(d ** 2) / np.mean(m ** 2)):.1f} dB")
