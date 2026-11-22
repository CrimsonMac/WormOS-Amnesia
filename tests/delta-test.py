#!/usr/bin/env python3
"""Check how wh upgrades: deltas, unchanged files and the download cache.

A second repository is made from out/repo with three packages changed:
zsh (one completion file), tree (rebuilt, nothing changed) and less (one
script); scripts/make-deltas.sh makes deltas for them against out/repo,
as it does against the published repository. The development image
(out/wormos.img, which has out/repo installed) boots, one file less's
delta leaves out is changed first, and wh upgrades from the second
repository. Checks: zsh and tree come as deltas, less as its full package,
every file ends up as the new version has it, files that did not change
keep their inode (left in place), and the download cache is empty after.
Disk writes are discarded. The console output is in build/delta.log.

Usage: tests/delta-test.py [--timeout SECONDS]
"""

import argparse
import hashlib
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
REPO = os.path.join(ROOT, "out", "repo")
WORK = os.path.join(BUILD, "delta-test")
LOG = os.path.join(BUILD, "delta.log")
KEY = os.environ.get("WORMOS_SIGNING_KEY", os.path.expanduser("~/.config/wormos/core.key"))


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tests", file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot_test", "boot-test.py")

# Package: {path: bytes appended} (empty: rebuilt with the same files).
CHANGES = {
    "zsh": {"usr/share/zsh/site-functions/_worm": b"\n# changed by the delta test\n"},
    "tree": {},
    "less": {"usr/libexec/less-osc8-open": b"\n# changed by the delta test\n"},
}


def index_entries(text):
    entries, name = {}, None
    for line in text.splitlines():
        if line.startswith("["):
            name = line[1:-1]
            entries[name] = {}
        elif name and " = " in line:
            k, v = line.split(" = ", 1)
            entries[name][k] = v
    return entries


def rebuild(name, entry, changes):
    """The package with its release raised and the changes made."""
    src = os.path.join(REPO, entry["file"])
    data = subprocess.run(["zstd", "-dcq", src], capture_output=True, check=True).stdout
    tin = tarfile.open(fileobj=io.BytesIO(data))
    members = [(m, tin.extractfile(m).read() if m.isfile() else None) for m in tin.getmembers()]
    contents = {m.name: (c + changes[m.name] if m.name in changes else c) for m, c in members}
    info = contents[".PKGINFO"].decode()
    release = int(re.search(r"^release = (\d+)", info, re.M).group(1)) + 1
    info = re.sub(r"^release = \d+", f"release = {release}", info, flags=re.M)
    for path in changes:
        digest = hashlib.sha256(contents[path]).hexdigest()
        info = re.sub(rf"^file = [0-9a-f]+ {re.escape(path)}$", f"file = {digest} {path}",
                      info, flags=re.M)
    contents[".PKGINFO"] = info.encode()
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tout:
        for m, _ in members:
            c = contents[m.name]
            if c is not None:
                m.size = len(c)
            tout.addfile(m, io.BytesIO(c) if c is not None else None)
    version = re.search(r"^version = (\S+)", info, re.M).group(1)
    file = f"{name}-{version}-{release}.wpkg"
    out = subprocess.run(["zstd", "-q", "-19", "-c"], input=raw.getvalue(),
                         capture_output=True, check=True).stdout
    with open(os.path.join(WORK, "repo", file), "wb") as f:
        f.write(out)
    return f"{version}-{release}", file, hashlib.sha256(out).hexdigest(), len(out)


