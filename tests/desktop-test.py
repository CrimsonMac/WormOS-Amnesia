#!/usr/bin/env python3
"""Boot the WormOS image with a virtual GPU and check the desktop.

The VM gets a virtio GPU, keyboard and tablet, an HD Audio device whose
output QEMU records to a WAV file, and a QEMU monitor socket.
The test logs in as root on the serial console for checks, and types on
the graphical console through the monitor (sendkey), as a user would:
it answers the first-boot setup on tty1, logs in there (which starts the
desktop), types into the terminal and opens a new one with a shortcut.
It also checks the screen itself (screendump), resizes the screen the way a
viewer window would (through QEMU's VNC server) and measures memory use.
The full console output is written to build/desktop.log.

With --release IMAGE it checks a release image instead (such as
out/release/wormos-VERSION-x86_64.qcow2), where root has no password until
the first-boot setup: the setup is typed first, then root logs in on the
serial console with the password it set.

Usage: tests/desktop-test.py [--timeout SECONDS] [--release IMAGE]
"""

import argparse
import base64
import hashlib
import importlib.util
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "desktop.log")

# The serial console helper is shared with the boot test.
_spec = importlib.util.spec_from_file_location(
    "boot_test", os.path.join(ROOT, "tests", "boot-test.py"))
boot_test = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(boot_test)

USER = "tester"
USER_PASSWORD = "Worm-hole 42"
ROOT_PASSWORD = "Root:pass 7"

# Budgets from the project goals.
IDLE_RAM_MB = 400
ROOTFS_MB = 2000

DEVICE_CHECKS = [
    ("device manager running", "pidof udevd", r"^\d+"),
    ("modules come from kmod", "readlink /usr/sbin/modprobe", r"kmod$"),
    ("module loaded on demand", "modprobe uinput && lsmod | grep -c '^uinput '", r"^1$"),
    ("GPU device for the video group", "stat -c '%G %a' /dev/dri/card0", r"^video 660$"),
    ("render node for everyone", "stat -c '%G %a' /dev/dri/renderD128", r"^render 666$"),
    ("input devices for the input group",
     "stat -c '%G %a' /dev/input/event0", r"^input 660$"),
    ("devices have udev properties",
     "udevadm info /dev/input/event0 | grep -c ID_INPUT=1", r"^1$"),
    ("seat manager running", "pidof seatd", r"^\d+"),
    ("seat socket for the video group", "stat -c '%G %a' /run/seatd.sock", r"^video 770$"),
    ("Intel GPU firmware present", "ls /usr/lib/firmware/i915 | grep -c 'dmc.*\\.bin\\.zst'", r"^[1-9]"),
    ("su is setuid root", "stat -c '%U %a' /usr/bin/su", r"^root 4755$"),
]

GREETER_MODE = "tr '\\0' '\\n' < /proc/$(pidof wormos-greeter)/environ | grep MODE"
ACCENT = (0x7c, 0x3a, 0xed)

AFTER_SETUP_CHECKS = [
    ("user created", f"grep -c '^{USER}:x:1000:' /etc/passwd", r"^1$"),
    ("full name recorded", f"grep '^{USER}:' /etc/passwd | cut -d: -f5", r"^Tester$"),
    ("user in the desktop groups",
     f"id -Gn {USER} | tr ' ' '\\n' | grep -cxE 'wheel|video|input|render'", r"^4$"),
    ("user has a password", f"grep '^{USER}:' /etc/shadow | cut -d: -f2", r"^\$6\$"),
    ("root has a password", "grep '^root:' /etc/shadow | cut -d: -f2", r"^\$6\$"),
    ("time zone chosen by searching for a city", "readlink /etc/localtime",
     r"^/usr/share/zoneinfo/Europe/Bucharest$"),
    ("root is read-only again", "touch /probe 2>&1", r"Read-only file system"),
    ("setup snapshot taken", "snap list", r"First boot: created " + USER),
    ("runtime directory", "stat -c '%U %a' /run/user/1000", rf"^{USER} 700$"),
    ("setup not offered again", "/usr/lib/worm/first-boot --needed; echo rc=$?", r"rc=1"),
]

SESSION_CHECKS = [
    ("compositor running as the user", "ps -o user,comm | grep -w labwc",
     rf"^{USER}\s+labwc"),
    ("wayland socket", "ls /run/user/1000/", r"wayland-0"),
    ("software renderer on a virtual GPU", "grep -c 'pixman' /run/user/1000/desktop.log",
     r"^[1-9]"),
    ("wallpaper running", "pidof swaybg", r"^\d+"),
    ("welcome terminal open", "pidof foot", r"^\d+$"),
    ("shell running", "pidof worm-shell", r"^\d+$"),
    ("session bus for the user", "stat -c '%U %F' /run/user/1000/bus",
     rf"^{USER} socket$"),
    ("session bus answers",
     f"su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 dbus-send --session --print-reply "
     "--dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames'",
     r'string "org\.freedesktop\.DBus"'),
    ("programs on the desktop know the bus",
     "tr '\\0' '\\n' < /proc/$(pidof foot | cut -d' ' -f1)/environ | grep ^DBUS_SESSION",
     r"^DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus$"),
    ("standard folders", f"ls -1 /home/{USER}; grep -c DIR= /home/{USER}/.config/user-dirs.dirs",
     r"Documents(.|\n)*Downloads(.|\n)*Pictures(.|\n)*^8$"),
    # X11 programs: Xwayland starts when the first one connects. xkbcomp
    # is an X client too (it reads the server's keymap).
    ("programs on the desktop know the X display",
     "tr '\\0' '\\n' < /proc/$(pidof foot | cut -d' ' -f1)/environ | grep ^DISPLAY=",
     r"^DISPLAY=:\d+$"),
    ("an X11 program connects through Xwayland",
     f"d=$(tr '\\0' '\\n' < /proc/$(pidof foot | cut -d' ' -f1)/environ | sed -n 's/^DISPLAY=//p'); "
     f"su {USER} -c \"DISPLAY=$d xkbcomp -xkb $d /tmp/x11.xkb\" 2>&1 | grep -v -i warning | head -3; "
     "grep -c xkb_keymap /tmp/x11.xkb; pidof Xwayland | wc -w",
     r"^1\n1$"),
]

