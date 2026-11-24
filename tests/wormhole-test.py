#!/usr/bin/env python3
"""Check wormholes: software from Debian and Kali in containers.

Boots a throwaway copy of the image (like boot-test.py) and needs internet
access: it sets up the Debian and Kali wormholes from their real archives,
installs a package from each, runs the exported commands as root and as a
normal user, checks what the containers can and cannot touch, and removes
the packages again. Takes several minutes. Console output goes to
build/wormhole.log.

With --dns ADDR the VM resolves names through ADDR instead of the DNS
relay of QEMU, which forwards to the first nameserver of this machine.

Usage: tests/wormhole-test.py [--timeout SECONDS] [--dns ADDR]
"""

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "wormhole.log")

spec = importlib.util.spec_from_file_location(
    "boot_test", os.path.join(ROOT, "tests", "boot-test.py"))
boot_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_test)

CHECKS = [
    # The first answer needs a TLS handshake and DNSSEC priming: slow under TCG.
    ("encrypted DNS resolves",
     "i=0; until nslookup deb.debian.org >/dev/null 2>&1 || [ $i -ge 30 ]; do sleep 2; i=$((i+1)); done; "
     "nslookup deb.debian.org 2>&1 | tail -3", r"Address:?\s+[0-9a-f.:]+"),
    # Before any wormhole: search reads the package list (root fetches it).
    # zathura: a Debian program WormOS does not ship, with a desktop entry
    # and a scalable icon (btop, used before, is a core package since 1.8).
    ("search without a wormhole", "wh search zathura 2>&1 | grep -A1 '^zathura '",
     r"^zathura \[debian\]\n    .*document viewer"),
    ("debian install", "wh -y install --from debian zathura 2>&1 | tail -3",
     r"zathura -> /usr/bin/zathura \(debian\)"),
    ("debian wormhole ready", "cat /var/lib/wormholes/debian/etc/os-release",
     r"Debian GNU/Linux 13"),
    ("exported command", "head -2 /usr/local/bin/zathura", r"Exported by wh from the debian wormhole"),
    ("runs as root", "zathura --version 2>&1 | head -1", r"^zathura 0\."),
    ("listed", "wh list | grep zathura", r"^zathura\s+\S+\s+\[debian\]$"),
    ("found by search", "wh search zathura", r"^zathura .*\[debian\]$"),
    # Without --from: the existing Debian wormhole has it, so Kali is not
    # set up (apt-cache's long answer must not count as a failure).
    ("source found in an existing wormhole", "wh -y install figlet 2>&1 | tail -1; wh list",
     r"^figlet\s+\S+\s+\[debian\]$"),
    ("no second wormhole for it", "ls /var/lib/wormholes | grep -c kali", r"^0$"),
    # Java's setup links its man page into man1, which a trimmed wormhole
    # used to lack (openjdk, and Ghidra with it, failed to configure).
    ("java installs", "wh -y install --from debian openjdk-21-jre-headless 2>&1 | tail -2; "
     "whc debian java -version 2>&1 | head -1", r'version "21'),
    ("a broken wormhole repairs itself",
     "rmdir /var/lib/wormholes/debian/usr/share/man/man1 2>/dev/null; "
     "wh -y install --from debian sl >/dev/null 2>&1; "
     "test -d /var/lib/wormholes/debian/usr/share/man/man1 && echo repaired", r"^repaired$"),
    ("desktop entry exported", "grep -E '^(Exec|X-WormOS-Wormhole)=' "
     "/usr/local/share/applications/org.pwmt.zathura.desktop",
     r"^Exec=zathura(.|\n)*^X-WormOS-Wormhole=true$"),
    ("icons exported", "ls /usr/local/share/icons/hicolor/scalable/apps/",
     r"^org\.pwmt\.zathura\.svg$"),
    ("no setuid escape", "mount | grep ' /var/lib/wormholes '", r"nosuid,nodev"),
    ("add a user", "worm unlock >/dev/null && adduser -D tester && worm lock >/dev/null && echo ok",
     r"^ok$"),
    ("runs as a user", "su tester -c 'zathura --version' 2>&1 | head -1", r"^zathura 0\."),
    ("user keeps their identity", "su tester -c 'whc debian id -un'", r"^tester$"),
    ("user cannot change the container",
     "su tester -c 'whc debian touch /usr/x' 2>&1", r"(Permission denied|Read-only)"),
    ("user sees their home", "su tester -c 'cd && touch here && whc debian ls ~/here'",
     r"/home/tester/here$"),
    ("host network shared", "whc debian cat /etc/resolv.conf | grep -c nameserver", r"^[1-9]"),
    # Wayland programs from wormholes appear on the desktop: here a
    # headless compositor, run by the user, and Debian's event viewer.
    ("wayland program from debian", "wh -y install --from debian wev 2>&1 | tail -3",
     r"wev -> /usr/bin/wev \(debian\)"),
    ("system fonts shared", "su tester -c 'whc debian ls /usr/local/share/fonts/wormos'",
     r"\bhack\b"),
    ("wayland program shows a window",
     "/usr/lib/worm/runtime-dirs; su tester -c 'export XDG_RUNTIME_DIR=/run/user/1000 "
     "WLR_BACKENDS=headless WLR_RENDERER=pixman; labwc > /tmp/labwc.log 2>&1 & "
     # script gives wev a terminal, so its output is not lost in a buffer.
     "sleep 5; WAYLAND_DISPLAY=wayland-0 timeout 10 whc debian script -qfc wev /tmp/wev.log "
     "> /dev/null 2>&1; "
     "grep -c xdg_toplevel /tmp/wev.log'; kill $(pidof labwc)", r"^[1-9]"),
    # X11 programs from a wormhole open on the desktop through Xwayland,
    # which the compositor starts when the first one connects.
    ("x11 programs from debian", "wh -y install --from debian x11-apps x11-utils 2>&1 | tail -1; "
     "ls -1 /usr/local/bin/xeyes /usr/local/bin/xwininfo", r"^/usr/local/bin/xwininfo$"),
    ("x11 program shows a window",
     "su tester -c 'export XDG_RUNTIME_DIR=/run/user/1000 "
     "WLR_BACKENDS=headless WLR_RENDERER=pixman; labwc > /tmp/labwc-x.log 2>&1 & "
     "sleep 5; d=:$(ls /tmp/.X11-unix | sed -n \"s/^X//p\" | head -1); "
     "DISPLAY=$d xeyes > /tmp/xeyes.log 2>&1 & sleep 6; "
     "DISPLAY=$d xwininfo -root -tree | grep -c -i xeyes; kill %2'; kill $(pidof labwc)",
     r"^[1-9]"),
    ("core command not shadowed", "ls /usr/local/bin/ls 2>&1", r"No such file"),
    ("debian remove", "wh -y remove zathura && ls /usr/local/bin/zathura 2>&1",
     r"No such file"),
    ("desktop entry and icons removed",
     "ls /usr/local/share/applications/org.pwmt.zathura.desktop "
     "/usr/local/share/icons/hicolor/scalable/apps/org.pwmt.zathura.svg 2>&1 | grep -c 'No such file'",
     r"^2$"),
    # Blackholes with a wormhole's system, and Windows programs under Wine.
    ("blackhole with debian's system", "bh open --from debian -- cat /etc/debian_version",
     r"^1\d\.\d+"),
    ("blackhole keeps its own /lib64 for debian's loader",
     "bh open --from debian -- readlink /lib64", r"^usr/lib64$"),
    ("64-bit wine installed", "wh -y install wine wine64 2>&1 | tail -1; "
     "ls /var/lib/wormholes/debian/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/cmd.exe", r"cmd\.exe$"),
    ("64-bit windows program in a blackhole",
     "bh open --from debian -- sh -c 'WINEDEBUG=-all wine cmd /c echo win64 ok' 2>/dev/null",
     r"^win64 ok\s*$"),
    ("32-bit wine installed", "wh -y install wine32:i386 2>&1 | tail -1; "
     "ls /var/lib/wormholes/debian/usr/lib/i386-linux-gnu/wine/i386-windows/cmd.exe", r"cmd\.exe$"),
    ("i386 added once", "whc debian dpkg --print-foreign-architectures", r"^i386$"),
    ("32-bit system calls refused in a namespace blackhole",
     "bh open --from debian -- sh -c 'WINEDEBUG=-all wine C:\\\\windows\\\\syswow64\\\\cmd.exe "
     "/c echo x' 2>&1 | tail -1", r"Bad system call"),
    ("32-bit windows program in a virtual machine",
     "bh open --vm --from debian --memory 1G -- sh -c 'WINEDEBUG=-all wine "
     "C:\\\\windows\\\\syswow64\\\\cmd.exe /c echo win32 ok' 2>/dev/null",
     r"^win32 ok\s*$"),
    ("64-bit windows program in a virtual machine",
     "bh open --vm --from debian --memory 1G -- sh -c 'WINEDEBUG=-all wine cmd /c echo win64 vm' "
     "2>/dev/null", r"^win64 vm\s*$"),
    ("kali fallback", "wh -y install --from kali hashid 2>&1 | tail -3",
     r"hashid -> .* \(kali\)"),
    ("kali tool runs", "hashid --version 2>&1", r"^(?i:hashid) v\d"),
    ("kali wormhole", "cat /var/lib/wormholes/kali/etc/os-release", r"Kali GNU/Linux"),
    # A tool kit: a small one made for the test, with a tool no source has.
    ("test kit", "worm unlock > /dev/null && printf 'title = Test\\ndescription = A test kit\\n"
     "tool = toilet\\ntool = no-such-tool-wormos\\n' > /usr/share/worm/kits/test && "
     "worm lock > /dev/null && worm kit show test", r"toilet no-such-tool-wormos"),
    ("kit installed, missing tool skipped", "worm kit install test 2>&1 | tail -2",
     r"1 of 2 tools installed(.|\n)*no-such-tool-wormos"),
    ("kit tool runs", "ls /usr/local/bin/toilet && worm kit list | grep '^test'", r"installed"),
    ("kit removed", "worm kit remove test && ls /usr/local/bin/toilet 2>&1", r"No such file"),
    ("system still read-only", "touch /usr/x 2>&1", r"Read-only file system"),
    ("clock checked over HTTPS", "worm time sync", r"(right|corrected)"),
    ("anonymous mode on", "worm anon on", r"^Anonymous mode is on"),
    ("tor connects",
     "i=0; until worm anon status | grep -q connected || [ $i -ge 300 ]; do sleep 2; i=$((i+1)); done; "
     "worm anon status", r"connected"),
    ("traffic goes through Tor",
     "curl -s -m 60 https://check.torproject.org/api/ip", r'"IsTor":\s*true'),
    ("names resolve through Tor",
     "for i in 1 2 3; do nslookup -type=a deb.debian.org 2>&1 | grep -A1 '^Name:' && break; "
     "sleep 5; done", r"Address"),
    ("clock checked through Tor", "worm time sync", r"(right|corrected)"),
    ("anonymous mode off", "worm anon off", r"^Anonymous mode is off"),
    # The resolver reconnects first, which can take a while.
    ("direct again", "for i in 1 2 3; do curl -s -m 60 https://check.torproject.org/api/ip "
     "&& break; done", r'"IsTor":\s*false'),
]


