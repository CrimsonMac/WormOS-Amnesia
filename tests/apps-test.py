#!/usr/bin/env python3
"""Check that programs from the tool kits, the core repository and the
Debian and Kali wormholes open and run on the desktop.

Boots a throwaway copy of the image with a virtual screen, goes through
the graphical first-boot setup (the desktop then runs as the new user,
with the session's real environment), installs the tool kits and a set of
graphical programs, then:

  - starts every menu entry the way the launcher does (gio launch, from
    the session's environment) and waits for its window: programs with a
    window open one, command line tools open a terminal;
  - runs the command of every terminal entry, and every exported command
    with --help, as the user, and looks for the signs of a broken program
    (a missing library, module, script or toolkit plugin);
  - checks the runtime pieces that make this work for every program:
    the companions of the runtime rules, the stand-ins (sudo, xdg-open,
    x-terminal-emulator) reaching the session's bridge, generated menu
    entries and Kali's helper scripts.

Needs internet access and a lot of downloads (the pentest kit alone is
over a gigabyte); takes an hour or more. Console output goes to
build/apps.log, a report of every program to build/apps-report.txt.

Usage: tests/apps-test.py [--kits all|NAME,...] [--no-gui-apps] [--image IMAGE]
"""

import argparse
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "apps.log")
REPORT = os.path.join(ROOT, "build", "apps-report.txt")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tests", path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot_test", "boot-test.py")
desktop = load("desktop_test", "desktop-test.py")

USER, USER_PASSWORD, ROOT_PASSWORD = desktop.USER, desktop.USER_PASSWORD, desktop.ROOT_PASSWORD
GCC = desktop.GCC
SYSROOT = os.path.join(ROOT, "build", "sysroot")

KITS = ["blueteam", "forensics", "network", "passwords", "reverse", "web", "windows", "pentest"]

# Graphical programs people install, across toolkits: GTK 3 and 4, Qt 5
# and 6, Java, Electron-like browsers, plain X11. (package, source)
GUI_APPS = [
    ("wireshark", "debian"), ("mousepad", "debian"), ("gnome-calculator", "debian"),
    ("keepassxc", "debian"), ("vlc", "debian"), ("gimp", "debian"),
    ("firefox-esr", "debian"), ("chromium", "debian"), ("filezilla", "debian"),
    ("thunar", "debian"), ("audacity", "debian"), ("qbittorrent", "debian"),
    ("okular", "debian"), ("sqlitebrowser", "debian"), ("meld", "debian"),
    ("xterm", "debian"), ("gparted", "debian"),
    ("ghidra", "kali"), ("burpsuite", "kali"), ("zaproxy", "kali"), ("rizin-cutter", "kali"),
]

# Signs of a program that cannot run at all (not of one that only
# dislikes --help as an argument).
BROKEN = re.compile(
    r"error while loading shared libraries|ModuleNotFoundError|ImportError|No module named|"
    r"can't open file|Could not find the Qt platform plugin|no Qt platform plugin|"
    r"Exec format error|^whc: |(?<!man): not found$|command not found|cannot execute", re.M)

# Broken in the distribution itself, not on WormOS: Kali's wrapper runs a
# script its package no longer ships.
UPSTREAM = {"responder-DHCP_Auto"}

# Seconds to wait for a window: Java programs start slowly.
SLOW = re.compile(r"ghidra|burp|zap|armitage|maltego", re.I)

# Entries that do something other than open a window of their own.
NO_WINDOW = re.compile(r"-settings\.desktop$|bulk-rename|okularApplication_")

# The session's environment for commands run as the user (from worm-shell,
# the launcher's process).
IN_SESSION = r"""#!/bin/sh
pid=$(pidof worm-shell | cut -d' ' -f1)
tr '\0' '\n' < /proc/$pid/environ | grep -v '^_=' |
  sed "s/'/'\\\\''/g; s/^\([A-Za-z_][A-Za-z0-9_]*\)=\(.*\)/export \1='\2'/" > /tmp/session.env
chmod 644 /tmp/session.env
exec su tester -s /bin/sh -c ". /tmp/session.env; cd; $*"
"""