TYPING_CHECKS = [
    ("typed into the terminal", "cat /tmp/typed", rf"^{USER} typed$"),
]

# zsh as the user sees it: the WormOS settings from /etc/zsh/zshrc.
ZSH_CHECK = r"""zsh -ic 'bindkey "^W"; bindkey "^H"
print snap=$_comps[snap] wh=$_comps[wh] bh=$_comps[bh] worm=$_comps[worm] clip=$_comps[clip]
whence -w _zsh_autosuggest_start _zsh_highlight
_wormos_prompt_state; print -r -- "state=$_wormos_state"; print history=$SAVEHIST'
"""
SHELL_CHECKS = [
    ("users get zsh", f"grep ^{USER}: /etc/passwd", r":/usr/bin/zsh$"),
    ("root gets zsh too", "grep ^root: /etc/passwd", r":/usr/bin/zsh$"),
    ("foot sends ^W for Ctrl+Backspace",
     "grep -A4 '^\\[text-bindings\\]' /etc/xdg/foot/foot.ini", r"^\\x17 = Control\+BackSpace$"),
    ("zsh: Ctrl+Backspace deletes a word", f"su {USER} -c 'sh /tmp/zsh-check' 2>&1",
     r'"\^W" backward-kill-word\s+"\^H" backward-kill-word'),
    ("zsh completes snap, wh, bh, worm and clip", f"su {USER} -c 'sh /tmp/zsh-check' 2>&1",
     r"snap=_snap wh=_wh bh=_bh worm=_worm clip=_clip"),
    ("zsh suggestions and highlighting loaded", f"su {USER} -c 'sh /tmp/zsh-check' 2>&1",
     r"_zsh_autosuggest_start: function\s+_zsh_highlight: function"),
    ("prompt shows the newest snapshot", f"su {USER} -c 'sh /tmp/zsh-check' 2>&1",
     r"^state=.*\[snap \d+\]"),
    ("large shared history", f"su {USER} -c 'sh /tmp/zsh-check' 2>&1", r"^history=50000$"),
    ("zsh knows the terminal (moves the cursor over suggestions)",
     "TERM=foot script -qfc \"zsh -fic 'zmodload zsh/terminfo; print cub1=\\${+terminfo[cub1]}'\" /dev/null",
     r"cub1=1"),
]

CLIPBOARD_CHECKS = [
    ("clipboard agent running", "pidof clip", r"^\d+$"),
    ("agent port for the video group",
     "stat -L -c '%G %a' /dev/virtio-ports/com.redhat.spice.0", r"^video 660$"),
]

SHORTCUT_CHECKS = [
    ("Super+Enter opens a terminal", "pidof foot | wc -w", r"^2$"),
]

# A new terminal greets the user (its login shell runs /etc/profile).
GREETING = ("echo exit | env -u WORMOS_GREETED su " + USER +
            " -c 'WAYLAND_DISPLAY=wayland-0 sh -il' 2>&1")
WELCOME_CHECKS = [
    ("new terminals start login shells", "grep -c '^login-shell=yes' /etc/xdg/foot/foot.ini",
     r"^1$"),
    ("new terminal shows the welcome", GREETING, r"Tip: .+"),
    ("welcome --full shows the guide", "welcome --full", r"Welcome to WormOS"),
]

GRIM = (f"su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 "
        "grim /tmp/shot.png' && od -An -c -N4 /tmp/shot.png")
FOOT = (f"cd /tmp && su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 "
        "foot true' 2>&1 | grep -c -e compose -e locale; true")
BUDGET_CHECKS = [
    ("screenshot with grim", GRIM, r"P\s+N\s+G"),
    ("terminal has a UTF-8 locale and compose table", FOOT, r"^0$"),
]


SOUND_CHECKS = [
    ("sound card found", "cat /proc/asound/cards", r"HDA-Intel"),
    ("codec driver loaded as a module", "grep -c '^snd_hda_codec_generic ' /proc/modules",
     r"^1$"),
    ("sound devices for the audio group", "stat -c '%G %a' /dev/snd/pcmC0D0p", r"^audio 660$"),
    ("user in the audio group", f"id -Gn {USER} | tr ' ' '\\n' | grep -cx audio", r"^1$"),
    ("sound server running as the user",
     "for p in pipewire wireplumber pipewire-pulse; do stat -c %U /proc/$(pidof -s $p); done",
     rf"^{USER}\n{USER}\n{USER}$"),
    ("PulseAudio socket for programs", "stat -c %F /run/user/1000/pulse/native", r"^socket$"),
    ("no errors from the sound server", "grep -c -E ' E | C ' /run/user/1000/sound.log",
     r"^0$"),
]

# One second of noise through PipeWire, then the volume as the shell sees it.
PLAY = ("head -c 192000 /dev/urandom | pw-cat --playback --raw --format s16 --rate 48000 "
        "--channels 2 - && echo played\n")
VOLUME = "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null"


def wav_peak(path):
    """The loudest 16-bit sample QEMU recorded from the guest's output."""
    try:
        with open(path, "rb") as f:
            data = f.read()[44:]
    except OSError:
        return 0
    data = data[:len(data) // 2 * 2]
    return max((abs(v) for (v,) in struct.iter_unpack("<h", data)), default=0)


# Window sizes a viewer asks for, including odd ones; the desktop follows.
RESIZES = [(1600, 900), (1366, 768), (2560, 1440), (1131, 777)]


GCC = os.path.join(ROOT, "build", "tools", "bin", "x86_64-wormos-linux-gnu-gcc")


def build_gtk_check():
    """Compile tests/gtk-check.c with the WormOS toolchain; return its bytes
    (None without a toolchain, as on a CI runner testing a release)."""
    if not os.access(GCC, os.X_OK):
        return None
    sysroot = os.path.join(ROOT, "build", "sysroot")
    env = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=sysroot,
               PKG_CONFIG_LIBDIR=f"{sysroot}/usr/lib/pkgconfig:{sysroot}/usr/share/pkgconfig")
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "gtk4"], env=env,
                           check=True, capture_output=True, text=True).stdout.split()
    out = os.path.join(ROOT, "build", "gtk-check")
    subprocess.run([GCC, "-O2", "-s", "-o", out, os.path.join(ROOT, "tests", "gtk-check.c")] + flags,
                   check=True)
    with open(out, "rb") as f:
        return f.read()


