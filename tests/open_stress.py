"""Runs rtl_sdr N times back to back; saves the libusb debug log of the first failure.
usage: open_stress.py N [freq_Hz] [gain] [samples]   (set DBG=4 for libusb debug)"""
import os, subprocess, sys, collections
BIN = os.path.join(os.path.dirname(__file__), "..", "bin")
n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
freq = sys.argv[2] if len(sys.argv) > 2 else "100000000"
gain = sys.argv[3] if len(sys.argv) > 3 else None
samples = sys.argv[4] if len(sys.argv) > 4 else "240000"
env = dict(os.environ, LIBUSB_DEBUG=os.environ.get("DBG", "0"))
codes = collections.Counter()
saved = False
for i in range(n):
    cmd = [os.path.join(BIN, "rtl_sdr.exe"), "-f", freq, "-s", "2400000", "-n", samples]
    if gain:
        cmd += ["-g", gain]
    r = subprocess.run(cmd + [os.path.join(os.environ["TEMP"], "stress.cu8")], capture_output=True, text=True, env=env)
    codes[r.returncode] += 1
    if r.returncode != 0 and not saved:
        open(os.path.join(os.environ["TEMP"], "stress_fail.txt"), "w").write(r.stderr)
        saved = True
print(f"{codes[0]}/{n} ok; exit codes: {dict(codes)}")