# --apps: real programs across the toolkits people install, each started
# as a user with X11 (GTK and Qt told to use it too, which they otherwise
# would not, so that xwininfo sees every window). (package, source,
# command, window name pattern, seconds to wait for the window)
APPS = [
    ("mousepad", "debian", "mousepad", "Mousepad", 60),
    ("qbittorrent", "debian", "qbittorrent", "qBittorrent", 90),
    ("idle", "debian", "idle", "IDLE|Python", 60),
    ("ghidra", "kali", "ghidra", "Ghidra", 240),
    ("burpsuite", "kali", "burpsuite", "Burp", 240),
]

APP_SESSION = (
    "worm unlock >/dev/null; id tester >/dev/null 2>&1 || adduser -D tester; "
    "worm lock >/dev/null; /usr/lib/worm/runtime-dirs; "
    "su tester -c 'export XDG_RUNTIME_DIR=/run/user/1000 WLR_BACKENDS=headless "
    "WLR_RENDERER=pixman; labwc > /tmp/labwc-apps.log 2>&1 & "
    # A session bus, as on the desktop: GTK programs stall without one.
    "dbus-daemon --session --address=unix:path=/run/user/1000/bus --fork "
    ">/dev/null'; sleep 5; "
    "ls /tmp/.X11-unix")


