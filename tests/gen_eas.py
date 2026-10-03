"""Generates an EAS/SAME header (AFSK 520.83 baud, mark 2083.3 Hz, space 1562.5 Hz) as raw
s16le mono at 22050 Hz, sent 3 times like the real thing. usage: gen_eas.py out.raw [message]"""
import sys
import numpy as np
fs = 22050
msg = sys.argv[2] if len(sys.argv) > 2 else "ZCZC-WXR-TOR-020173+0030-2761900-KICT/NWS-"
baud, mark, space = 520.83, 2083.3, 1562.5
data = bytes([0xAB] * 16) + msg.encode("ascii")          # preamble then header, LSB first
bits = [(b >> i) & 1 for b in data for i in range(8)]
spb = fs / baud
phase, out = 0.0, []
for n_bit, bit in enumerate(bits):
    f = mark if bit else space
    n = int(round((n_bit + 1) * spb)) - int(round(n_bit * spb))
    for _ in range(n):
        phase += 2 * np.pi * f / fs
        out.append(np.sin(phase))
burst = np.array(out)
silence = np.zeros(fs)
sig = np.concatenate([silence, burst, silence, burst, silence, burst, silence])
sig += np.random.default_rng(1).normal(0, 0.05, len(sig))    # a little noise
(sig * 12000).astype(np.int16).tofile(sys.argv[1])
print(f"{len(sig) / fs:.1f}s written")