# Starts each entry given and prints what came of it: the windows that
# appeared and the end of its output.
PROBE = r"""#!/bin/sh
# probe WAIT ENTRY...: launch each entry as the launcher does.
wait=$1; shift
for f; do
  /tmp/in-session /tmp/toplevels | sort > /tmp/probe-before
  ps -o pid,user | awk '$2 == "tester" {print $1}' | sort > /tmp/probe-procs
  log=/tmp/probe-$(basename "$f").log
  /tmp/in-session "gio launch $f > $log 2>&1 &"
  i=0; new=
  while [ $i -lt "$wait" ] && [ -z "$new" ]; do
    sleep 2; i=$((i + 2))
    /tmp/in-session /tmp/toplevels | sort > /tmp/probe-after
    new=$(comm -13 /tmp/probe-before /tmp/probe-after)
  done
  echo "## $(basename "$f")"
  [ -n "$new" ] && echo "$new" | sed 's/^/   window: /'
  echo "   log: $(tail -c 400 "$log" | tr '\n' ' ')"
  # Close what it started.
  ps -o pid,user | awk '$2 == "tester" {print $1}' | sort > /tmp/probe-now
  for p in $(comm -13 /tmp/probe-procs /tmp/probe-now); do kill -9 "$p" 2>/dev/null; done
  sleep 1
done
"""

# Runs a command line as the user for at most SECONDS, then stops it and
# everything it started (interactive tools such as recon-ng never exit on
# their own), and prints its output and status.
RUN = r"""#!/bin/sh
# run SECONDS COMMAND
limit=$1; shift
ps -o pid,user | awk '$2 == "tester" {print $1}' | sort > /tmp/run-procs
rm -f /tmp/run.out /tmp/run.status
su tester -s /bin/sh -c ". /tmp/session.env; cd; ($* ) </dev/null > /tmp/run.out 2>&1; echo \$? > /tmp/run.status" &
i=0
while [ $i -lt "$limit" ] && [ ! -e /tmp/run.status ]; do sleep 1; i=$((i + 1)); done
ps -o pid,user | awk '$2 == "tester" {print $1}' | sort > /tmp/run-now
for p in $(comm -13 /tmp/run-procs /tmp/run-now); do kill -9 "$p" 2>/dev/null; done
head -c 600 /tmp/run.out
echo
echo "status $(cat /tmp/run.status 2>/dev/null || echo stopped)"
"""

# Runs every exported command with --help as the user.
COMMANDS = r"""#!/bin/sh
for w in /usr/local/bin/*; do
  echo "## $(basename $w)"
  /tmp/run 20 "$w --help"
done
echo "## end"
"""


def grow_disk(path, size):
    """Extend a raw GPT disk image and its last partition (WormOS's) to size."""
    sector = 512
    with open(path, "r+b") as f:
        f.truncate(size)
        f.seek(sector)
        header = bytearray(f.read(92))
        entries_lba, count, entry_size = struct.unpack_from("<QII", header, 72)
        f.seek(entries_lba * sector)
        table = bytearray(f.read(count * entry_size))
        last_sector = size // sector - 1
        last_usable = last_sector - 33
        # The partition ending last is the system (BIOS boot and EFI come first).
        index = max(range(count), key=lambda i: struct.unpack_from(
            "<Q", table, i * entry_size + 40)[0])
        struct.pack_into("<Q", table, index * entry_size + 40, last_usable)
        table_crc = zlib.crc32(table) & 0xffffffff
        f.seek(entries_lba * sector)
        f.write(table)
        # Primary header: where the backup is, the last usable sector, the
        # table's checksum, then its own.
        struct.pack_into("<Q", header, 32, last_sector)
        struct.pack_into("<Q", header, 48, last_usable)
        struct.pack_into("<I", header, 88, table_crc)
        struct.pack_into("<I", header, 16, 0)
        struct.pack_into("<I", header, 16, zlib.crc32(header) & 0xffffffff)
        f.seek(sector)
        f.write(header)
        # The backup table and header at the end of the disk.
        backup = bytearray(header)
        struct.pack_into("<QQ", backup, 24, last_sector, 1)
        struct.pack_into("<Q", backup, 72, last_sector - 32)
        struct.pack_into("<I", backup, 16, 0)
        struct.pack_into("<I", backup, 16, zlib.crc32(backup) & 0xffffffff)
        f.seek((last_sector - 32) * sector)
        f.write(table)
        f.seek(last_sector * sector)
        f.write(backup)