def app_check(con, name, source, cmd, pattern, wait):
    """Install a program and wait for its window; returns (ok, output)."""
    out = con.run(f"wh -y install --from {source} {name} 2>&1 | tail -3", 3600)
    if "wh:" in out and "->" not in out and "already" not in out:
        return False, out
    start = (f"su tester -c 'cd; export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:0 "
             f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus "
             f"GDK_BACKEND=x11 QT_QPA_PLATFORM=xcb; "
             f"unset WAYLAND_DISPLAY; {cmd} > /tmp/app-{name}.log 2>&1 &'")
    con.run(start)
    look = (f"for i in $(seq {wait // 3}); do su tester -c 'DISPLAY=:0 xwininfo -root -tree' "
            f"2>/dev/null | grep -E -c '{pattern}' && break; sleep 3; done")
    found = con.run(look, wait + 60)
    con.run(f"pkill -u tester -f '{cmd}' 2>/dev/null; sleep 2; "
            f"pkill -9 -u tester java 2>/dev/null; true")
    ok = re.search(r"^[1-9]", found, re.M) is not None
    return ok, found + con.run(f"tail -5 /tmp/app-{name}.log")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=3600,
                        help="seconds to wait for each step")
    parser.add_argument("--apps", action="store_true",
                        help="also install and start real programs (Ghidra, Burp Suite, GTK, "
                             "Qt and Tk programs; several GB of downloads)")
    parser.add_argument("--dns", metavar="ADDR",
                        help="name server for the VM, instead of QEMU's relay")
    args = parser.parse_args()

    env = dict(os.environ, QEMU_MEM=os.environ.get("QEMU_MEM", "2048"))
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    passed = failures = 0
    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", 600)
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            con.run("dmesg -n 1")
            if args.dns:
                # After DHCP has written its own (the lease outlasts the test).
                con.run("while [ ! -s /run/resolv.conf ]; do sleep 1; done; "
                        f"echo 'nameserver {args.dns}' > /run/resolv.conf")
            for name, cmd, pattern in CHECKS:
                start = time.time()
                out = con.run(cmd, args.timeout)
                ok = re.search(pattern, out, re.MULTILINE) is not None
                passed += ok
                failures += not ok
                print(f"{'PASS' if ok else 'FAIL'}  {name} ({time.time() - start:.0f}s)")
                if not ok:
                    print(f"      output: {out[-400:]!r}")
            if args.apps:
                con.run(APP_SESSION, 120)
                for app in APPS:
                    begin = time.time()
                    ok, out = app_check(con, *app)
                    passed += ok
                    failures += not ok
                    print(f"{'PASS' if ok else 'FAIL'}  {app[0]} opens a window "
                          f"({time.time() - begin:.0f}s)")
                    if not ok:
                        print(f"      output: {out[-600:]!r}")
                # Menu entries come from tool-menus (Kali's menu): Ghidra's
                # starts it, a command line tool's opens a terminal.
                con.run("wh -y install crunch > /tmp/crunch.log 2>&1", 600)
                out = con.run("cat /usr/local/share/applications/kali-ghidra.desktop "
                              "/usr/local/share/applications/kali-crunch.desktop; "
                              "ls /usr/share/icons/hicolor/*/apps/kali-ghidra.*")
                ok = bool(re.search(r"^Exec=ghidra$", out, re.M) and "kali-ghidra." in out and
                          re.search(r'^Exec=/usr/lib/worm/exec-in-shell "crunch"$', out, re.M) and
                          re.search(r"^Terminal=true$", out, re.M) and
                          re.search(r"^X-WormOS-Section=Credential Access$", out, re.M))
                passed += ok
                failures += not ok
                print(f"{'PASS' if ok else 'FAIL'}  menu entries for Ghidra and crunch, with sections")
                if not ok:
                    print(f"      output: {out[-900:]!r}")
            con.send("poweroff\n")
            con.expect("Power down", 120)
        except (TimeoutError, RuntimeError) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()
    print(f"{passed}/{len(CHECKS) + (len(APPS) + 1 if args.apps else 0)} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
