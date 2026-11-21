#!/usr/bin/env python3
"""Upgrade an older release image to the packages in out/repo.

The image (such as wormos-1.3-x86_64.qcow2 from an earlier release) boots
with the desktop test's virtual GPU and keyboard; the first-boot setup is
typed on the keyboard, then root logs in on the serial console, points wh
at out/repo (served over HTTP) and runs wh upgrade, the way a user of that
release would. Releases without the edition package (before 1.8) need a
second wh upgrade, with the new wh, to get the programs the standard
edition now comes with; the test runs it, then removes one of them and
checks that upgrades leave it removed. Disk writes are discarded. The
console output is written to build/upgrade.log.

Usage: tests/upgrade-test.py IMAGE [--timeout SECONDS]
"""

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "upgrade.log")


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tests", file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot_test", "boot-test.py")
desktop = load("desktop_test", "desktop-test.py")
USER, USER_PASSWORD, ROOT_PASSWORD = desktop.USER, desktop.USER_PASSWORD, desktop.ROOT_PASSWORD


def version_of(path):
    with open(path) as f:
        return re.search(r"^VERSION_ID=(.*)$", f.read(), re.M).group(1).strip('"')


VERSION = version_of(os.path.join(ROOT, "recipes", "base-files", "files", "etc", "os-release"))

AFTER_CHECKS = [
    ("new version", "grep ^VERSION_ID= /etc/os-release", rf"^VERSION_ID={re.escape(VERSION)}$"),
    ("nothing left to upgrade", "wh --repo {repo} upgrade", r"^Everything is up to date\.$"),
    # Applets that busybox gave up: their packages must have taken over.
    ("replaced commands still there",
     "for c in less tar cpio unzip xz unxz bzip2 bzcat tree lsof vi xxd strings; do "
     "command -v $c >/dev/null || echo missing $c; done; echo done", r"^done$"),
    ("tar is bsdtar", "tar --version | head -1", r"^bsdtar 3"),
    ("busybox left no stale links",
     "grep -c -e ' usr/bin/less$' -e ' usr/bin/tar$' /usr/lib/wh/db/busybox/files",
     r"^0$"),
    ("Tor Browser comes along", "cat /opt/tor-browser/version", r"^\d+\.\d+"),
    ("accounts kept", f"grep -c -e '^{USER}:' -e '^messagebus:' -e '^unbound:' /etc/passwd; "
     f"grep -c '^{USER}:\\$' /etc/shadow", r"^3\n1$"),
    ("root's shell moves to zsh", "grep ^root: /etc/passwd", r":/usr/bin/zsh$"),
    ("system services running", "pidof dbus-daemon unbound | wc -w", r"^[2-9]$"),
    ("the user's files kept", f"ls -d /home/{USER}", rf"^/home/{USER}$"),
    ("still read-only", "touch /usr/x 2>&1", r"Read-only file system"),
    ("the edition's package", "wh list | grep -c '^wormos-standard '", r"^1$"),
    ("the new release's programs arrived",
     "btop --version | head -1; gdb --version | head -1; command -v fastfetch mc 7z",
     r"btop version(.|\n)*GNU gdb(.|\n)*/usr/bin/fastfetch\n/usr/bin/mc\n/usr/bin/7z"),
]

# After the user removes a program the edition recommends.
DECLINED_CHECKS = [
    ("removed program stays removed", "command -v btop || echo gone; "
     "wh --repo {repo} upgrade", r"^gone\n(.|\n)*Everything is up to date\.$"),
    ("remembered as declined", "cat /usr/lib/wh/declined", r"^btop$"),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", help="an older release image (qcow2)")
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds to wait for the login prompt")
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="wormos-upgrade-")
    mon_path = os.path.join(tmp, "monitor")
    extra = (f"-vga none -device virtio-gpu-pci,addr=0x10 "
             f"-device virtio-keyboard-pci,addr=0x11 -device virtio-tablet-pci,addr=0x12 "
             f"-monitor unix:{mon_path},server,nowait")
    env = dict(os.environ, QEMU_EXTRA=extra, QEMU_MEM="3072",
               IMAGE=os.path.abspath(args.image))
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    repo = boot_test.serve_repo()
    passed = failures = 0
    total = len(AFTER_CHECKS) + len(DECLINED_CHECKS) + 5

    def check(name, ok, out=""):
        nonlocal passed, failures
        ok = bool(ok)
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    def root_login(con):
        deadline = time.time() + 600
        while True:
            con.send("root\n")
            con.expect("Password:", 60)
            con.send(ROOT_PASSWORD + "\n")
            try:
                con.expect("# ", 15)
                con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
                return
            except TimeoutError:
                if time.time() > deadline:
                    raise
                con.expect("wormos login:", 60)

    start = time.time()
    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", args.timeout)
            print(f"booted to login prompt in {time.time() - start:.0f}s")
            mon = desktop.Monitor(mon_path)

            # The graphical first-boot setup, as in the desktop test.
            deadline = time.time() + 300
            while time.time() < deadline:
                path = os.path.join(ROOT, "build", "upgrade-setup.ppm")
                mon.cmd(f"screendump {path}")
                time.sleep(1)
                w, h, px = desktop.read_ppm(path)
                if desktop.solid_block(px, w, h, desktop.ACCENT):
                    break
                time.sleep(5)
            mon.key("ret")
            time.sleep(2)
            mon.type("Tester\n")
            time.sleep(1)
            mon.type(USER + "\n")
            for secret in (USER_PASSWORD, USER_PASSWORD):
                time.sleep(1)
                mon.type(secret + "\n")
            time.sleep(2)
            for secret in (ROOT_PASSWORD, ROOT_PASSWORD):
                time.sleep(1)
                mon.type(secret + "\n")
            root_login(con)
            out = con.run("grep ^VERSION_ID= /etc/os-release")
            check("older release set up", "VERSION_ID=" in out and VERSION not in out, out)

            out = con.run(f"wh --repo {repo} update", 120)
            check("package index from out/repo", "Package index updated" in out, out)

            con.send(f"wh --repo {repo} -y upgrade\n")
            out = con.expect("login:", 1800)
            check("upgrade applied", "applied the package change" in out, out[-1500:])
            root_login(con)
            # Releases before the edition package (1.8) upgrade with their
            # own wh, which does not know it: the next upgrade, with the new
            # wh, adds it and the programs it recommends.
            out = con.run("test -e /usr/lib/wh/db/wormos-standard && echo has-edition")
            if "has-edition" in out:
                check("second upgrade not needed", True)
            else:
                con.send(f"wh --repo {repo} -y upgrade\n")
                out = con.expect("login:", 1800)
                check("second upgrade brings the edition's programs",
                      "wormos-standard" in out and "applied the package change" in out,
                      out[-1500:])
                root_login(con)
            for name, cmd, pattern in AFTER_CHECKS:
                out = con.run(cmd.format(repo=repo), 120)
                check(name, re.search(pattern, out, re.M) is not None, out)

            con.send(f"wh --repo {repo} -y remove btop\n")
            out = con.expect("login:", 600)
            check("program removed", "applied the package change" in out, out[-1500:])
            root_login(con)
            for name, cmd, pattern in DECLINED_CHECKS:
                out = con.run(cmd.format(repo=repo), 120)
                check(name, re.search(pattern, out, re.M) is not None, out)

            con.send("poweroff\n")
            con.expect("Power down", 120)
            proc.wait(timeout=60)
            print("powered off cleanly")
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