def put_file(con, data, path, mode="755"):
    """Copy data into the VM over the serial console (base64, in pieces)."""
    text = base64.b64encode(data).decode()
    lines = [text[i:i + 76] for i in range(0, len(text), 76)]
    con.run(f"rm -f {path}.b64")
    for i in range(0, len(lines), 12):
        quoted = " ".join(f"'{line}'" for line in lines[i:i + 12])
        con.run(f"printf '%s\\n' {quoted} >> {path}.b64")
    con.run(f"base64 -d {path}.b64 > {path}; rm {path}.b64; chmod {mode} {path}")
    # Short commands: a long one wraps on the console and hides its output.
    out = con.run(f"sha256sum {path}")
    return hashlib.sha256(data).hexdigest() in out


# Runs a command as on the desktop, with the session's environment.
IN_SESSION = r"""pid=$(pidof labwc)
vars=$(tr '\0' '\n' < /proc/$pid/environ | grep -E '^(WAYLAND_DISPLAY|XDG_[A-Z_]+|DBUS_SESSION_BUS_ADDRESS|GSK_RENDERER|GTK_A11Y|LANG|HOME|PATH|LABWC_PID)=')
exec env -i $vars "$@"
"""

NOTIFY = ("gdbus call --session --dest org.freedesktop.Notifications "
          "--object-path /org/freedesktop/Notifications "
          "--method org.freedesktop.Notifications.Notify "
          "Tests 0 dialog-information-symbolic 'Snapshot taken' 'Number 3: before the update' "
          "'[]' '{}' 20000")
SCALE = "wlr-randr | grep -i scale"

GTK_CHECKS = [
    ("GTK uses the WormOS theme", "cat /tmp/gtk.out", r"^theme=WormOS$"),
    ("GTK finds the Adwaita icons", "cat /tmp/gtk.out", r"^icons=Adwaita$(.|\n)*^folder icon=yes$"),
    ("GTK uses the Adwaita cursors", "cat /tmp/gtk.out", r"^cursor=Adwaita$"),
    ("GTK uses the system font", "cat /tmp/gtk.out", r"^font=Inter Variable 10$"),
    ("settings are kept by dconf", "cat /tmp/gtk.out", r"^settings store=DConfSettingsBackend$"),
    ("GTK draws with cairo on a virtual GPU", "cat /tmp/gtk.out", r"^renderer=GskCairoRenderer$"),
    ("no GTK warnings", "grep -c -i -e warning -e critical /tmp/gtk.out", r"^0$"),
]


class Monitor:
    """The QEMU monitor (HMP) on a UNIX socket."""

    # sendkey names for characters that are not plain letters or digits.
    KEYS = {
        " ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot",
        ",": "comma", "/": "slash", ";": "semicolon", "'": "apostrophe",
        "_": "shift-minus", ":": "shift-semicolon", ">": "shift-dot",
        "<": "shift-comma", '"': "shift-apostrophe", "!": "shift-1",
        "?": "shift-slash", "|": "shift-backslash", "$": "shift-4",
        "(": "shift-9", ")": "shift-0", "*": "shift-8", "&": "shift-7",
    }

    def __init__(self, path):
        deadline = time.time() + 30
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.read()

    timeout = 60    # seconds to wait for an answer (screendumps can be slow)

    def read(self):
        data = b""
        self.sock.settimeout(self.timeout)
        while not data.endswith(b"(qemu) "):
            data += self.sock.recv(4096)
        return data.decode(errors="replace")

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\n")
        return self.read()

    def key(self, name):
        self.cmd(f"sendkey {name}")
        time.sleep(0.08)

    def type(self, text):
        for ch in text:
            if ch in self.KEYS:
                self.key(self.KEYS[ch])
            elif ch.isupper():
                self.key(f"shift-{ch.lower()}")
            else:
                self.key(ch)


