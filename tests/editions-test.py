#!/usr/bin/env python3
"""Boot the ISO of a WormOS edition and check what makes it that edition.

    barebones   the operating system alone: no Tor, no Tor Browser, none of
                the everyday tools; the network works, the basic kit and the
                core repository are there to add them
    amnesia     live only, no installer, the grey look with the panel at the
                bottom, Tor from the start and nothing leaving the machine
                past it (a server and a UDP listener on the host must never
                hear from it), the stricter kernel settings; it starts from
                a USB stick, and pulling the stick out powers it off

The ISO (out/wormos-EDITION.iso by default) boots in QEMU with a GPU, so the
live user's desktop starts; root logs in on the serial console (with
--release, whose root is locked, the live user does). The console output is
written to build/editions.log, the screen to build/editions-EDITION.ppm.

Usage: tests/editions-test.py EDITION [ISO] [--release] [--timeout SECONDS]
"""

import argparse
import functools
import http.server
import importlib.util
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "editions.log")


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tests", file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot_test", "boot-test.py")
desktop_test = load("desktop_test", "desktop-test.py")

COMMON_CHECKS = [
    ("live system from memory", "cat /proc/cmdline; readlink /dev/wormos-root",
     r"wormos\.live(.|\n)*^/dev/loop\d+$"),
    ("system read-only", "touch /usr/x 2>&1", r"Read-only file system"),
    ("desktop started for the live user",
     "for i in $(seq 60); do pidof worm-shell > /dev/null && break; sleep 2; done; "
     "stat -c %U /proc/$(pidof labwc | cut -d' ' -f1)", r"^live$"),
]

BAREBONES_CHECKS = [
    ("edition", "cat /etc/worm/variant", r"^VARIANT_ID=barebones$"),
    ("welcome names it", "welcome | head -3", r"WormOS [\d.]+ Barebones"),
    ("no Tor, no Tor Browser, no extra tools",
     "for f in tor tor-browser htop vim python3 nmap git 7z; do "
     "command -v $f > /dev/null || echo missing; done | grep -c missing", r"^8$"),
    ("the operating system is complete",
     "for f in wh snap worm bh nft unbound curl nano labwc worm-shell wormos-installer; do "
     "command -v $f > /dev/null || echo missing $f; done; echo end", r"^end$"),
    ("network configured", "ip -4 addr show eth0", r"inet 10\.0\.2\.15"),
    ("the basic kit offers the tools", "worm kit show basic",
     r"WormOS packages(.|\n)* coreutils (.|\n)* 7zip (.|\n)*Not installed"),
]

AMNESIA_CHECKS = [
    ("the live user sees Tor's progress",
     "for i in $(seq 30); do su live -c 'worm anon status' | grep -q connected && break; sleep 2; done; "
     "su live -c 'worm anon status'", r"^Anonymous mode: on: all traffic through Tor \(conn"),
    ("edition", "cat /etc/worm/variant", r"^VARIANT_ID=amnesia$"),
    ("no installer", "command -v wormos-installer || echo none", r"^none$"),
    ("stricter kernel", "cat /proc/cmdline; cat /sys/kernel/security/lockdown",
     r"init_on_free=1(.|\n)*\[confidentiality\]"),
    ("anonymous mode on from the start, Tor running", "worm anon status",
     r"^Anonymous mode: on(?!, but)"),
    ("anonymous mode cannot be turned off", "worm anon off 2>&1; worm anon status",
     r"(always sends everything through Tor|needs root)(.|\n)*^Anonymous mode: on"),
    ("Tor Browser is there", "command -v tor-browser", r"^/usr/bin/tor-browser$"),
    ("grey desktop settings",
     "tr '\\0' '\\n' < /proc/$(pidof worm-shell)/environ | "
     "grep -e ^GTK_THEME= -e ^WORM_SHELL_PANEL= -e ^XDG_CONFIG_DIRS= | sort",
     r"^GTK_THEME=WormOS-Amnesia\nWORM_SHELL_PANEL=bottom\nXDG_CONFIG_DIRS=/etc/xdg/amnesia:"),
    ("grey wallpaper", "tr '\\0' ' ' < /proc/$(pidof swaybg)/cmdline",
     r"wormos-amnesia\.png"),
    ("welcome says what it is", "welcome | head -12",
     r"WormOS [\d.]+ Amnesia(.|\n)*Nothing here is kept"),
]

