"""Saves our local changes to each upstream source clone as patches/<name>.patch, and the exact
upstream versions to patches/sources.txt, so fetch-sources.cmd can recreate src/ from scratch."""
import os, subprocess
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src, out = os.path.join(root, "src"), os.path.join(root, "patches")
os.makedirs(out, exist_ok=True)
def git(repo, *a): return subprocess.run(["git", "-C", repo, *a], capture_output=True, text=True, encoding="utf-8").stdout
lines = []
for name in sorted(os.listdir(src)):
    repo = os.path.join(src, name)
    if not os.path.isdir(os.path.join(repo, ".git")):
        continue
    url, commit = git(repo, "remote", "get-url", "origin").strip(), git(repo, "rev-parse", "HEAD").strip()
    lines.append(f"{name}|{url}|{commit}")
    git(repo, "add", "-N", ".")                       # include new files we added (intent-to-add)
    diff = git(repo, "diff", "HEAD", "--binary")
    path = os.path.join(out, f"{name}.patch")
    if diff.strip():
        open(path, "w", encoding="utf-8", newline="\n").write(diff)
        files = [l[6:] for l in diff.splitlines() if l.startswith("+++ b/")]
        print(f"{name}: {len(files)} files: {', '.join(files)}")
    elif os.path.exists(path):
        os.remove(path)
open(os.path.join(out, "sources.txt"), "w", newline="\n").write(
    "# name|upstream|commit  (fetch-sources.cmd clones these, then applies <name>.patch if present)\n" + "\n".join(lines) + "\n")
