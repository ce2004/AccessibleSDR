"""Recreates src/ (upstream projects at pinned commits, plus our patches), llvm-mingw and the
vcpkg libraries, so a fresh checkout can be built with build-all.cmd."""
import os, subprocess, sys, urllib.request, zipfile, shutil
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def run(*a, cwd=None):
    print(">", " ".join(a), flush=True)
    subprocess.run(a, cwd=cwd, check=True)

# upstream sources at the exact commits we built and tested
for line in open(os.path.join(root, "patches", "sources.txt")):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    name, url, commit = line.split("|")
    dst = os.path.join(root, "src", name)
    if os.path.isdir(os.path.join(dst, ".git")):
        print(f"{name}: already present"); continue
    os.makedirs(dst, exist_ok=True)
    run("git", "init", "-q", dst)
    run("git", "-C", dst, "fetch", "-q", "--depth", "1", url, commit)
    run("git", "-C", dst, "checkout", "-q", "FETCH_HEAD")
    patch = os.path.join(root, "patches", name + ".patch")
    if os.path.exists(patch):
        run("git", "-C", dst, "apply", "--whitespace=nowarn", patch)

# llvm-mingw (clang for the POSIX-style projects; one toolchain targets both ARM64 and x64),
# in the build of the machine we're running on
import platform
host = "aarch64" if platform.machine().upper() in ("ARM64", "AARCH64") else "x86_64"
lm = os.path.join(root, "llvm-mingw")
if not os.path.isdir(lm):
    name = f"llvm-mingw-20260922-ucrt-{host}"
    url = f"https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/{name}.zip"
    z = os.path.join(root, "llvm-mingw.zip")
    print("downloading", name); urllib.request.urlretrieve(url, z)
    zipfile.ZipFile(z).extractall(root); os.remove(z)
    os.rename(os.path.join(root, name), lm)

# vcpkg libraries (MSVC) for the architecture(s) being built: ARCH=arm64, x64, or both (default)
arches = [a for a in os.environ.get("ARCH", "arm64 x64").replace(",", " ").split() if a]
vp = os.path.join(root, "vcpkg")
if not os.path.isdir(vp):
    run("git", "clone", "-q", "--depth", "1", "https://github.com/microsoft/vcpkg.git", vp)
    run("cmd", "/c", "bootstrap-vcpkg.bat", "-disableMetrics", cwd=vp)
for a in arches:
    run(os.path.join(vp, "vcpkg.exe"), "install", "libusb", "pthreads", "fftw3", "libsndfile[core]", "openssl",
        "--triplet", f"{a}-windows", "--clean-after-build")
print("SOURCES OK")
