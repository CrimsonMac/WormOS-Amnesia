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
     r"^GTK_THEME=WormOS-Amnesia\nWORM_SHELL_PANEL=bottom\n"
     r"XDG_CONFIG_DIRS=/home/live/\.config/wormos/xdg:/etc/xdg/amnesia:"),
    # labwc runs every autostart it finds; WormOS's steps aside for Amnesia's.
    ("one panel and one wallpaper (only Amnesia's autostart)",
     "pidof worm-shell | wc -w; pidof swaybg | wc -w", r"^1\n1$"),
    ("grey wallpaper", "tr '\\0' ' ' < /proc/$(pidof swaybg)/cmdline",
     r"wormos-amnesia\.png"),
    ("the updater is there, for the user", "ls -l /usr/bin/amnesia-update", r"^-rwsr-xr-x .* root "),
    # Programs from wormholes reach the desktop through the session's bridge.
    ("the wormhole bridge runs in the session", "ls -l /run/user/*/wormhole-bridge",
     r"^p.* live "),
    ("root requests say that root is locked", "su live -c 'sudo id' 2>&1",
     r"no administrator \(root is locked\)"),
    ("no root, but the user installs programs into memory (through Tor)",
     "for i in $(seq 90); do worm anon status | grep -q connected && break; sleep 2; done; "
     "su live -c 'wh install tree' 2>&1 | tail -1; su live -c 'tree -d /etc/xdg | tail -1'",
     r"Done: changed in the running system(.|\n)*directories"),
    ("welcome says what it is", "welcome | head -12",
     r"WormOS [\d.]+ Amnesia(.|\n)*Nothing here is kept"),
    ("the keyboard layout from the start screen",
     "cat /home/live/.config/labwc/environment; "
     "tr '\\0' '\\n' < /proc/$(pidof worm-shell)/environ | grep ^XKB_DEFAULT_LAYOUT=",
     r"^XKB_DEFAULT_LAYOUT=de\nXKB_DEFAULT_LAYOUT=de$"),
    ("no microphone: sound capture is root's alone, playback works",
     "stat -c '%a %U' /dev/snd/pcmC0D0c /dev/snd/pcmC0D0p", r"^600 root\n660 root$"),
    ("each destination gets its own Tor circuit", "grep ^TransPort /etc/tor/torrc",
     r"IsolateDestAddr"),
    # The VM offers a shared folder and a guest agent channel: both stay off.
    ("the host's shared folder is seen but not mounted",
     "grep -e ^share-tags= -e ^shares= /run/worm/vm; grep -c ' /media/' /proc/mounts",
     r"^share-tags=hostshare\nshares=off\n0$"),
    ("no guest agent", "grep ^agent= /run/worm/vm; su live -c 'worm vm' | grep 'Guest agent'",
     r"^agent=off\nGuest agent: +off$"),
    ("Settings is there, the system stays locked",
     "command -v wormos-settings; echo x | su live -c '/usr/lib/worm/settings hostname other' "
     "> /dev/null 2>&1; echo rc=$?; hostname",
     r"^/usr/bin/wormos-settings\nrc=[13]\n(?!other)"),
    # Root is locked: WormOS's own safe actions run for the live user.
    ("the live user sees the protections", "su live -c 'worm status' 2>&1",
     r"(?i)firewall(?![\s\S]*needs root)"),
    ("the live user sees the firewall", "su live -c 'worm fw status' 2>&1", r"^Firewall: active"),
    ("the live user checks the clock", "su live -c 'worm time sync' 2>&1; echo rc=$?",
     r"\A(?![\s\S]*needs root)[\s\S]*rc=\d+"),
    ("the live user sees the bridges", "su live -c 'worm anon bridges status' 2>&1",
     r"\A(?![\s\S]*needs root)[\s\S]*\S"),
    ("no root terminal in the launcher or the menu",
     "grep -c '^X-WormOS-Needs-Root=true' /usr/share/applications/org.wormos.RootTerminal.desktop; "
     "grep -c root-terminal /etc/xdg/amnesia/labwc/menu.xml; "
     "grep -c 'menu=\"amnesia-menu\"' /etc/xdg/amnesia/labwc/rc.xml",
     r"^1\n0\n1$"),
    # Tor's controller: only root reaches it; the live user gets new circuits
    # through the settings helper.
    ("new Tor circuits for the live user", "su live -c 'worm anon new-circuits' 2>&1",
     r"^New connections now use new Tor circuits\.$"),
    ("Tor's controller is out of the user's reach",
     "stat -c '%a %U' /run/tor-control; su live -c 'cat /run/tor-control/cookie 2>/dev/null' | wc -c; "
     "su live -c 'ls /run/tor-control' 2>&1",
     r"^700 tor\n0\n.*(Permission denied|can't open)"),
    ("no TCP timestamps, no core dumps", "sysctl -n net.ipv4.tcp_timestamps kernel.core_pattern",
     r"^0\n\|/bin/false$"),
    # The same for everyone: what the local network and programs can see
    # does not tell one Amnesia computer from another.
    ("looks the same on every computer",
     "hostname; date +%Z; cat /sys/class/net/eth0/address; "
     # (the live user of a release cannot see root's processes)
     "p=$(pidof udhcpc); if [ -n \"$p\" ]; then tr '\\0' ' ' < /proc/$p/cmdline; else printf hidden; fi; "
     "echo; grep -c SendHostname=true /etc/iwd/main.conf",
     r"^wormos\nUTC\n(?!52:54:00:12:34:56)[0-9a-f:]{17}\n(udhcpc -f -S -i eth0 -V  -s \S+ |hidden)\n0$"),
    ("Super+Shift+Escape shuts down at once",
     "grep -c 'key=\"W-S-Escape\".*/usr/lib/worm/power off' /etc/xdg/amnesia/labwc/rc.xml; "
     "grep -c 'quick-action new-circuits' /etc/xdg/amnesia/labwc/menu.xml", r"^1\n1$"),
    # Everyday programs: the Welcome window at every start, Software
    # without a password, Files.
    ("the Welcome window opens at every start", "pidof wormos-welcome", r"^\d+$"),
    ("Software searches for the live user", "su live -c '/usr/lib/worm/software search tree' 2>&1 | head -3",
     r"^tree "),
    ("Files, Software and Welcome in the menu",
     "grep -c -e wormos-files -e wormos-software -e wormos-welcome /etc/xdg/amnesia/labwc/menu.xml; "
     "grep -c 'key=\"W-e\".*wormos-files' /etc/xdg/amnesia/labwc/rc.xml", r"^3\n1$"),
    ("updates are looked for once Tor is connected",
     "ls /usr/lib/worm/amnesia-update-notice; grep -c amnesia-update-notice "
     "/etc/xdg/amnesia/labwc/autostart", r"amnesia-update-notice\n1$"),
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
    parser.add_argument("--skew", type=int, metavar="HOURS",
                        help="start with the clock this many hours behind (negative: ahead), "
                             "far enough that Tor cannot connect (amnesia, e.g. -48): "
                             "it must be set from Tor's reports")
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
    if args.edition == "amnesia":
        # A sound card with a microphone, which Amnesia must not let anyone use.
        extra += " -audiodev none,id=snd0 -device ich9-intel-hda -device hda-duplex,audiodev=snd0"
        # A shared folder and a guest agent channel, which Amnesia leaves alone.
        share = os.path.join(tmp, "share")
        os.mkdir(share)
        extra += (f" -virtfs local,path={share},mount_tag=hostshare,security_model=none,id=fs0"
                  f" -chardev socket,path={os.path.join(tmp, 'qga')},server=on,wait=off,id=qga0"
                  " -device virtserialport,bus=vserial0.0,chardev=qga0,name=org.qemu.guest_agent.0")
    if args.skew:
        behind = time.gmtime(time.time() - args.skew * 3600)
        # Without kvmclock the guest takes its time from the (skewed) RTC.
        extra += " -rtc base=" + time.strftime("%Y-%m-%dT%H:%M:%S", behind)
        if os.access("/dev/kvm", os.W_OK):
            extra += " -cpu host,kvmclock=off"
    env = dict(os.environ, IMAGE=disk, QEMU_MEM="3072", QEMU_EXTRA=extra)
    env["USB_ISO" if args.edition == "amnesia" else "ISO"] = os.path.abspath(iso)
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh")],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    checks = COMMON_CHECKS + (BAREBONES_CHECKS if args.edition == "barebones"
                              else AMNESIA_CHECKS)
    if args.skew:
        checks = checks + [("the clock was set from Tor's reports",
                            "for i in $(seq 60); do logread | grep -q 'reported by Tor' && break; "
                            "sleep 2; done; logread | grep -o 'clock corrected.*'; date -u +%s",
                            r"clock corrected by [-+]\d+ s \(reported by Tor's relays\)")]
    if args.release and args.edition == "amnesia":
        checks = checks + [("root cannot log in",
                            "echo | su -c 'echo got-root' root 2>&1; echo rc=$?",
                            r"\A(?![\s\S]*got-root)[\s\S]*rc=[1-9]")]
    if not args.release:
        checks = checks + ROOT_CHECKS + (BAREBONES_ROOT_CHECKS if args.edition == "barebones"
                                         else AMNESIA_ROOT_CHECKS)
    passed = failures = 0
    total = len(checks) + 3 + 2 * (args.edition == "amnesia")

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
                # The start screen is grey too: no strongly coloured pixels.
                shot = os.path.join(ROOT, "build", "editions-start.ppm")
                mon.cmd(f"screendump {shot}")
                time.sleep(1)
                width, height, pixels = desktop_test.read_ppm(shot)
                coloured = sum(1 for i in range(0, width * height, 7)
                               if max(pixels[3 * i:3 * i + 3]) - min(pixels[3 * i:3 * i + 3]) > 40)
                check("the start screen is grey", coloured < 20, f"{coloured} coloured pixels")
                # The keyboard layout (two fields back from the start button;
                # forwards are the restart and shut-down buttons), then Tor
                # directly, no administrator.
                mon.key("shift-tab")
                mon.key("shift-tab")
                mon.key("spc")
                time.sleep(1)
                mon.type("German")
                mon.key("ret")
                time.sleep(1)
                mon.key("tab")
                mon.key("tab")
                mon.key("ret")
            for name, cmd, pattern in checks:
                if args.release:
                    cmd = re.sub(r"su live -c '([^']*)'", r"\1", cmd)
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
