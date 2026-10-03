"""Reads an rtl_power CSV and lists FM channels that stand above the noise floor."""
import csv, sys, math

bins = {}
for r in csv.reader(open(sys.argv[1])):
    lo, step = float(r[2]), float(r[4])
    for i, v in enumerate(r[6:]):
        try:
            x = float(v)
        except ValueError:
            continue
        if not math.isnan(x):
            bins[lo + step * i] = x
vals = sorted(bins.values())
floor = vals[len(vals) // 4]
found = []
for k in range(101):
    f = 87.9e6 + 0.2e6 * k
    near = [v for b, v in bins.items() if abs(b - f) <= 60e3]
    if near and max(near) - floor > 10:
        found.append((round(f / 1e6, 1), round(max(near) - floor, 1)))
print("noise floor dB", round(floor, 1))
print(len(found), "stations (MHz, dB above floor):")
for f, snr in sorted(found, key=lambda x: -x[1]):
    print(f"  {f:6.1f}  {snr:5.1f}")
