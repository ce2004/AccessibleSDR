"""Feed a raw 48k s16le file into dsd-neo through a real pipe, in small paced chunks
(like a live radio app would), and collect stdout audio as it arrives."""
import subprocess, sys, threading, time

exe = r"C:\Users\Conner\Documents\SDR\bin\dsd-neo.exe"
src = sys.argv[1]
speed = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
data = open(src, "rb").read()
p = subprocess.Popen([exe, "-fa", "-i", "-", "-s", "48000", "-o", "-", "--stdout-mono"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
out = bytearray()
first = [None]
t0 = time.time()

def rd():
    while True:
        b = p.stdout.read1(4096) if hasattr(p.stdout, "read1") else p.stdout.read(4096)
        if not b:
            break
        if first[0] is None:
            first[0] = time.time() - t0
        out.extend(b)

err = []
th = threading.Thread(target=rd); th.start()
te = threading.Thread(target=lambda: err.append(p.stderr.read())); te.start()
chunk = 48000 * 2 // 50  # 20 ms
for i in range(0, len(data), chunk):
    p.stdin.write(data[i:i + chunk]); p.stdin.flush()
    time.sleep(0.02 / speed)
p.stdin.close()
p.wait(); th.join(); te.join()
lines = err[0].decode(errors="replace").splitlines()
print("rc", p.returncode, "out bytes", len(out), "first audio after %.2fs" % (first[0] or -1),
      "sync lines", sum("Sync:" in l and "no sync" not in l for l in lines))