class Qmp:
    """QEMU's machine protocol, for pointer events on the tablet (the
    monitor's mouse_move only moves relative pointers)."""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(10)
        self.sock.connect(path)
        self.file = self.sock.makefile("rw")
        self.file.readline()                       # greeting
        self.execute("qmp_capabilities")

    def execute(self, command, **arguments):
        self.file.write(json.dumps({"execute": command, "arguments": arguments}) + "\n")
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if "return" in reply or "error" in reply:
                return reply

    def click(self, x, y, width, height, button):
        """Move the tablet to (x, y) on a width x height screen and click."""
        def abs_(axis, v, size):
            return {"type": "abs", "data": {"axis": axis, "value": v * 0x7fff // size}}
        # The display named: without one QEMU's vdagent scales the
        # position to 1024x768 instead of the screen's size.
        self.execute("input-send-event", device="video0", head=0,
                     events=[abs_("x", x, width), abs_("y", y, height)])
        time.sleep(0.5)
        for down in (True, False):
            self.execute("input-send-event", device="video0", head=0, events=[
                {"type": "btn", "data": {"down": down, "button": button}}])
            time.sleep(0.2)

    def key(self, qcode, down):
        """Press or release one key (sendkey always does both)."""
        self.execute("input-send-event", events=[
            {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}])
        time.sleep(0.3)


class Vnc:
    """A minimal VNC client for QEMU's server: it only asks for a new size,
    as a viewer does when its window is resized (ExtendedDesktopSize)."""

    ENCODINGS = (0, -308)                          # raw, ExtendedDesktopSize

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(10)
        self.sock.connect(path)
        self.recv(12)                              # ProtocolVersion
        self.sock.sendall(b"RFB 003.008\n")
        count = self.recv(1)[0]
        if 1 not in self.recv(count):              # security type None
            raise RuntimeError("VNC server wants authentication")
        self.sock.sendall(b"\x01")
        if struct.unpack(">I", self.recv(4))[0] != 0:
            raise RuntimeError("VNC security handshake failed")
        self.sock.sendall(b"\x01")                 # ClientInit: shared
        header = self.recv(24)                     # ServerInit
        self.recv(struct.unpack(">I", header[20:24])[0])
        self.sock.sendall(struct.pack(">BxH", 2, len(self.ENCODINGS)) +
                          b"".join(struct.pack(">i", e) for e in self.ENCODINGS))

    def recv(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise RuntimeError("VNC connection closed")
            data += chunk
        return data

    def resize(self, width, height):
        """SetDesktopSize with one screen."""
        self.sock.sendall(struct.pack(">BxHHBx", 251, width, height, 1) +
                          struct.pack(">IHHHHI", 0, 0, 0, width, height, 0))


class VncScreen(Vnc):
    """The whole screen through VNC, as a PPM file. QEMU's screendump cannot
    read a GL screen (virgl with egl-headless: "no surface"); its VNC
    server can."""

    def grab(self, path):
        # 32 bits per pixel, little-endian, red at bit 16 (BGRX in memory).
        self.sock.sendall(struct.pack(">BxxxBBBBHHHBBBxxx", 0, 32, 24, 0, 1,
                                      255, 255, 255, 16, 8, 0))
        self.sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, 0xffff, 0xffff))
        self.sock.settimeout(60)
        width = height = 0
        rects = []
        while True:
            kind = self.recv(1)[0]
            if kind == 2:
                continue
            if kind == 3:
                self.recv(struct.unpack(">xxxI", self.recv(7))[0])
                continue
            if kind != 0:
                raise RuntimeError(f"unexpected VNC message {kind}")
            count = struct.unpack(">xH", self.recv(3))[0]
            for _ in range(count):
                x, y, w, h, enc = struct.unpack(">HHHHi", self.recv(12))
                if enc == 0:
                    rects.append((x, y, w, h, self.recv(w * h * 4)))
                    width, height = max(width, x + w), max(height, y + h)
                elif enc == -308:
                    n = struct.unpack(">Bxxx", self.recv(4))[0]
                    self.recv(16 * n)
            if rects:
                break
        rgb = bytearray(width * height * 3)
        for x, y, w, h, data in rects:
            for row in range(h):
                line = data[row * w * 4:(row + 1) * w * 4]
                out = ((y + row) * width + x) * 3
                rgb[out:out + w * 3:3] = line[2::4]
                rgb[out + 1:out + w * 3:3] = line[1::4]
                rgb[out + 2:out + w * 3:3] = line[0::4]
        with open(path, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (width, height) + bytes(rgb))
        self.sock.close()


class VncClipboard(Vnc):
    """A VNC client using the clipboard, as viewers do: plain ClientCutText
    to copy, and the extended clipboard (notify, request, provide) to paste
    what the guest copied. QEMU passes both on through its vdagent."""

    ENCODINGS = (0, -1063131698)                   # raw, extended clipboard
    TEXT, CAPS, REQUEST, NOTIFY, PROVIDE = 1, 1 << 24, 1 << 25, 1 << 27, 1 << 28

    def __init__(self, path):
        super().__init__(path)
        flags = self.CAPS | self.TEXT | self.REQUEST | self.NOTIFY | self.PROVIDE
        self.sock.sendall(struct.pack(">BxxxiII", 6, -8, flags, 1 << 20))

    def copy(self, text):
        data = text.encode("latin-1")
        self.sock.sendall(struct.pack(">BxxxI", 6, len(data)) + data)

    def message(self):
        """The next clipboard message: (flags, payload), or None for text."""
        while True:
            kind = self.recv(1)[0]
            if kind == 2:                          # Bell
                continue
            if kind != 3:
                raise RuntimeError(f"unexpected VNC message {kind}")
            length = struct.unpack(">xxxi", self.recv(7))[0]
            if length >= 0:
                self.recv(length)
                return None
            data = self.recv(-length)
            return struct.unpack(">I", data[:4])[0], data[4:]

    def paste(self, timeout):
        """Waits for the guest's copy, asks for it and returns the text."""
        deadline = time.time() + timeout
        self.sock.settimeout(timeout)
        try:
            while time.time() < deadline:
                msg = self.message()
                if msg and msg[0] & self.NOTIFY and msg[0] & self.TEXT:
                    self.sock.sendall(struct.pack(">BxxxiI", 6, -4, self.REQUEST | self.TEXT))
                elif msg and msg[0] & self.PROVIDE and msg[0] & self.TEXT:
                    data = zlib.decompress(msg[1])
                    size = struct.unpack(">I", data[:4])[0]
                    return data[4:4 + size].rstrip(b"\0").decode(errors="replace")
        except (socket.timeout, OSError):
            pass
        finally:
            self.sock.settimeout(10)
        return None


def near(pixels, width, x, y, rgb, tolerance):
    i = 3 * (y * width + x)
    return all(abs(pixels[i + k] - rgb[k]) < tolerance for k in range(3))


def solid_block(pixels, width, height, rgb):
    """Centre of the solid area in colour rgb (a button, not a 1-pixel border)."""
    xs, ys = [], []
    for y in range(0, height - 8, 2):
        for x in range(0, width - 16, 2):
            if (near(pixels, width, x, y, rgb, 14) and near(pixels, width, x, y + 8, rgb, 14)
                    and near(pixels, width, x + 16, y, rgb, 14)):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs) + max(xs) + 16) // 2, (min(ys) + max(ys) + 8) // 2


