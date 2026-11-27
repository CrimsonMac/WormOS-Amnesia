#!/usr/bin/env python3
"""Boot WormOS with Secure Boot on (OVMF with Microsoft's keys, as PCs ship).

1. Without WormOS's certificate: shim (signed by Microsoft) starts, cannot
   verify GRUB and hands over to MokManager, so nothing of WormOS runs.
2. With the certificate enrolled (written into MokList as MokManager does;
   MokManager itself needs a person at the screen): the disk
   image boots, the kernel reports Secure Boot and lockdown.
3. The ISO boots to the live system the same way.

The certificate is the one on the image's EFI system partition (the release
key, or the development key of a build without it).

With --release (release images: no login on the serial console) the disk
image is only checked to start GRUB and pass the kernel's signature check; the ISO's live
system, where root logs in on the serial console, gets the kernel checks.

Usage: tests/secureboot-test.py [--image IMAGE] [--iso ISO] [--release] [--timeout SECONDS]
"""

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("boot_test", os.path.join(ROOT, "tests", "boot-test.py"))
boot_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_test)
Console = boot_test.Console

LOG = os.path.join(ROOT, "build", "secureboot.log")
MS_VARS = "/usr/share/OVMF/OVMF_VARS_4M.ms.fd"
# Any owner GUID will do for the MokList entry.
OWNER = "a6dc6b8b-5d1e-4e2b-9b2c-576f726d4f53"

CHECKS = [
    ("kernel saw it", "dmesg | grep -i -m1 'secure boot'", r"(?i)secure boot enabled"),
    ("lockdown", "cat /sys/kernel/security/lockdown", r"\[(integrity|confidentiality)\]"),
]

results = []


def check(name, ok, out=""):
    results.append(ok)
    print(("PASS  " if ok else "FAIL  ") + name)
    if not ok and out:
        print("      output: " + repr(out[-400:]))


def qemu(env_extra, log):
    env = dict(os.environ, **env_extra)
    return subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default=os.path.join(ROOT, "out", "wormos.img"))
    parser.add_argument("--iso", default=os.path.join(ROOT, "out", "wormos.iso"))
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    tmp = tempfile.mkdtemp()
    cert = os.path.join(tmp, "wormos.cer")
    image = args.image
    if image.endswith(".qcow2"):
        image = os.path.join(tmp, "disk.raw")
        subprocess.run(["qemu-img", "convert", "-O", "raw", args.image, image], check=True)
    # The ESP starts at 1 MiB.
    subprocess.run(["mcopy", "-i", image + "@@1M", "::/EFI/BOOT/wormos.cer", cert], check=True)
    plain = os.path.join(tmp, "ms.fd")
    trusted = os.path.join(tmp, "trusted.fd")
    shutil.copy(MS_VARS, plain)
    pem = os.path.join(tmp, "wormos.pem")
    subprocess.run(["openssl", "x509", "-inform", "DER", "-in", cert, "-out", pem], check=True)
    # In MokList, where MokManager puts an enrolled certificate.
    subprocess.run(["virt-fw-vars", "--input", MS_VARS, "--output", trusted, "--secure-boot",
                    "--add-mok", OWNER, pem], check=True, stdout=subprocess.DEVNULL)

    with open(LOG, "wb") as log:
        # 1. Not trusted: GRUB never runs.
        vars1 = os.path.join(tmp, "v1.fd")
        shutil.copy(plain, vars1)
        proc = qemu({"IMAGE": args.image, "SECURE_BOOT_VARS": vars1}, log)
        con = Console(proc, log)
        try:
            # shim hands over to MokManager instead of GRUB.
            out = con.expect("GNU GRUB", 60)
            check("refused without the certificate", False, out)
        except Exception:
            check("refused without the certificate", True)
        finally:
            proc.kill()
            proc.wait()

        # 2. Trusted: the disk image boots.
        vars2 = os.path.join(tmp, "v2.fd")
        shutil.copy(trusted, vars2)
        proc = qemu({"IMAGE": args.image, "SECURE_BOOT_VARS": vars2}, log)
        con = Console(proc, log)
        try:
            if args.release:
                # No login on the serial console: GRUB must start and the
                # kernel pass shim's check (no "bad shim signature").
                con.expect("GNU GRUB", 60)
                end = time.time() + 30
                while time.time() < end:
                    con._read(1)
                check("disk image boots", b"bad shim signature" not in con.buf, con.buf.decode(errors="replace"))
                raise StopIteration
            con.expect("login:", args.timeout)
            check("disk image boots", True)
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")
            for name, cmd, pattern in CHECKS:
                out = con.run(cmd)
                check(name, re.search(pattern, out, re.MULTILINE) is not None, out)
        except StopIteration:
            pass
        except Exception as e:
            check("disk image boots", False, str(e))
        finally:
            proc.kill()
            proc.wait()

        # 3. The ISO.
        if os.path.exists(args.iso):
            vars3 = os.path.join(tmp, "v3.fd")
            shutil.copy(trusted, vars3)
            disk = os.path.join(tmp, "disk.img")
            subprocess.run(["truncate", "-s", "1G", disk], check=True)
            # The live system runs from memory.
            proc = qemu({"IMAGE": disk, "ISO": args.iso, "SECURE_BOOT_VARS": vars3,
                         "QEMU_MEM": "3072"}, log)
            con = Console(proc, log)
            try:
                con.expect("login:", args.timeout)
                con.send("root\n")
                con.expect("# ", 60)
                con.send("exec /bin/sh\n")
                out = con.run("cat /proc/cmdline")
                check("ISO boots with Secure Boot", "wormos.live" in out, out)
                for name, cmd, pattern in CHECKS:
                    out = con.run(cmd)
                    check(name + " (ISO)", re.search(pattern, out, re.MULTILINE) is not None, out)
            except Exception as e:
                check("ISO boots with Secure Boot", False, str(e))
            finally:
                proc.kill()
                proc.wait()

    shutil.rmtree(tmp)
    print(f"{sum(results)}/{len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
