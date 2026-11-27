#!/usr/bin/env python3
"""Boot WormOS in QEMU for Windows and check it there.

Runs from WSL on a Windows PC with QEMU for Windows installed. QEMU's
serial console, monitor and QMP connect to this script over TCP (WSL
forwards the Windows side's localhost). The VM is what a Windows user would
start: UEFI firmware from QEMU's share folder, the Windows Hypervisor
Platform when it is available (TCG otherwise), a virtio GPU in a QEMU
window, virtio keyboard and tablet, HD Audio recorded to a WAV file.

Checks: the accelerator, network and encrypted DNS, the graphical
first-boot setup (typed through the monitor), the desktop, typing in the
terminal, what is on the screen, sound, the ACPI power button, and a second
boot with BIOS firmware (SeaBIOS). The full console output goes to
build/qemu-windows.log.

Usage: tests/qemu-windows-test.py [--timeout SECONDS] [--keep]
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "qemu-windows.log")
QEMU = os.environ.get("WORMOS_QEMU_WINDOWS", "/mnt/d/qemu/qemu-system-x86_64.exe")
WORKDIR = os.environ.get("WORMOS_QEMU_WINDOWS_DIR", "/mnt/d/wormos-tools/vms/qemu")


def load(name):
    spec = importlib.util.spec_from_file_location(
        name.replace("-", "_"), os.path.join(ROOT, "tests", f"{name}.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot-test")
desktop = load("desktop-test")
vmware = load("vmware-test")
USER, USER_PASSWORD, ROOT_PASSWORD = desktop.USER, desktop.USER_PASSWORD, desktop.ROOT_PASSWORD


def winpath(path):
    return vmware.winpath(path)


def listen():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Loopback only: WSL 2 forwards localhost ports to Windows, and the
    # VM's console must not be reachable from the network.
    s.bind(("127.0.0.1", 0))
    s.listen(1)
    s.settimeout(120)
    return s


def start(uefi, wav):
    """Start QEMU; return (process, console socket, monitor, QMP)."""
    serial, monitor, qmp = listen(), listen(), listen()
    share = os.path.join(os.path.dirname(QEMU), "share")
    args = [QEMU, "-machine", "q35", "-accel", "whpx", "-accel", "tcg", "-smp", "4", "-m", "2048",
            "-drive", f"file={winpath(os.path.join(WORKDIR, 'wormos.qcow2'))},format=qcow2,"
                      "if=none,id=disk0",
            "-device", "virtio-blk-pci,drive=disk0,bootindex=0", "-snapshot",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
            "-device", "virtio-rng-pci",
            "-vga", "none", "-device", "virtio-gpu-pci", "-display", "gtk",
            "-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci",
            "-audiodev", f"wav,id=snd0,path={winpath(wav)}", "-device", "ich9-intel-hda",
            "-device", "hda-duplex,audiodev=snd0",
            "-serial", f"tcp:127.0.0.1:{serial.getsockname()[1]}",
            "-monitor", f"tcp:127.0.0.1:{monitor.getsockname()[1]}",
            "-qmp", f"tcp:127.0.0.1:{qmp.getsockname()[1]}"]
    if uefi:
        vars_ = os.path.join(WORKDIR, "vars.fd")
        shutil.copy(os.path.join(share, "edk2-i386-vars.fd"), vars_)
        args += ["-drive", "if=pflash,format=raw,readonly=on,file="
                 + winpath(os.path.join(share, "edk2-x86_64-code.fd")),
                 "-drive", f"if=pflash,format=raw,file={winpath(vars_)}"]
    proc = subprocess.Popen(args, cwd=WORKDIR, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    socks = []
    for s in (serial, monitor, qmp):
        conn, _ = s.accept()
        s.close()
        socks.append(conn)
    mon = desktop.Monitor.__new__(desktop.Monitor)
    mon.sock = socks[1]
    mon.read()
    q = desktop.Qmp.__new__(desktop.Qmp)
    q.sock = socks[2]
    q.sock.settimeout(30)
    q.file = q.sock.makefile("rw")
    q.file.readline()
    q.execute("qmp_capabilities")
    return proc, socks[0], mon, q


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--keep", action="store_true", help="keep the work folder")
    args = parser.parse_args()

    shutil.rmtree(WORKDIR, ignore_errors=True)
    os.makedirs(WORKDIR)
    subprocess.run(["qemu-img", "convert", "-O", "qcow2", os.path.join(ROOT, "out", "wormos.img"),
                    os.path.join(WORKDIR, "wormos.qcow2")], check=True)
    wav = os.path.join(WORKDIR, "sound.wav")

    passed = failures = 0

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}", flush=True)
        if not ok:
            print(f"      output: {out!r}", flush=True)

    def wait_for(con, cmd, pattern, timeout):
        deadline = time.time() + timeout
        while True:
            out = con.run(cmd)
            if re.search(pattern, out, re.MULTILINE) or time.time() > deadline:
                return out
            time.sleep(2)

    proc = None
    with open(LOG, "wb") as log:
        try:
            start_time = time.time()
            proc, sock, mon, qmp = start(True, wav)
            con = boot_test.Console(vmware.SocketProc(sock), log)
            con.expect("login:", args.timeout)
            print(f"booted to login prompt in {time.time() - start_time:.0f}s", flush=True)
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            # Under WHPX the guest sees Hyper-V; under TCG no hypervisor.
            hv = con.run("dmesg | grep -c 'Hypervisor detected: Microsoft Hyper-V'")
            print(f"accelerator: {'whpx' if hv == '1' else 'tcg'}", flush=True)
            check("UEFI boot", con.run("test -d /sys/firmware/efi && echo efi") == "efi")
            out = wait_for(con, "curl -sS -o /dev/null -w '%{http_code}' https://deb.debian.org/",
                           r"^(200|301|302)$", 180)
            check("internet through encrypted DNS", re.search(r"^(200|301|302)$", out) is not None,
                  out)

            out = wait_for(con, desktop.GREETER_MODE, r"MODE=setup", 300)
            check("graphical first-boot setup shown", "MODE=setup" in out,
                  con.run("tail -n 5 /run/wormlogin/greeter.log"))
            time.sleep(5)
            mon.key("ret")
            time.sleep(2)
            mon.type("Tester\n")
            time.sleep(1)
            mon.type(USER + "\n")
            for secret in (USER_PASSWORD, USER_PASSWORD, ROOT_PASSWORD, ROOT_PASSWORD):
                time.sleep(1.5)
                mon.type(secret + "\n")
            out = wait_for(con, "snap list", r"First boot: created", 300)
            check("first-boot setup finished", "First boot: created" in out, out[-300:])
            out = wait_for(con, "pidof foot", r"^\d+", 300)
            check("desktop started", re.search(r"^\d+", out, re.M) is not None,
                  con.run("tail -n 20 /run/user/1000/desktop.log"))
            time.sleep(10)
            mon.type("echo $(id -un) typed > /tmp/typed\n")
            out = wait_for(con, "cat /tmp/typed", r"typed", 60)
            check("typed into the terminal", out == f"{USER} typed", out)

            shot = os.path.join(WORKDIR, "screen.ppm")
            mon.cmd(f"screendump {winpath(shot)}")
            time.sleep(2)
            width, height, pixels = desktop.read_ppm(shot)
            colors = {pixels[i:i + 3] for i in range(0, len(pixels), 3 * 997)}
            purple = sum(1 for c in colors if c[2] > c[1] + 40 and c[0] > c[1] + 20)
            check("screen shows the desktop", width >= 1024 and len(colors) > 50 and purple > 5,
                  f"{width}x{height}, {len(colors)} colours, {purple} purple; " +
                  con.run(f"su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 "
                          "wlr-randr' | head -8"))

            size = os.path.getsize(wav) if os.path.exists(wav) else 0
            out = con.run(f"su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 sh -c \"{desktop.PLAY.strip()}\"'",
                          timeout=90)
            time.sleep(2)
            grown = (os.path.getsize(wav) if os.path.exists(wav) else 0) - size
            check("sound plays", "played" in out and grown > 100000, f"{out}, {grown} bytes")

            mon.cmd("system_powerdown")
            out = con.expect("Power down", 120)
            check("power button powers off cleanly", "powering off" in out, out[-300:])
            proc.wait(timeout=60)

            # BIOS firmware (SeaBIOS), as QEMU uses without pflash drives.
            proc, sock, mon, qmp = start(False, wav)
            con = boot_test.Console(vmware.SocketProc(sock), log)
            con.expect("login:", args.timeout)
            # -snapshot dropped the setup: root has no password again.
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            out = con.run("test -d /sys/firmware/efi && echo efi || echo bios")
            check("boots with BIOS firmware", out == "bios", out)
            mon.cmd("system_powerdown")
            con.expect("Power down", 120)
            proc.wait(timeout=60)
        except (TimeoutError, RuntimeError, OSError, subprocess.TimeoutExpired) as e:
            check(f"test aborted: {e}", False)
        finally:
            if proc and proc.poll() is None:
                proc.kill()
                proc.wait()
            if not args.keep:
                time.sleep(2)
                shutil.rmtree(WORKDIR, ignore_errors=True)

    print(f"{passed}/{passed + failures} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