def region_diff(a, b, width, box):
    """Count pixels that differ between two screens inside box (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, y1, 2):
        row = 3 * (y * width)
        for x in range(x0, x1, 2):
            i = row + 3 * x
            if a[i:i + 3] != b[i:i + 3]:
                n += 1
    return n


def read_ppm(path):
    """Return (width, height, pixels) of a binary PPM (P6) file."""
    with open(path, "rb") as f:
        data = f.read()
    fields = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    width, height = int(fields.group(1)), int(fields.group(2))
    pixels = data[fields.end():]
    # QEMU pads rows at odd widths (1131 pixels are written as 1132).
    row = len(pixels) // height
    if row != 3 * width:
        pixels = b"".join(pixels[y * row:y * row + 3 * width] for y in range(height))
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds to wait for the login prompt")
    parser.add_argument("--virgl", action="store_true",
                        help="a 3D virtio GPU (virgl), as virt-manager with OpenGL on; "
                             "needs a render node on this machine")
    parser.add_argument("--release", metavar="IMAGE",
                        help="check this release image (root locked until set up)")
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="wormos-desktop-")
    mon_path = os.path.join(tmp, "monitor")
    vnc_path = os.path.join(tmp, "vnc")
    qmp_path = os.path.join(tmp, "qmp")
    wav_path = os.path.join(tmp, "sound.wav")
    gpu = ("virtio-gpu-gl-pci,addr=0x10,id=video0 -display egl-headless" if args.virgl
           else "virtio-gpu-pci,addr=0x10,id=video0")
    if args.virgl:
        # With a GPU the session renders with GLES, and GTK with GL.
        for checks in (SESSION_CHECKS, GTK_CHECKS):
            for i, (name, cmd, pattern) in enumerate(checks):
                if name == "software renderer on a virtual GPU":
                    checks[i] = ("GLES renderer on the 3D virtual GPU",
                                 "p=$(pidof labwc | cut -d' ' -f1); "
                                 "tr '\\0' '\\n' < /proc/$p/environ | grep -c WLR_RENDERER=pixman; "
                                 "grep -c -e virtio_gpu -e libgallium /proc/$p/maps",
                                 r"^0\n[1-9]")
                elif name == "GTK draws with cairo on a virtual GPU":
                    # GTK 4.24 keeps drawing with cairo here; the compositor
                    # uses the GPU either way.
                    checks[i] = ("GTK draws on the 3D virtual GPU", cmd,
                                 r"^renderer=Gsk(GL|NGL|Cairo)Renderer$")
    extra = (f"-vga none -device {gpu} "
             f"-device virtio-keyboard-pci,addr=0x11 -device virtio-tablet-pci,addr=0x12 "
             f"-monitor unix:{mon_path},server,nowait -vnc unix:{vnc_path} "
             f"-qmp unix:{qmp_path},server,nowait "
             f"-audiodev wav,id=snd0,path={wav_path} -device ich9-intel-hda,addr=0x13 "
             f"-device hda-duplex,audiodev=snd0")
    # The pointer goes through the guest's SPICE agent once it says it can
    # take it, as with virt-manager: the click tests then test the agent.
    env = dict(os.environ, QEMU_EXTRA=extra, QEMU_MEM="1024", QEMU_VDAGENT_MOUSE="on")
    if args.release:
        env["IMAGE"] = os.path.abspath(args.release)
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    passed = failures = 0
    gtk_check = build_gtk_check()
    total = (len(DEVICE_CHECKS) + 1 + len(AFTER_SETUP_CHECKS) + 3 + len(SESSION_CHECKS) +
             len(TYPING_CHECKS) + len(SHORTCUT_CHECKS) + len(BUDGET_CHECKS) +
             len(RESIZES) + len(WELCOME_CHECKS) + len(GTK_CHECKS) + len(SOUND_CHECKS) +
             len(SHELL_CHECKS) + len(CLIPBOARD_CHECKS) + 37)
    if gtk_check is None:
        total -= len(GTK_CHECKS) + 3

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    def run_checks(con, checks):
        for name, cmd, pattern in checks:
            out = con.run(cmd)
            check(name, re.search(pattern, out, re.MULTILINE) is not None, out)

    def wait_for(con, cmd, pattern, timeout):
        """Run cmd until its output matches; return the last output."""
        deadline = time.time() + timeout
        while True:
            out = con.run(cmd)
            if re.search(pattern, out, re.MULTILINE) or time.time() > deadline:
                return out
            time.sleep(2)

    start = time.time()
    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", args.timeout)
            print(f"booted to login prompt in {time.time() - start:.0f}s")
            mon = Monitor(mon_path)

            def screendump(path):
                if os.path.exists(path):
                    os.unlink(path)
                if args.virgl:
                    VncScreen(vnc_path).grab(path)
                    return ""
                return mon.cmd(f"screendump {path}")

            def screen(name):
                path = os.path.join(ROOT, "build", f"desktop-{name}.ppm")
                screendump(path)
                time.sleep(1)
                return read_ppm(path)

            def first_boot():
                """Answer the graphical first-boot setup, on the keyboard."""
                # Wait for the setup screen: its "Get started" button.
                deadline = time.time() + 300
                while time.time() < deadline:
                    w, h, px = screen("setup")
                    if solid_block(px, w, h, ACCENT):
                        break
                    time.sleep(5)
                mon.key("ret")                  # Get started
                time.sleep(2)
                mon.type("Tester\n")            # the user name is suggested from it
                time.sleep(1)
                mon.type(USER + "\n")
                time.sleep(1)
                mon.type(USER_PASSWORD + "\n")
                time.sleep(1)
                mon.type(USER_PASSWORD)
                # The time zone list: its search finds a city anywhere in
                # the name, and Enter takes the first match.
                mon.key("tab")                  # keyboard layout
                mon.key("tab")                  # time zone
                time.sleep(1)
                mon.key("spc")
                time.sleep(2)
                mon.type("buch")
                time.sleep(2)
                mon.key("ret")
                time.sleep(2)
                mon.key("shift-tab")
                mon.key("shift-tab")            # back to the password
                time.sleep(1)
                mon.key("ret")
                time.sleep(2)                   # the root password page
                for secret in (ROOT_PASSWORD, ROOT_PASSWORD):
                    time.sleep(1)
                    mon.type(secret + "\n")

            if args.release:
                # Root cannot log in before the setup; afterwards it can,
                # with the password typed into the setup.
                con.send("root\n")
                con.expect("Password:", 60)
                con.send("\n")
                # The prompt itself: busybox's error also starts with "login:".
                out = con.expect("wormos login:", 120)
                # busybox login refuses a locked password ("!") as a bad salt.
                check("root locked before the setup",
                      "incorrect" in out or "bad salt" in out, out[-200:])
                time.sleep(5)
                first_boot()
                deadline = time.time() + 600
                while True:
                    con.send("root\n")
                    con.expect("Password:", 60)
                    con.send(ROOT_PASSWORD + "\n")
                    try:
                        con.expect("# ", 15)
                        con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
                        break
                    except TimeoutError:
                        if time.time() > deadline:
                            raise
                        con.expect("wormos login:", 60)
                check("first-boot setup finished", True)
                run_checks(con, DEVICE_CHECKS)
                # Its snapshot (a manifest of the whole system) and the
                # session may still be under way.
                wait_for(con, "snap list", r"First boot: created", 300)
                wait_for(con, "ls -d /run/user/1000", r"^/run/user/1000$", 120)
            else:
                con.send("root\n")
                con.expect("# ", 60)
                con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
                run_checks(con, DEVICE_CHECKS)
                out = wait_for(con, GREETER_MODE, r"MODE=setup", 300)
                check("graphical first-boot setup shown", "MODE=setup" in out,
                      con.run("cat /run/wormlogin/greeter.log; tail /run/wormlogin/greeter/labwc.log"))
                first_boot()
                out = wait_for(con, "snap list", r"First boot: created", 300)
                check("first-boot setup finished", "First boot: created" in out, out[-300:])
            run_checks(con, AFTER_SETUP_CHECKS)

            # The new user is logged in right away: the desktop starts.
            out = wait_for(con, "pidof foot", r"^\d+", 300)
            check("desktop started after the setup", re.search(r"^\d+", out, re.M) is not None,
                  con.run("tail -n 20 /run/user/1000/desktop.log"))
            time.sleep(5)
            run_checks(con, SESSION_CHECKS)

            # The session's agent took the pointer over (QEMU marks the
            # mouse in use with a star).
            for _ in range(30):
                mice = mon.cmd("info mice")
                if re.search(r"^\s*\*.*vdagent", mice, re.M | re.I):
                    break
                time.sleep(1)
            check("pointer goes through the SPICE agent",
                  re.search(r"^\s*\*.*vdagent", mice, re.M | re.I) is not None, mice)

            # The welcome terminal has the focus.
            mon.type("echo $(id -un) typed > /tmp/typed\n")
            wait_for(con, "cat /tmp/typed", r"typed", 60)
            run_checks(con, TYPING_CHECKS)

            # The terminal's shell: Ctrl+Backspace deletes the last word,
            # and an earlier command comes back as a suggestion (End takes
            # it). zsh writes the history as it goes.
            mon.type("echo $(id -un) one two")
            mon.key("ctrl-backspace")
            mon.type("> /tmp/word\n")
            out = wait_for(con, "cat /tmp/word", r"\S", 60)
            check("Ctrl+Backspace deletes the last word", out == f"{USER} one", out)
            mon.type(": recall; echo $(id -un) recalled > /tmp/recalled\n")
            wait_for(con, "cat /tmp/recalled", r"recalled", 60)
            con.run("rm -f /tmp/recalled")
            mon.type(": recall")
            time.sleep(3)
            mon.key("end")
            time.sleep(1)
            mon.key("ret")
            out = wait_for(con, "cat /tmp/recalled", rf"^{USER} recalled$", 60)
            check("suggestion from the history taken with End", out == f"{USER} recalled", out)
            # Short commands: a long one wraps on the console.
            out = con.run(f"grep -c recall ~{USER}/.zsh_h*")
            check("history saved as you go", out.isdigit() and int(out) >= 1, out)
            put_file(con, ZSH_CHECK.encode(), "/tmp/zsh-check")
            run_checks(con, SHELL_CHECKS)

            mon.key("meta_l-ret")
            wait_for(con, "pidof foot | wc -w", r"^2$", 60)
            run_checks(con, SHORTCUT_CHECKS)

            # Tiling from the keyboard: the new terminal, snapped to the left
            # half, then maximized, is about half as wide as then.
            time.sleep(2)
            mon.key("meta_l-left")
            time.sleep(2)
            mon.type("stty size > /tmp/tile-half\n")
            time.sleep(1)
            mon.key("meta_l-f")
            time.sleep(2)
            mon.type("stty size > /tmp/tile-full\n")
            time.sleep(1)
            mon.key("meta_l-f")
            time.sleep(1)
            mon.key("meta_l-down")
            time.sleep(1)
            half = con.run("cut -d' ' -f2 /tmp/tile-half")
            full = con.run("cut -d' ' -f2 /tmp/tile-full")
            ok = half.isdigit() and full.isdigit() and 1.8 < int(full) / max(int(half), 1) < 2.3
            check("Super+Left tiles to half the screen, Super+F maximizes", ok,
                  f"{half} columns tiled, {full} maximized")

            # What is really on the screen: the wallpaper's purple and the
            # dark terminal, not a blank or text console.
            time.sleep(3)
            shot = os.path.join(tmp, "screen.ppm")
            said = screendump(shot)
            time.sleep(1)
            if not os.path.exists(shot):
                raise RuntimeError(f"no screendump: {said!r}")
            width, height, pixels = read_ppm(shot)
            colors = {pixels[i:i + 3] for i in range(0, len(pixels), 3 * 997)}
            purple = sum(1 for c in colors if c[2] > c[1] + 40 and c[0] > c[1] + 20)
            check("screen shows the desktop",
                  width >= 1024 and len(colors) > 50 and purple > 5,
                  f"{width}x{height}, {len(colors)} colours, {purple} purple")

            run_checks(con, WELCOME_CHECKS)

            # Clicks on the background: the left button does nothing, the
            # right one opens the menu. New windows open in the middle; the
            # strip along the right edge shows only the wallpaper.
            qmp = Qmp(qmp_path)

            # Caps Lock turns off as the key goes down: a letter typed
            # while it is still held comes out in lower case.
            mon.type("echo x")
            qmp.key("caps_lock", True)
            qmp.key("caps_lock", False)
            mon.key("a")
            qmp.key("caps_lock", True)
            mon.key("b")
            qmp.key("caps_lock", False)
            mon.type(" > /tmp/caps\n")
            out = wait_for(con, "cat /tmp/caps", r"\S", 60)
            check("Caps Lock turns off on press", out == "xAb", out)
            px, py = width - 60, height - 80
            box = (width - width // 8, 0, width, height)
            before = screen("before-click")[2]
            qmp.click(px, py, width, height, "left")
            time.sleep(2)
            changed = region_diff(before, screen("left-click")[2], width, box)
            check("left click on the desktop opens no menu", changed < 50,
                  f"{changed} pixels changed")
            qmp.click(px, py, width, height, "right")
            time.sleep(2)
            changed = region_diff(before, screen("right-click")[2], width, box)
            check("right click on the desktop opens the menu", changed > 1000,
                  f"{changed} pixels changed")
            mon.key("esc")
            time.sleep(1)

            # The screen follows the viewer's window size.
            vnc = Vnc(vnc_path)
            for want in RESIZES:
                vnc.resize(*want)
                deadline = time.time() + 60
                while True:
                    time.sleep(2)
                    screendump(shot)
                    time.sleep(1)
                    got = read_ppm(shot)[:2]
                    if got == want or time.time() > deadline:
                        break
                check(f"screen follows the window: {want[0]}x{want[1]}", got == want,
                      f"{got[0]}x{got[1]}; " +
                      con.run("grep -i 'preferred mode' /run/user/1000/desktop.log | tail -n 3"))

            # A GTK program on the desktop.
            if gtk_check is None:
                print(f"SKIP  the GTK program checks ({len(GTK_CHECKS) + 3}): no WormOS toolchain")
            else:
                check("GTK test program copied in", put_file(con, gtk_check, "/tmp/gtk-check"))
            put_file(con, IN_SESSION.encode(), "/tmp/in-session")

            def session(cmd):
                return con.run(f"su {USER} -c '/tmp/in-session {cmd}'")

            # The clipboard, shared with the viewer through QEMU's vdagent.
            run_checks(con, CLIPBOARD_CHECKS)
            clipboard = VncClipboard(vnc_path)
            clipboard.copy("copied on the host 42")
            put_file(con, b"/tmp/in-session clip paste; echo\n", "/tmp/paste")
            out = wait_for(con, f"su {USER} -c 'sh /tmp/paste'", r"^copied on the host 42$", 60)
            check("text copied on the host pastes in the guest",
                  out == "copied on the host 42", out + con.run("cat /run/user/1000/clip.log"))
            session("clip copy copied in the guest 7")
            text = clipboard.paste(60)
            check("text copied in the guest reaches the host", text == "copied in the guest 7",
                  repr(text) + con.run("cat /run/user/1000/clip.log"))

            if gtk_check is not None:
                con.run(f"su {USER} -c '/tmp/in-session /tmp/gtk-check > /tmp/gtk.out 2>&1 &'")
                out = wait_for(con, "cat /tmp/gtk.out", r"^shown$", 120)
                check("GTK window shown", re.search(r"^shown$", out, re.M) is not None, out)
                time.sleep(3)
                width, height, pixels = screen("gtk")
                accent = sum(1 for i in range(0, len(pixels), 3)
                             if abs(pixels[i] - 0x7c) < 16 and abs(pixels[i + 1] - 0x3a) < 16
                             and abs(pixels[i + 2] - 0xed) < 16)
                run_checks(con, GTK_CHECKS)
                check("GTK window drawn in the WormOS colours", accent > 2000, f"{accent} pixels")
                con.run("kill $(pidof gtk-check)")

            # The shell: notifications, the launcher, the panel, the scale.
            put_file(con, NOTIFY.encode(), "/tmp/notify.sh")
            out = session("sh /tmp/notify.sh")
            check("notification accepted", re.search(r"\(uint32 \d+,\)", out) is not None, out)
            time.sleep(3)
            width, height, pixels = screen("notification")
            corner = sum(1 for y in range(0, 220) for x in range(width - 420, width)
                         if near(pixels, width, x, y, (0x8b, 0x5c, 0xf6), 30))
            check("notification shown at the top right", corner > 100, f"{corner} pixels")

            before = screen("before-launcher")[2]
            feet = len(con.run("pidof foot").split())
            session("worm-shell --launcher")
            time.sleep(3)
            changed = region_diff(before, screen("launcher")[2], width, (0, 0, width, height))
            check("launcher covers the screen", changed > width * height // 8 // 2,
                  f"{changed} of {width * height // 4} sampled pixels changed")
            mon.type("foot")
            time.sleep(2)
            mon.key("ret")
            out = wait_for(con, "pidof foot | wc -w", rf"^{feet + 1}$", 60)
            check("launcher starts the program typed", out.strip() == str(feet + 1), out)

            session("worm-shell --quit")
            time.sleep(3)
            gone = screen("no-panel")[2]
            con.run(f"su {USER} -c '/tmp/in-session worm-shell > /dev/null 2>&1 &'")
            time.sleep(8)
            back = screen("panel")[2]
            changed = region_diff(gone, back, width, (0, 0, width, 30))
            check("panel drawn along the top", changed > width // 4, f"{changed} pixels changed")

            vnc.resize(3840, 2160)
            out = wait_for(con, f"su {USER} -c '/tmp/in-session {SCALE}'", r"2\.0+", 90)
            check("automatic scale 2 on a 4K screen", re.search(r"2\.0+", out) is not None, out)
            con.run(f"mkdir -p /home/{USER}/.config/wormos && echo 'scale = 1.25' > "
                    f"/home/{USER}/.config/wormos/display && chown -R {USER} /home/{USER}/.config")
            session("worm-shell --quit")
            con.run(f"su {USER} -c '/tmp/in-session worm-shell > /dev/null 2>&1 &'")
            out = wait_for(con, f"su {USER} -c '/tmp/in-session {SCALE}'", r"1\.25", 90)
            check("chosen scale applied", "1.25" in out, out)
            # Below 100%: more room on big screens (wlroots keeps scales in
            # 256ths: 0.8 is 0.8008).
            con.run(f"echo 'scale = 0.8' > /home/{USER}/.config/wormos/display")
            session("worm-shell --quit")
            con.run(f"su {USER} -c '/tmp/in-session worm-shell > /dev/null 2>&1 &'")
            out = wait_for(con, f"su {USER} -c '/tmp/in-session {SCALE}'", r"0\.80\d*", 90)
            check("a scale below 100% applied", re.search(r"0\.80\d*", out) is not None, out)
            con.run(f"echo 'scale = 1.25' > /home/{USER}/.config/wormos/display")
            vnc.resize(1280, 800)
            time.sleep(5)

            # Sound: the guest's output reaches QEMU's recording, and the
            # volume keys' command changes the volume and the panel's icon.
            run_checks(con, SOUND_CHECKS)
            put_file(con, PLAY.encode(), "/tmp/play.sh")
            before = wav_peak(wav_path)
            out = session("sh /tmp/play.sh")
            time.sleep(2)
            peak = wav_peak(wav_path)
            check("sound played through PipeWire", "played" in out and peak > 1000,
                  f"{out}; loudest sample {before} before, {peak} after")
            out = session(VOLUME)
            m = re.search(r"Volume: ([0-9.]+)", out)
            level = float(m.group(1)) if m else -1
            check("default output has a volume", 0 <= level <= 1.5, out)
            session("worm-shell --volume down")
            session("worm-shell --volume down")
            out = wait_for(con, f"su {USER} -c '/tmp/in-session {VOLUME}'",
                           rf"Volume: {max(level - 0.1, 0):.2f}", 60)
            check("volume down from the shell", f"{max(level - 0.1, 0):.2f}" in out,
                  f"{level:.2f} before; {out}")
            width, height, pixels = screen("volume")
            session("worm-shell --volume mute")
            out = wait_for(con, f"su {USER} -c '/tmp/in-session {VOLUME}'", r"MUTED", 30)
            check("muted from the shell", "MUTED" in out, out)
            time.sleep(3)
            changed = region_diff(pixels, screen("muted")[2], width,
                                  (width - width // 4, 0, width, 40))
            check("panel shows the volume", changed > 5, f"{changed} pixels changed")
            session("worm-shell --volume mute")
            out = wait_for(con, f"su {USER} -c '/tmp/in-session {VOLUME}'", r"^Volume: [0-9.]+$",
                           30)
            check("unmuted from the shell", "MUTED" not in out, out)

            # The Snapshots program: take a snapshot with its button.
            # Through a script, so that a crash shows up in the log.
            put_file(con, b"wormos-snapshots\necho exited with status $?\n", "/tmp/run-snapshots")
            con.run(f"su {USER} -c '/tmp/in-session sh /tmp/run-snapshots > /tmp/snapshots.log 2>&1 &'")
            out = wait_for(con, "pidof wormos-snapshots", r"^\d+$", 60)
            check("Snapshots program started", re.search(r"^\d+$", out, re.M) is not None,
                  con.run("cat /tmp/snapshots.log"))
            time.sleep(6)
            width, height, pixels = screen("snapshots")
            button = solid_block(pixels, width, height, (0x7c, 0x3a, 0xed))
            check("Snapshots shows its button", button is not None)
            if button:
                qmp.click(button[0], button[1], width, height, "left")
                time.sleep(2)
                mon.type("From the desktop test\n")
            out = wait_for(con, "snap list", r"From the desktop test", 90)
            check("snapshot taken from the program", "From the desktop test" in out, out[-300:])
            time.sleep(3)
            screen("snapshots-after")
            # With a GPU compositor GTK reports frame timings it could not
            # match (virgl): noise, not a fault of the program.
            noise = " | grep -v gdk_frame_timings" if args.virgl else ""
            check("no warnings from Snapshots",
                  con.run(f"grep -i -e critical -e warning /tmp/snapshots.log{noise} | grep -c .")
                  == "0",
                  con.run("cat /tmp/snapshots.log"))

            # Log out: the login screen comes back. A wrong password is
            # refused, the right one starts the desktop again.
            # What "labwc --exit" (the shell's Log out) does.
            con.run("kill $(pidof labwc)")
            out = wait_for(con, GREETER_MODE, r"MODE=login", 120)
            check("logging out shows the login screen", "MODE=login" in out,
                  con.run("cat /run/wormlogin/greeter.log"))
            time.sleep(8)
            screen("login")
            mon.type("not the password\n")
            time.sleep(8)
            out = con.run("pidof wormos-greeter; pidof worm-shell || echo no-desktop")
            check("wrong password refused", out.endswith("no-desktop") and out[0].isdigit(), out)
            # A layout of the user's own (as the first-boot setup writes it)
            # must not hide the session's cursor and keyboard defaults.
            con.run(f"mkdir -p ~{USER}/.config/labwc")
            con.run(f"echo XKB_DEFAULT_LAYOUT=us > ~{USER}/.config/labwc/environment")
            con.run(f"chown -R {USER}: ~{USER}/.config")
            mon.type(USER_PASSWORD + "\n")
            out = wait_for(con, "pidof worm-shell", r"^\d+$", 120)
            check("logging in starts the desktop", re.search(r"^\d+$", out, re.M) is not None,
                  con.run("cat /run/wormlogin/greeter.log"))
            time.sleep(10)
            put_file(con, b"tr '\\0' '\\n' < /proc/$(pidof worm-shell)/environ | grep ^X\n",
                     "/tmp/shell-env")
            out = con.run("sh /tmp/shell-env")
            check("own layout keeps the cursor and keyboard defaults",
                  "XCURSOR_THEME=Adwaita" in out and
                  "XKB_DEFAULT_OPTIONS=wormos:caps_unlock_on_press" in out, out)

            run_checks(con, BUDGET_CHECKS)
            used = con.run("free -m | awk '/^Mem:/ { print $3 }'")
            mb = int(used.split()[-1]) if used.split() else 0
            check(f"idle memory with the desktop: {mb} MB (budget {IDLE_RAM_MB})",
                  0 < mb <= IDLE_RAM_MB, used)
            size = con.run("du -smx / | cut -f1")
            mb = int(size.split()[-1]) if size.split() else 0
            check(f"system size: {mb} MB (budget {ROOTFS_MB})", 0 < mb <= ROOTFS_MB, size)

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