def make_repo():
    """WORK/repo: out/repo with CHANGES, a newer signed index and deltas."""
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(WORK, "repo"))
    text = open(os.path.join(REPO, "index")).read()
    entries = index_entries(text)
    for f in os.listdir(REPO):
        if f.endswith(".wpkg"):
            os.link(os.path.join(REPO, f), os.path.join(WORK, "repo", f))
    for name, changes in CHANGES.items():
        old = entries[name]
        version, file, digest, size = rebuild(name, old, changes)
        os.unlink(os.path.join(WORK, "repo", old["file"]))
        section = re.search(rf"^\[{re.escape(name)}\]\n(?:[^\[\n].*\n?)*", text, re.M)
        new = (section.group(0)
               .replace(f"version = {old['version']}\n", f"version = {version}\n")
               .replace(f"file = {old['file']}\n", f"file = {file}\n")
               .replace(f"sha256 = {old['sha256']}\n", f"sha256 = {digest}\n")
               .replace(f"size = {old['size']}\n", f"size = {size}\n"))
        text = text[:section.start()] + new + text[section.end():]
    text = re.sub(r"^date = \d+", f"date = {int(time.time())}", text, count=1, flags=re.M)
    index = os.path.join(WORK, "repo", "index")
    open(index, "w").write(text)
    subprocess.run([os.path.join(BUILD, "host", "wsign"), "sign", KEY, index], check=True)
    env = dict(os.environ, DELTA_REPO=os.path.join(WORK, "repo"),
               DELTA_BASE_URL=f"file://{REPO}/", DELTA_WORK=os.path.join(WORK, "base"))
    out = subprocess.run([os.path.join(ROOT, "scripts", "make-deltas.sh")], env=env,
                         capture_output=True, text=True)
    print(out.stdout.strip().splitlines()[-1] if out.stdout.strip() else out.stderr)
    return out.returncode == 0


def sha_of(name, path):
    data = subprocess.run(["zstd", "-dcq", os.path.join(WORK, "repo", files[name])],
                          capture_output=True, check=True).stdout
    info = tarfile.open(fileobj=io.BytesIO(data)).extractfile(".PKGINFO").read().decode()
    return re.search(rf"^file = ([0-9a-f]+) {re.escape(path)}$", info, re.M).group(1)


files = {}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()

    passed = failures = 0

    def check(name, ok, out=""):
        nonlocal passed, failures
        ok = bool(ok)
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    check("deltas made against out/repo", make_repo())
    index = index_entries(open(os.path.join(WORK, "repo", "index")).read())
    for name in CHANGES:
        files[name] = index[name]["file"]
    check("zsh, tree and less have deltas",
          all("delta" in index[n] for n in CHANGES), {n: index[n].get("delta") for n in CHANGES})

    repo = boot_test.serve_repo(os.path.join(WORK, "repo"))
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=dict(os.environ, QEMU_MEM="2048"))
    total = 14

    def login(con, timeout):
        con.expect("login:", timeout)
        con.send("root\n")
        con.expect("# ", 60)
        con.send("exec /bin/sh\n")

    zsh_bin = "/usr/bin/zsh"
    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            login(con, args.timeout)
            inode = con.run(f"stat -c %i {zsh_bin} /usr/bin/tree").split()
            # A file less's delta leaves out, changed on this system.
            con.run("worm unlock >/dev/null 2>&1; echo x >> /usr/libexec/lessecho; "
                    "worm lock >/dev/null 2>&1; echo done")
            out = con.run(f"wh --repo {repo} update", 120)
            check("index with deltas accepted", "Package index updated" in out, out)
            con.send(f"wh --repo {repo} -y upgrade\n")
            out = con.expect("login:", 1800)
            check("all three as changes only first", "3 as changes only" in out, out[-2000:])
            check("less as its full package (a file was changed here)",
                  "less: files of the installed version were changed" in out, out[-2000:])
            check("upgrade applied", "applied the package change" in out, out[-1500:])
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")
            out = con.run("wh list | grep -E '^(zsh|tree|less) '")
            check("new versions installed",
                  all(re.search(rf"^{n}\s+{re.escape(index[n]['version'])}\b", out, re.M)
                      for n in CHANGES), out)
            for name, path in (("zsh", "usr/share/zsh/site-functions/_worm"),
                               ("zsh", "usr/bin/zsh"), ("tree", "usr/bin/tree"),
                               ("less", "usr/libexec/less-osc8-open"),
                               ("less", "usr/libexec/lessecho")):
                out = con.run(f"sha256sum /{path} | cut -c1-64")
                check(f"/{path} is the new version's", sha_of(name, path) in out, out)
            after = con.run(f"stat -c %i {zsh_bin} /usr/bin/tree").split()
            check("unchanged files left in place", inode == after, (inode, after))
            out = con.run("ls /var/cache/wh | wc -l")
            check("download cache emptied", re.search(r"^0$", out, re.M), out)
            con.send("poweroff\n")
            con.expect("Power down", 120)
            proc.wait(timeout=60)
        except (TimeoutError, RuntimeError, OSError, subprocess.TimeoutExpired) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()

    print(f"{passed}/{total} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
