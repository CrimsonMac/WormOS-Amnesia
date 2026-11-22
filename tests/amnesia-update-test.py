#!/usr/bin/env python3
"""Check amnesia-update on the build machine: update an old ISO to a new one.

The updater is built in its test mode (-DAMNESIA_UPDATE_TEST: URL, key,
work folder, current version and "stick" come from the environment, and
the stick is an ISO file). A local HTTP server that honours Range requests
serves the new ISO and its manifest, signed with a throwaway key. Checks:
a tampered manifest is refused, the old ISO is turned into the new one
byte for byte (reporting how much was downloaded), and an up-to-date
system says so.

--synthetic makes the two files itself (a 48 MB "old ISO" of random data
and a "new" one with data inserted, changed and appended), so the test
runs anywhere, as CI does. Monocypher's sources come from the build
(build/work/monocypher or build/host/monocypher) or MONOCYPHER_SRC.

Usage: tests/amnesia-update-test.py OLD.iso NEW.iso
       tests/amnesia-update-test.py --synthetic
"""

import http.server
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
MONO = next((d for d in (os.environ.get("MONOCYPHER_SRC", ""),
                         os.path.join(BUILD, "work", "monocypher", "src"),
                         os.path.join(BUILD, "host", "monocypher", "src"))
             if d and os.path.isfile(os.path.join(d, "monocypher.c"))), None)


def synthetic(tmp):
    """A random "old ISO" and a "new" one sharing most of its blocks."""
    rnd = random.Random(1)
    old = bytearray(rnd.randbytes(48 << 20))
    new = bytearray(old[:5 << 20] + rnd.randbytes(3 << 20) + old[5 << 20:])  # inserted, the rest shifted
    new[30 << 20:31 << 20] = rnd.randbytes(1 << 20)                # changed
    new += rnd.randbytes(2 << 20)                                   # appended
    for data in (old, new):
        # A primary volume descriptor, as the updater looks for, with the size.
        pvd = bytearray(2048)
        pvd[0:6] = b"\x01CD001"
        pvd[80:84] = (len(data) // 2048).to_bytes(4, "little")
        data[16 * 2048:17 * 2048] = pvd
    paths = os.path.join(tmp, "old.iso"), os.path.join(tmp, "new.iso")
    for path, data in zip(paths, (old, new)):
        with open(path, "wb") as f:
            f.write(data)
    return paths


REQUESTS = []


class Ranges(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        REQUESTS.append(self.path)
        path = self.translate_path(self.path)
        r = self.headers.get("Range")
        if not r or not os.path.isfile(path):
            return super().do_GET()
        a, b = map(int, re.match(r"bytes=(\d+)-(\d+)", r).groups())
        with open(path, "rb") as f:
            f.seek(a)
            data = f.read(b - a + 1)
        self.send_response(206)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Range", f"bytes {a}-{b}/{os.path.getsize(path)}")
        self.end_headers()
        self.wfile.write(data)


def main():
    if not MONO:
        sys.exit("Monocypher's sources were not found: build first, or set MONOCYPHER_SRC")
    tmp = tempfile.mkdtemp(prefix="amnesia-update-")
    if sys.argv[1:] == ["--synthetic"]:
        old, new = synthetic(tmp)
    elif len(sys.argv) == 3:
        old, new = sys.argv[1:3]
    else:
        sys.exit(__doc__)
    srv, work = os.path.join(tmp, "srv"), os.path.join(tmp, "work")
    os.makedirs(srv)
    os.makedirs(work)
    exe = os.path.join(tmp, "amnesia-update")
    subprocess.run(["gcc", "-w", "-DAMNESIA_UPDATE_TEST", "-I", f"{ROOT}/src/snap", "-I",
                    f"{ROOT}/src/wh", "-I", MONO, "-I", f"{MONO}/optional", "-o", exe,
                    f"{ROOT}/src/amnesia/update.c", f"{ROOT}/src/wh/sig.c",
                    f"{ROOT}/src/snap/sha256.c", f"{MONO}/monocypher.c",
                    f"{MONO}/optional/monocypher-ed25519.c"], check=True)
    wsign = os.path.join(BUILD, "host", "wsign")
    if not os.access(wsign, os.X_OK):
        wsign = os.path.join(tmp, "wsign")
        subprocess.run(["gcc", "-w", "-I", MONO, "-I", f"{MONO}/optional", "-o", wsign,
                        f"{ROOT}/src/wh/wsign.c", f"{ROOT}/src/wh/sig.c", f"{MONO}/monocypher.c",
                        f"{MONO}/optional/monocypher-ed25519.c"], check=True)
    key, pub = os.path.join(tmp, "key"), os.path.join(tmp, "pub")
    subprocess.run([wsign, "keygen", key, pub], check=True, stdout=subprocess.DEVNULL)
    shutil.copy(new, srv)
    manifest = os.path.join(srv, "amnesia.update")
    subprocess.run([f"{ROOT}/tools/amnesia-manifest", os.path.join(srv, os.path.basename(new)),
                    "99.0", manifest], check=True)
    subprocess.run([wsign, "sign", key, manifest], check=True)
    latest = os.path.join(srv, "amnesia.latest")
    open(latest, "w").write("version = 99.0\n")
    subprocess.run([wsign, "sign", key, latest], check=True)
    handler = lambda *a, **k: Ranges(*a, directory=srv, **k)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    stick = os.path.join(tmp, "stick.iso")
    shutil.copy(old, stick)
    env = dict(os.environ, TEST_URL=f"http://127.0.0.1:{server.server_address[1]}/",
               TEST_KEY=pub, TEST_WORK=work, TEST_VERSION="1.0", TEST_MEDIUM=stick)
    passed = failures = 0

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += bool(ok)
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    def run(args=(), stdin="", **extra):
        p = subprocess.run([exe, *args], input=stdin, capture_output=True, text=True,
                           env=dict(env, **extra))
        return p.returncode, p.stdout + p.stderr

    good = open(manifest).read()
    open(manifest, "w").write(good.replace("version = 99.0", "version = 98.0"))
    rc, out = run(["--yes"])
    check("a tampered manifest is refused", rc != 0 and "signature" in out, out)
    open(manifest, "w").write(good)
    rc, out = run(["--yes"])
    # A stick is larger than any ISO: only the ISO's length counts.
    want = open(new, "rb").read()
    same = open(stick, "rb").read()[:len(want)] == want
    check("the old ISO becomes the new one", rc == 0 and same, out[-500:])
    m = re.search(r"(\d+) of (\d+) blocks are already", out)
    if m:
        print(f"      reused {m.group(1)} of {m.group(2)} blocks "
              f"({100 * int(m.group(1)) // int(m.group(2))}%)")
    m = re.search(r"Wrote (\d+) of (\d+) MB", out)
    if m:
        print(f"      wrote {m.group(1)} of {m.group(2)} MB to the stick")
    rc, out = run(["--check"], TEST_VERSION="99.0")
    check("an up-to-date system says so", rc == 0 and "latest version" in out, out)
    REQUESTS.clear()
    rc, out = run(["--check"])
    check("a check reads only the signed version",
          rc == 0 and "99.0 is available" in out and "/amnesia.update" not in REQUESTS,
          (out, REQUESTS))
    server.shutdown()
    shutil.rmtree(tmp)
    print(f"{passed}/{passed + failures} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
