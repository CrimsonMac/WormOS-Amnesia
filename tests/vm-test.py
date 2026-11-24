#!/usr/bin/env python3
"""Virtual machine support (worm vm, the guest agent, shared folders).

Boots out/wormos.img in QEMU with a guest agent channel
(org.qemu.guest_agent.0) on a socket of this machine and a folder of this
machine shared over 9p (mount tag "hostshare"). Checks what `worm vm`
found and wrote to /run/worm/vm, the settings (`worm vm set`, the
per-hypervisor files), the shared folder under /media, and talks to the
guest agent as libvirt does: sync, ping, info, OS, host name, time,
interfaces, users, an unknown command, a refused one, then a clean
shutdown through the agent. The console output goes to build/vm.log.

Usage: tests/vm-test.py [--timeout SECONDS]
"""

import argparse
import importlib.util
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "vm.log")


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tests", file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot_test", "boot-test.py")

CHECKS = [
    ("hypervisor found", "worm vm", r"^Virtual machine: QEMU with KVM|^Virtual machine: QEMU \(emulated"),
    ("written for the desktop", "cat /run/worm/vm",
     r"(?s)^hypervisor=qemu$.*^agent-port=yes$.*^shares=hostshare$.*^renderer=software$"),
    ("SPICE channel seen", "grep -x spice=yes /run/worm/vm", r"^spice=yes$"),
    ("readable by users", "stat -c %a /run/worm/vm", r"^644$"),
    ("base settings for each hypervisor", "ls /usr/share/worm/vm | tr '\\n' ' '",
     r"hyperv\.conf.*qemu\.conf.*virtualbox\.conf"),
    ("virtual disk without I/O scheduler", "cat /sys/block/vda/queue/scheduler", r"\[none\]"),
    ("shared folder mounted", "cat /media/hostshare/hello.txt", r"^hello from the host$"),
    ("shared folder is 9p", "grep ' /media/hostshare ' /proc/mounts", r"^hostshare /media/hostshare 9p"),
    ("guest agent running", "worm vm | grep 'Guest agent'", r"running"),
    ("a setting changes", "worm vm set cursor software && grep -x cursor=software /run/worm/vm",
     r"(?m)^cursor=software$"),
    ("kept in vm.conf", "cat /etc/worm/vm.conf", r"(?m)^cursor = software$"),
    ("root stays read-only", "grep ' / ' /proc/mounts", r"\sro[,\s]"),
    ("bad values refused", "worm vm set cursor sideways; echo status=$?", r"status=2"),
    ("back to the default", "worm vm reset cursor && grep -c '^cursor' /etc/worm/vm.conf",
     r"^0$"),
    ("agent can be turned off", "worm vm set agent off >/dev/null; worm vm | grep -c 'Guest agent:     off'",
     r"^1$"),
    ("agent back on", "worm vm set agent on >/dev/null; sleep 12; worm vm | grep 'Guest agent'",
     r"running"),
    ("users can look", "su -s /bin/sh nobody -c 'worm vm' | head -n 1", r"Virtual machine: QEMU"),
    ("users cannot change", "su -s /bin/sh nobody -c 'worm vm set agent off'; echo status=$?",
     r"needs root(.|\n)*status=1"),
    ("host name changes", "worm hostname testbox && hostname && cat /etc/hostname",
     r"^Host name: testbox\ntestbox\ntestbox$"),
]


class Agent:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.sock.settimeout(20)
        self.buf = b""

    def call(self, execute, arguments=None, raw=None):
        msg = raw if raw is not None else json.dumps(
            {"execute": execute, **({"arguments": arguments} if arguments else {})})
        self.sock.sendall(msg.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("the agent closed the channel")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.lstrip(b"\xff"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="wormos-vm-test-")
    share = os.path.join(tmp, "share")
    os.mkdir(share)
    with open(os.path.join(share, "hello.txt"), "w") as f:
        f.write("hello from the host\n")
    qga = os.path.join(tmp, "qga.sock")
    extra = (f"-chardev socket,path={qga},server=on,wait=off,id=qga0 "
             f"-device virtserialport,bus=vserial0.0,chardev=qga0,name=org.qemu.guest_agent.0 "
             f"-virtfs local,path={share},mount_tag=hostshare,security_model=none,id=fs0")
    env = dict(os.environ, QEMU_EXTRA=extra)
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    passed = failures = 0

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += bool(ok)
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", args.timeout)
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")
            for name, cmd, pattern in CHECKS:
                out = con.run(cmd, 90)
                check(name, re.search(pattern, out, re.MULTILINE) is not None, out)

            agent = Agent(qga)
            r = agent.call(None, raw='{"execute": "guest-sync-delimited", "arguments": {"id": 4711}}')
            check("agent: sync", r.get("return") == 4711, r)
            r = agent.call("guest-ping")
            check("agent: ping", r.get("return") == {}, r)
            r = agent.call(None, raw='{"execute": "guest-ping", "id": "abc"}')
            check("agent: request id echoed", r.get("id") == "abc", r)
            r = agent.call("guest-info")
            names = {c["name"] for c in r.get("return", {}).get("supported_commands", [])}
            check("agent: commands listed", {"guest-shutdown", "guest-network-get-interfaces"} <= names
                  and "guest-exec" not in names, r)
            r = agent.call("guest-get-osinfo")
            check("agent: OS", r.get("return", {}).get("name") == "WormOS" and
                  "wormos" in r["return"].get("kernel-release", ""), r)
            r = agent.call("guest-get-hostname")
            check("agent: host name", r.get("return", {}).get("host-name") == "testbox", r)
            r = agent.call("guest-get-time")
            check("agent: time", abs(r.get("return", 0) / 1e9 - time.time()) < 120, r)
            r = agent.call("guest-get-timezone")
            check("agent: time zone", "offset" in r.get("return", {}), r)
            r = agent.call("guest-network-get-interfaces")
            addrs = [a["ip-address"] for i in r.get("return", []) for a in i.get("ip-addresses", [])]
            check("agent: addresses", "10.0.2.15" in addrs, r)
            r = agent.call("guest-get-vcpus")
            check("agent: processors", len(r.get("return", [])) >= 1, r)
            r = agent.call("guest-set-time", {"time": int(time.time() * 1e9)})
            check("agent: set the clock", r.get("return") == {}, r)
            r = agent.call("guest-exec", {"path": "/bin/sh"})
            check("agent: running programs refused",
                  r.get("error", {}).get("class") == "CommandNotFound", r)
            agent.sock.sendall(b'{"execute": ')
            time.sleep(0.5)
            r = agent.call(None, raw='"guest-ping"}')
            check("agent: a command in two pieces", r.get("return") == {}, r)
            r = agent.call(None, raw='{"execute": "guest-ping", "arguments": [1, {"a": "}"}]}')
            check("agent: odd input answered", "error" in r or r.get("return") == {}, r)
            agent.sock.sendall(b'{"execute": "guest-shutdown"}\n')
            try:
                proc.wait(120)
                check("agent: clean shutdown", True)
            except subprocess.TimeoutExpired:
                check("agent: clean shutdown", False, "QEMU still running")
        except (TimeoutError, RuntimeError, EOFError, OSError, ValueError) as e:
            check("test ran to the end", False, str(e))
        finally:
            if proc.poll() is None:
                proc.kill()
    subprocess.run(["rm", "-rf", tmp])
    print(f"\n{passed}/{passed + failures} checks passed (console in {LOG})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