def build_toplevels():
    """Compile tests/toplevels.c with the WormOS toolchain (None without one)."""
    if not os.access(GCC, os.X_OK):
        return None
    work = tempfile.mkdtemp(prefix="wormos-toplevels-")
    xml = os.path.join(ROOT, "src", "shell", "protocol",
                       "wlr-foreign-toplevel-management-unstable-v1.xml")
    header = os.path.join(work, "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h")
    code = os.path.join(work, "protocol.c")
    subprocess.run(["wayland-scanner", "client-header", xml, header], check=True)
    subprocess.run(["wayland-scanner", "private-code", xml, code], check=True)
    out = os.path.join(work, "toplevels")
    subprocess.run([GCC, "-O2", "-s", f"-I{work}", f"-I{SYSROOT}/usr/include", "-o", out,
                    os.path.join(ROOT, "tests", "toplevels.c"), code,
                    f"-L{SYSROOT}/usr/lib", "-lwayland-client"], check=True)
    with open(out, "rb") as f:
        return f.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kits", default="all", help="kits to install (all, or NAME,NAME)")
    parser.add_argument("--no-gui-apps", action="store_true",
                        help="leave out the graphical programs from Debian and Kali")
    parser.add_argument("--image", help="test this image instead of out/wormos.img")
    parser.add_argument("--timeout", type=int, default=600, help="seconds to boot")
    args = parser.parse_args()
    kits = KITS if args.kits == "all" else [k for k in args.kits.split(",") if k]

    toplevels = build_toplevels()
    if toplevels is None:
        print("SKIP  no WormOS toolchain to build the window lister (tests/toplevels.c)")
        return 0

    # On disk: the copy of the image grows by several GB (/tmp is often
    # in memory).
    tmp = tempfile.mkdtemp(prefix="apps-test-", dir=os.path.join(ROOT, "build"))
    sockets = tempfile.mkdtemp(prefix="wormos-apps-")
    mon_path = os.path.join(sockets, "monitor")
    qmp_path = os.path.join(sockets, "qmp")
    extra = (f"-vga none -device virtio-gpu-pci,addr=0x10,id=video0 "
             f"-device virtio-keyboard-pci,addr=0x11 -device virtio-tablet-pci,addr=0x12 "
             f"-monitor unix:{mon_path},server,nowait -qmp unix:{qmp_path},server,nowait")
    env = dict(os.environ, QEMU_EXTRA=extra, QEMU_MEM="6144", QEMU_CPUS="6",
               QEMU_VDAGENT_MOUSE="on")
    if args.image:
        env["IMAGE"] = os.path.abspath(args.image)
    # The image's 8 GiB do not hold every kit: a copy with a bigger disk
    # (a raw image shares its blocks with the original on btrfs).
    source = env.get("IMAGE", os.path.join(ROOT, "out", "wormos.img"))
    image = os.path.join(tmp, "apps.img")
    if source.endswith(".qcow2"):
        subprocess.run(["qemu-img", "convert", "-O", "raw", source, image], check=True)
    else:
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", source, image], check=True)
    grow_disk(image, 60 << 30)
    env["IMAGE"] = image
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh")],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    passed = failures = 0
    report = open(REPORT, "w")

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += bool(ok)
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}", flush=True)
        if not ok:
            print(f"      output: {out[-700:]!r}", flush=True)

    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", args.timeout)
            mon = desktop.Monitor(mon_path)
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            con.run("dmesg -n 1")
            # The disk's file system takes the whole (bigger) disk.
            con.run("worm unlock >/dev/null; python3 -c \"import fcntl,os,struct; "
                    "fcntl.ioctl(os.open('/', os.O_RDONLY), 0x50009403, "
                    "bytearray(struct.pack('q4088s', 0, b'max')))\"; worm lock >/dev/null")
            out = con.run("df -m / | tail -1")
            check("file system grown to the disk", re.search(r"\s(\d{5,})\s", out), out)

            # The graphical first-boot setup, as a person would answer it.
            deadline = time.time() + 300
            while "MODE=setup" not in con.run(desktop.GREETER_MODE) and time.time() < deadline:
                time.sleep(3)
            time.sleep(5)
            mon.key("ret")
            time.sleep(2)
            mon.type("Tester\n")
            time.sleep(1)
            mon.type(USER + "\n")
            time.sleep(1)
            mon.type(USER_PASSWORD + "\n")
            time.sleep(1)
            mon.type(USER_PASSWORD + "\n")
            time.sleep(2)
            for secret in (ROOT_PASSWORD, ROOT_PASSWORD):
                time.sleep(1)
                mon.type(secret + "\n")
            deadline = time.time() + 300
            while not re.search(r"^\d+", con.run("pidof worm-shell"), re.M):
                if time.time() > deadline:
                    raise TimeoutError("the desktop did not start after the setup")
                time.sleep(3)
            time.sleep(10)
            check("desktop session running", True)

            desktop.put_file(con, toplevels, "/tmp/toplevels")
            desktop.put_file(con, IN_SESSION.encode(), "/tmp/in-session")
            desktop.put_file(con, PROBE.encode(), "/tmp/probe")
            desktop.put_file(con, COMMANDS.encode(), "/tmp/commands")
            desktop.put_file(con, RUN.encode(), "/tmp/run")
            out = con.run("/tmp/in-session /tmp/toplevels")
            check("windows listed (the welcome terminal)", "foot" in out, out)
            out = con.run("ls -l $(/tmp/in-session 'echo $XDG_RUNTIME_DIR')/wormhole-bridge")
            check("the session's wormhole bridge is listening", out.startswith("p"), out)

            # The kits, then the graphical programs. Core packages are left
            # out: applying them restarts the system (other tests cover them).
            con.run("worm unlock >/dev/null; sed -i '/^core = /d' /usr/share/worm/kits/*; "
                    "worm lock >/dev/null")
            for kit in kits:
                begin = time.time()
                out = con.run(f"worm kit install {kit} 2>&1 | tail -3", 7200)
                check(f"kit {kit} installed ({time.time() - begin:.0f}s)",
                      re.search(r"(\d+) of \1 tools installed", out), out)
            if not args.no_gui_apps:
                for pkg, source in GUI_APPS:
                    out = con.run(f"wh -y install --from {source} {pkg} 2>&1 | tail -4", 3600)
                    check(f"{pkg} installed from {source}",
                          "->" in out or "already" in out, out)

            # The runtime: companions and stand-ins.
            out = con.run("whc debian dpkg-query -W -f '${Package} ${db:Status-Abbrev}\\n' "
                          "librsvg2-common qt6-wayland 2>&1")
            check("runtime rules added the SVG loader and Qt's Wayland plugin",
                  re.search(r"^librsvg2-common ii", out, re.M) and
                  re.search(r"^qt6-wayland ii", out, re.M), out)
            out = con.run("for n in sudo pkexec xdg-open x-terminal-emulator; do "
                          "readlink /var/lib/wormholes/debian/usr/local/bin/$n; done")
            check("stand-ins in the wormhole", out.count("../lib/wormhole/bridge") == 4, out)
            out = con.run("ls /var/lib/wormholes/debian/usr/share/doc/ | wc -l")
            check("documentation kept in wormholes", re.search(r"^\d{2,}", out), out)

            # Every menu entry, as the launcher starts it.
            entries = con.run("ls /usr/local/share/applications/*.desktop "
                              "/usr/share/applications/*.desktop").split()
            report.write("# Menu entries\n")
            for entry in entries:
                name = os.path.basename(entry)
                text = con.run(f"grep -E '^(Exec|Terminal|NoDisplay)=' {entry}")
                if "NoDisplay=true" in text:
                    continue
                terminal = "Terminal=true" in text
                wait = 150 if SLOW.search(name + text) else 40
                out = con.run(f"/tmp/probe {wait} {entry}", wait + 120)
                report.write(out + "\n")
                windows = re.findall(r"^   window: (.+)$", out, re.M)
                if NO_WINDOW.search(name):
                    continue
                ok = any("foot" in w for w in windows) if terminal else bool(windows)
                ok = ok and not BROKEN.search(out)
                check(f"{name} opens {'a terminal' if terminal else 'a window'}", ok, out)
                # A terminal entry's command, run here to read its output.
                m = re.search(r'^Exec=/usr/lib/worm/exec-in-shell "(.*)"$', text, re.M)
                if m and terminal:
                    cmd = m.group(1).replace("\\$", "$").replace('\\"', '"')
                    desktop.put_file(con, cmd.encode(), "/tmp/entry-command", "644")
                    out = con.run("/tmp/run 30 sh /tmp/entry-command", 120)
                    report.write(f"   command: {out!r}\n")
                    check(f"{name}: {cmd} runs", not BROKEN.search(out), out)

            # Every exported command.
            out = con.run("/tmp/commands", 7200)
            report.write("# Commands\n" + out + "\n")
            for block in out.split("## ")[1:]:
                head, _, body = block.partition("\n")
                if head.startswith("end"):
                    break
                cmd = head.strip()
                status = re.search(r"^status (\S+)$", body, re.M)
                status = status.group(1) if status else "?"
                if cmd in UPSTREAM:
                    continue
                check(f"command {cmd} runs", status not in ("126", "127") and
                      not BROKEN.search(body), body)

            # Kali's impacket keeps its scripts with its documentation.
            out = con.run("su tester -c 'impacket-GetADUsers -h 2>&1 | head -3'")
            check("impacket scripts run", "Impacket" in out, out)
            # Commands behind Debian's alternatives (nc is a link to
            # nc.openbsd) and full versions of BusyBox's commands.
            out = con.run("head -3 /usr/local/bin/nc /usr/local/bin/ipcalc 2>&1")
            check("alternatives and BusyBox's commands exported",
                  out.count("Exported by wh") == 2, out)
            # Packages split in parts (vlc, keepassxc) export their commands.
            out = con.run("ls /usr/local/bin/vlc /usr/local/bin/keepassxc 2>&1")
            check("split packages export their commands", "No such" not in out, out)
            # A package without menu entries gets one listing its commands.
            out = con.run("grep -h '^Exec=' /usr/local/share/applications/wormhole-hexyl.desktop")
            check("generated menu entry", "exec-in-shell --about hexyl" in out, out)
            out = con.run("/tmp/in-session '/usr/lib/worm/exec-in-shell --run --about hexyl'")
            check("generated entry shows the package's commands",
                  re.search(r"^\s+hexyl$", out, re.M), out)
            # Kali's helper scripts run inside the wormhole (john lists its
            # helpers there).
            out = con.run("grep -h '^Exec=' /usr/local/share/applications/kali-john.desktop")
            m = re.search(r"--in (\w+) --script john\.sh", out)
            check("helper script entry", m, out)
            # john comes from Debian, whose package keeps the *2john helpers
            # elsewhere than Kali's: the script ran when it printed its title.
            out = con.run(f"/tmp/in-session '/usr/lib/worm/exec-in-shell --run --in "
                          f"{m.group(1) if m else 'debian'} --script john.sh' 2>&1 | tail -5")
            check("helper script runs inside the wormhole", "John helpers" in out, out)

            # The stand-ins reach the bridge: a terminal and the root
            # terminal open on the desktop.
            def count(cmd):
                last = con.run(cmd).splitlines()[-1:] or ["0"]
                return int(last[0]) if last[0].isdigit() else 0
            before = count("/tmp/in-session /tmp/toplevels | grep -c foot")
            con.run("/tmp/in-session 'whc debian x-terminal-emulator -e sleep 30'")
            time.sleep(4)
            after = count("/tmp/in-session /tmp/toplevels | grep -c foot")
            check("x-terminal-emulator in a wormhole opens a terminal", after > before,
                  f"{before} {after}")
            con.run("/tmp/in-session 'whc debian sudo id' 2>&1")
            time.sleep(4)
            out = con.run("/tmp/in-session /tmp/toplevels")
            check("sudo in a wormhole opens the root terminal", "Root terminal" in out, out)
            con.run("pkill -f 'Root terminal' ; true")
            con.run("/tmp/in-session 'whc debian xdg-open https://example.org/wormos-test'")
            time.sleep(2)
            out = con.run("cat /run/user/1000/bridge.log")
            check("xdg-open in a wormhole reaches the bridge",
                  "open debian" in out and "example.org/wormos-test" in out, out)
            # Root's files are shared with root's wormhole programs.
            out = con.run("cd /root && echo here > rootfile && "
                          "whc debian cat /root/rootfile; rm -f /root/rootfile")
            check("root's home is shared with wormhole programs", out == "here", out)
            # sudo on the system (the root terminal's password prompt, here
            # answered on the serial console as root, who needs none).
            out = con.run("sudo id -u")
            check("sudo as root runs the command", out == "0", out)

            con.send("poweroff\n")
            con.expect("Power down", 300)
        except (TimeoutError, RuntimeError) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()
    report.close()
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.rmtree(sockets, ignore_errors=True)
    print(f"{passed}/{passed + failures} checks passed (report: {REPORT})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