# Checked as root only (with --release the live user logs in).
ROOT_CHECKS = [
    ("firewall active", "worm fw status", r"^Firewall: active"),
]
BAREBONES_ROOT_CHECKS = [
    ("anonymous mode asks for Tor", "worm anon on 2>&1", r"Tor is not installed"),
]
AMNESIA_ROOT_CHECKS = [
    ("Tor runs as its own user",
     "for i in $(seq 30); do pidof tor > /dev/null && break; sleep 2; done; "
     # Tor makes itself non-dumpable, so /proc shows its files as root's:
     # read the IDs it runs with.
     "awk '/^Uid:/ { print $2 }' /proc/$(pidof tor)/status; id -u tor", r"^(\d+)\n\1$"),
    ("firewall sends everything to Tor", "nft list ruleset | grep -c -e redirect -e 'meta skuid'",
     r"^[2-9]"),
]


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("edition", choices=["barebones", "amnesia"])
    parser.add_argument("iso", nargs="?")
    parser.add_argument("--release", action="store_true",
                        help="a release ISO: root is locked, the live user logs in")
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    iso = args.iso or os.path.join(ROOT, "out", f"wormos-{args.edition}.iso")

    # What the host hears from the VM: HTTP requests and UDP datagrams.
    heard = []

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            heard.append(("http", self.path))
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"reached\n")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    http_port = server.server_address[1]
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 0))
    udp_port = udp.getsockname()[1]

    def listen_udp():
        while True:
            try:
                data, _ = udp.recvfrom(512)
                heard.append(("udp", data))
            except OSError:
                return

    threading.Thread(target=listen_udp, daemon=True).start()

    tmp = tempfile.mkdtemp(prefix="wormos-editions-")
    mon_path = os.path.join(tmp, "monitor")
    disk = os.path.join(tmp, "disk.qcow2")
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", disk, "4G"], check=True)
    extra = (f"-vga none -device virtio-gpu-pci -monitor unix:{mon_path},server,nowait "
             f"-vnc unix:{os.path.join(tmp, 'vnc')}")
    env = dict(os.environ, IMAGE=disk, QEMU_MEM="3072", QEMU_EXTRA=extra)
    env["USB_ISO" if args.edition == "amnesia" else "ISO"] = os.path.abspath(iso)
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh")],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    checks = COMMON_CHECKS + (BAREBONES_CHECKS if args.edition == "barebones"
                              else AMNESIA_CHECKS)
    if args.release and args.edition == "amnesia":
        checks = checks + [("root cannot log in",
                            "echo | su -c 'echo got-root' root 2>&1; echo rc=$?",
                            r"\A(?![\s\S]*got-root)[\s\S]*rc=[1-9]")]
    if not args.release:
        checks = checks + ROOT_CHECKS + (BAREBONES_ROOT_CHECKS if args.edition == "barebones"
                                         else AMNESIA_ROOT_CHECKS)
    passed = failures = 0
    total = len(checks) + 3 + (args.edition == "amnesia")

    def check(name, ok, out=""):
        nonlocal passed, failures
        ok = bool(ok)
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    start = time.time()
    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", args.timeout)
            con.send("live\n" if args.release else "root\n")
            con.expect("$ " if args.release else "# ", 60)
            con.send("exec /bin/sh\n")
            print(f"logged in after {time.time() - start:.0f}s")
            mon = desktop_test.Monitor(mon_path)
            if args.edition == "amnesia":
                # The start screen comes first; Tor waits for it.
                out = con.run("for i in $(seq 60); do pidof wormos-greeter > /dev/null && break; "
                              "sleep 2; done; pidof wormos-greeter > /dev/null && echo screen; "
                              "ls /run/worm/anon-hold; pidof tor || echo no-tor", 180)
                # As the live user (--release), /proc hides the greeter.
                check("the start screen comes first, Tor waits for it",
                      re.search(r"^screen\n/run/worm/anon-hold\nno-tor$" if not args.release
                                else r"^/run/worm/anon-hold\nno-tor$", out, re.M), out)
                time.sleep(3)
                mon.key("ret")          # Start Amnesia: Tor directly, no administrator
            for name, cmd, pattern in checks:
                if args.release:
                    cmd = cmd.replace("su live -c 'worm anon status'", "worm anon status")
                out = con.run(cmd, 180)
                check(name, re.search(pattern, out, re.M) is not None, out)

            # Straight to the host: a web server and a UDP listener.
            out = con.run(f"wget -q -T 8 -O- http://10.0.2.2:{http_port}/probe 2>&1; "
                          f"echo probe | nc -u -w 2 10.0.2.2 {udp_port} 2>&1; sleep 3; echo sent",
                          60)
            if args.edition == "amnesia":
                check("nothing reaches the host past Tor", not heard and "reached" not in out,
                      f"{out!r}, heard {heard!r}")
            else:
                check("direct connections work", ("http", "/probe") in heard,
                      f"{out!r}, heard {heard!r}")

            # The screen, once the desktop has settled.
            time.sleep(5)
            shot = os.path.join(ROOT, "build", f"editions-{args.edition}.ppm")
            mon.cmd(f"screendump {shot}")
            time.sleep(1)
            width, height, pixels = desktop_test.read_ppm(shot)
            # Mean colourfulness: max - min channel, over a sample of pixels.
            sample = range(0, width * height, 97)
            chroma = sum(max(pixels[3 * i:3 * i + 3]) - min(pixels[3 * i:3 * i + 3])
                         for i in sample) / len(sample)
            if args.edition == "amnesia":
                check("the screen is grey", chroma < 6, f"mean chroma {chroma:.1f}")
            else:
                check("the screen has the WormOS colours", chroma > 6, f"mean chroma {chroma:.1f}")

            if args.edition == "amnesia":
                # Pull the stick out: the machine must be off within seconds.
                pulled = time.time()
                mon.cmd("device_del stick")
                proc.wait(timeout=30)
                check("pulling the USB stick out powers off at once", True,
                      f"{time.time() - pulled:.0f}s")
            else:
                # The live user powers off as the panel does.
                con.send("/usr/lib/worm/power off\n" if args.release else "poweroff\n")
                con.expect("Power down", 120)
                proc.wait(timeout=60)
                check("powered off cleanly", True)
        except (TimeoutError, RuntimeError, OSError, subprocess.TimeoutExpired) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()
            server.shutdown()
            udp.close()
            subprocess.run(["rm", "-rf", tmp])
    print(f"{passed}/{total} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
