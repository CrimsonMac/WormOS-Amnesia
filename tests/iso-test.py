#!/usr/bin/env python3
"""Boot the WormOS ISO, check the live system, install onto a disk, boot it.

The ISO (out/wormos.iso by default) boots in QEMU with UEFI firmware and an
empty 12 GiB disk. Root logs in on the serial console of the live system,
which must run from memory with everything working, and installs WormOS
with the installer's backend (/usr/lib/worm/install-system). The disk then
boots without the ISO, with UEFI and with BIOS firmware, and the ISO also
boots with BIOS firmware. The console output is written to build/iso.log.

With --live-only it checks the live system and the installation, but not
the installed disk: for a release ISO, whose installed system starts with
the first-boot setup and has no root login before it.

Usage: tests/iso-test.py [ISO] [--live-only] [--timeout SECONDS]
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
LOG = os.path.join(ROOT, "build", "iso.log")


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tests", file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot_test", "boot-test.py")

LIVE_CHECKS = [
    ("live system", "cat /proc/cmdline", r"wormos\.live"),
    # Daemons' messages on the console can land on the same line.
    ("runs from memory", "readlink /dev/wormos-root", r"(^|\s)/dev/loop\d+$"),
    ("every subvolume from memory",
     "grep -E ' /(var|home|var/lib/wormholes) ' /proc/mounts | grep -vc -e loop -e wormos-root",
     r"^0$"),
    ("filesystem grown into memory", "df -m /var | awk 'NR == 2 { print $2 }'",
     r"^(1[5-9]\d\d|2\d\d\d)$"),
    ("ISO let go of", "grep -c iso9660 /proc/mounts", r"^0$"),
    ("system read-only", "touch /usr/x 2>&1", r"Read-only file system"),
    ("live user", "id live", r"\(wheel\)"),
    ("installer in the live user's launcher",
     "grep -c NoDisplay /home/live/.local/share/applications/org.wormos.Installer.desktop",
     r"^0$"),
    ("firewall active", "worm fw status", r"^Firewall: active"),
    ("snapshots work", "snap now -c 'live check' && snap list", r"live check"),
    ("root without a password", "grep -c '^root::' /etc/shadow", r"^1$"),
    ("desktop started for the live user",
     "for i in $(seq 30); do pidof labwc > /dev/null && break; sleep 2; done; "
     "stat -c %U /proc/$(pidof labwc | cut -d' ' -f1)", r"^live$"),
    ("installer opened", "for i in $(seq 30); do pidof wormos-installer && break; sleep 2; done",
     r"^\d+"),
    ("the disk is offered", "/usr/lib/worm/install-system list", r"^vda 12884901888 0 "),
    ("refuses a missing disk", "/usr/lib/worm/install-system install vdz 2>&1", r"not a disk"),
]

INSTALL_CHECKS = [
    ("partitioned", "grep -c 'vda[123]$' /proc/partitions", r"^3$"),
    ("disk not mounted after", "grep -c '^/dev/vda' /proc/mounts", r"^0$"),
]

INSTALLED_CHECKS = [
    ("started from the disk", "cat /proc/cmdline", r"root=UUID=(?![\s\S]*wormos\.live)"),
    ("root from the disk", "readlink /dev/wormos-root", r"^/dev/vda2$"),
    ("filesystem fills the disk", "df -m /var | awk 'NR == 2 { print $2 }'",
     r"^1[0-2]\d\d\d$"),
    ("no live user", "grep -c '^live:' /etc/passwd", r"^0$"),
    ("nothing from the live session", "snap list", r"No snapshots|^\s*ID"),
    ("firewall active", "worm fw status", r"^Firewall: active"),
    ("snapshots work", "snap now -c installed && snap list", r"installed"),
    ("installer refuses outside the live system",
     "/usr/lib/worm/install-system list 2>&1", r"only be installed from the live system"),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("iso", nargs="?", default=os.path.join(ROOT, "out", "wormos.iso"))
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--live-only", action="store_true")
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="wormos-iso-")
    disk = os.path.join(tmp, "disk.qcow2")
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", disk, "12G"], check=True)
    passed = failures = 0
    total = len(LIVE_CHECKS) + 1 + len(INSTALL_CHECKS) + 1
    if not args.live_only:
        total += 2 * len(INSTALLED_CHECKS) + 1

    def check(name, ok, out=""):
        nonlocal passed, failures
        ok = bool(ok)
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    def boot(log, iso, bios=False, label=""):
        env = dict(os.environ, IMAGE=disk, QEMU_MEM="3072")
        if iso:
            env["ISO"] = os.path.abspath(iso)
            # A GPU, so the live user's desktop starts (with no display).
            env["QEMU_EXTRA"] = "-vga none -device virtio-gpu-pci"

        cmd = [os.path.join(ROOT, "scripts", "run-qemu.sh")] + (["--bios"] if bios else [])
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=env)
        con = boot_test.Console(proc, log)
        start = time.time()
        con.expect("login:", args.timeout)
        con.send("root\n")
        con.expect("# ", 60)
        con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
        print(f"{label}: logged in after {time.time() - start:.0f}s")
        return proc, con

    def run(con, checks, timeout=120):
        for name, cmd, pattern in checks:
            out = con.run(cmd, timeout)
            check(name, re.search(pattern, out, re.M) is not None, out)

    def finish():
        print(f"{passed}/{total} checks passed")
        return 1 if failures else 0

    def power_off(proc, con):
        con.send("poweroff\n")
        con.expect("Power down", 120)
        proc.wait(timeout=60)

    with open(LOG, "wb") as log:
        proc = None
        try:
            proc, con = boot(log, args.iso, label="ISO, UEFI")
            run(con, LIVE_CHECKS)
            out = con.run("/usr/lib/worm/install-system install vda 2>&1 | tail -n 3", 900)
            check("installed onto the disk", re.search(r"^done$", out, re.M), out)
            run(con, INSTALL_CHECKS)
            power_off(proc, con)
            if args.live_only:
                check("powered off cleanly", True)
                return finish()

            proc, con = boot(log, None, label="installed disk, UEFI")
            run(con, INSTALLED_CHECKS)
            power_off(proc, con)

            proc, con = boot(log, None, bios=True, label="installed disk, BIOS")
            run(con, INSTALLED_CHECKS)
            power_off(proc, con)

            proc, con = boot(log, args.iso, bios=True, label="ISO, BIOS")
            out = con.run("cat /proc/cmdline; readlink /dev/wormos-root")
            check("ISO boots with BIOS firmware",
                  "wormos.live" in out and "/dev/loop" in out, out)
            power_off(proc, con)
            check("powered off cleanly", True)
            return finish()
        except (TimeoutError, RuntimeError, OSError, subprocess.TimeoutExpired) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc and proc.poll() is None:
                proc.kill()
            subprocess.run(["rm", "-rf", tmp])
    return finish()


if __name__ == "__main__":
    sys.exit(main())
