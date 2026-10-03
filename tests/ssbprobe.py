"""Where does WWV's carrier land in rtl_fm USB/LSB output when tuned 1 kHz either side?"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from modecheck import capture, spectrum

for mode in ("usb", "lsb"):
    for f in (9.999e6, 10.001e6):
        fr, s = spectrum(capture(f, mode))
        sel = fr > 200
        top = np.argsort(s[sel])[-3:][::-1]
        print(f"{mode} at {f / 1e6:.3f} MHz: strongest audio lines {[int(fr[sel][i]) for i in top]} Hz, "
              f"{s[sel][top[0]] - np.median(s[sel]):.1f} dB over median")
