# Building and testing WormOS

How to build WormOS from source, run it, test it, and what CI checks. For
how the pieces fit together inside, see [internals.md](internals.md).

WormOS is built entirely from source with its own cross toolchain
(`x86_64-wormos-linux-gnu`). Development targets x86_64 and is tested in
QEMU with UEFI firmware.

## Requirements

A Debian 13 (or similar) host with roughly 35 GB of free disk space:

```
sudo apt install build-essential bison flex gawk texinfo m4 help2man \
    libncurses-dev libssl-dev libelf-dev libgmp-dev libmpc-dev rsync curl xz-utils bzip2 \
    btrfs-progs dosfstools mtools fakeroot grub-efi-amd64-bin grub-pc-bin bc \
    grub-efi-amd64-signed sbsigntool xorriso python3-virt-firmware \
    qemu-system-x86 qemu-utils ovmf zip unzip \
    ninja-build gperf pipx python3-mako python3-yaml python3-ply python3-packaging python3-cairo \
    cmake nasm sassc gettext \
    libexpat1-dev llvm-19-dev libclang-19-dev libclang-cpp19-dev \
    libllvmspirvlib-19-dev spirv-tools libclc-19-dev
pipx install meson==1.12.1
```

Python 3.13 (Debian 13's `python3`) must be on the build machine: a cross
build of Python runs a Python of the same minor version.

Meson comes from pipx because some packages need a newer one than Debian
13 ships (the build puts `~/.local/bin` first in `PATH`). LLVM is only used
on the build machine, to build Mesa's OpenCL compiler (`mesa_clc`) for its
Intel drivers; nothing from LLVM goes into WormOS.

## Build and run

```
make            # toolchain, kernel, packages, repository, root filesystem and disk image
make run        # boot the image in QEMU on the serial console (Ctrl-A X quits)
make run-gui    # boot the image in a QEMU window
make test       # boot the image and run automated checks
```

`make test-wormholes` checks the Debian and Kali wormholes against the real
archives; it needs internet access and takes several minutes.
`tests/wormhole-test.py --apps` also installs real programs across the
toolkits (Ghidra and Burp Suite from Kali, a GTK, a Qt and a Tk program
from Debian; several GB of downloads) and checks that each opens a window.

`make test-apps` (`tests/apps-test.py`) checks programs the way a user
meets them: it answers the first-boot setup, so the desktop runs with the
session's real environment (the old checks set `GDK_BACKEND=x11` and
`QT_QPA_PLATFORM=xcb` themselves, which hid that every Qt program from
Debian quit on the desktop), installs every tool kit and 21 graphical
programs (GTK 3 and 4, Qt 5 and 6, Java, Firefox and Chromium, X11) on a
60 GiB copy of the image, then starts every menu entry with `gio launch`
from the shell's environment and waits for its window (listed by
`tests/toplevels.c` over wlr-foreign-toplevel, built with the WormOS
toolchain), runs every terminal entry's command and every exported command
with `--help`, looking for signs of a broken program (missing libraries,
modules, scripts or toolkit plugins), and checks the runtime: rules,
stand-ins and the bridge, generated entries, Kali's helper scripts.
`KITS=web,passwords` limits the kits; it takes over an hour with all of
them. Every program's result is in `build/apps-report.txt`.

`make test-upgrade FROM=wormos-1.8-x86_64.qcow2` sets up an older release
image and upgrades it to the packages just built.

`make test-delta` checks how wh upgrades (deltas, unchanged files left in
place, the download cache emptied): it makes a second repository from
`out/repo` with three packages changed, deltas for them with
`scripts/make-deltas.sh`, and upgrades the development image to it, with
one file of a package changed first so that its delta cannot be used.

`make test-secureboot` starts the development image with OVMF in Secure
Boot mode and Microsoft's keys (`OVMF_CODE_4M.secboot.fd`,
`OVMF_VARS_4M.ms.fd`, as PCs ship): without WormOS's certificate nothing
past shim may run; with it in MokList (written by `virt-fw-vars
--add-mok`, as MokManager does) the image boots and the kernel reports
Secure Boot and lockdown; then the ISO boots to the live system the same
way. `run-qemu.sh` takes `SECURE_BOOT_VARS=FILE` for this.

`make test-vm` checks the virtual machine support on the development
image: QEMU gets a guest agent channel on a socket of this machine and a
folder shared over 9p; the test checks `worm vm` and `/run/worm/vm`, the
shared folder under `/media`, `worm vm set`/`reset`, then talks to the
guest agent as libvirt does (sync, info, OS, host name, time, addresses,
a refused `guest-exec`, a command sent in two pieces) and shuts the VM
down through it.

`make test` also runs `tests/desktop-test.py`: the VM gets a virtio GPU,
keyboard and tablet, and the test types on the graphical console through
the QEMU monitor like a user would. It answers the first-boot setup, logs
in (which starts the desktop), types into the terminal, opens another with
Super+Enter, checks a screendump of the real screen, and compares memory
use and system size with their budgets. `tests/desktop-test.py --release
out/release/wormos-1.10-x86_64.qcow2` runs the same checks on a release
image, where root can only log in once the first-boot setup is done.
`tests/desktop-test.py --virgl` runs them on a virtio GPU with 3D (virgl,
as virt-manager with OpenGL), rendered on this machine's GPU through
`-display egl-headless`; screenshots then come through VNC, since QEMU's
screendump cannot read a GL screen.

`make test` boots a throwaway copy of the image, checks the running system
and exercises snapshots end to end (take, diff, restore, browse, rewind of
the system and of `/home`, and a rewind from the boot menu), then
`worm unlock`/`lock` and the package manager against `out/repo`, served to
the VM over HTTP (install, a trial that rolls back, remove and undo). It
also opens and closes a firewall port and checks it from the host through a
QEMU port forward (`QEMU_HOSTFWD`), tries to escape from a blackhole (as
root and as a normal user), switches anonymous mode on and off (checking
that nothing reaches the host directly), runs `worm panic` and boots safe
mode from the menu. It ends with the ACPI power button (a clean power-off)
and a second boot with BIOS firmware. `make test-wormholes` also checks
that Tor connects and traffic really goes through it.

`make iso` builds the live and installation ISO (`out/wormos.iso`) from
the same root filesystem, and `make test-iso` boots it in QEMU with an
empty disk: it checks the live system (running from memory, snapshots,
firewall), installs WormOS onto the disk with the installer's backend,
then starts the installed disk with UEFI and with BIOS firmware, and the
ISO with BIOS firmware.

`make iso-barebones` assembles the root filesystem again as Barebones and
builds its ISO (`out/wormos-barebones.iso`); `tests/editions-test.py
barebones` boots it and checks what makes the edition, and
`tests/iso-test.py out/wormos-barebones.iso` runs the installation checks
on it too. `make release VERSION=X` builds both editions.

## Continuous integration

GitHub Actions runs, on every push and pull request (`.github/workflows/ci.yml`):

- `scripts/lint.sh`: every recipe read as `tools/wbuild` reads it (name,
  version, a checksum per source, dependencies that exist), shell and
  Python syntax, ShellCheck at warning level (settings and the reasons
  for what it skips in `.shellcheckrc`; used locally when installed),
  links between the documents, and no private keys, large files or
  build-machine paths in the repository;
- actionlint on the workflows themselves;
- `scripts/ci-build.sh`: WormOS's own programs built with GCC and with
  Clang, warnings as errors, and the unit tests (`tests/unit`: the parser
  for Tor's clock-skew reports, SHA-256 against the FIPS 180-4 examples,
  and the signature checks behind wh's index and Amnesia's updates); a
  third job runs the unit tests under AddressSanitizer and
  UndefinedBehaviorSanitizer (`SANITIZE=1`). Neither script needs the
  toolchain: run them before committing.

`codeql.yml` runs CodeQL's security queries over the C programs and the
Python test harness on pushes, pull requests and every week (results under
Security, Code scanning; `CI_IN_PLACE=1` builds in the checkout so the
results point at the real files).

A full build takes hours, so images are not built there. Instead, on every
release and every week, `release-tests.yml` boots the published images in
QEMU with KVM and runs the release tests (desktop, live ISOs, an
encrypted install from the ISO, Secure Boot with Microsoft's keys and
WormOS's certificate enrolled, Barebones),
and takes the release before it and upgrades it from the published
package repository (`tests/upgrade-test.py --published`), as its users
do. `package-repo.yml` checks the published package repository every day
(`scripts/check-package-repo.sh`: signed index, every package present with
its size and SHA-256, dependencies, source archives); and `updates.yml`
lists recipes with newer upstream releases each week
(`scripts/check-updates.py`, from Repology) in an issue titled "Outdated
packages".

Every test job writes its checks to the run's summary page
(`scripts/ci-summary.py`: the totals, each failed check with its output,
the passed ones folded away), and failed image tests keep their logs and
screenshots as artifacts. Actions are pinned to commits, checkouts keep no
credentials, and every workflow starts with read-only permissions;
Dependabot keeps the pinned actions current.

On a Windows PC, from WSL: `make test-qemu-windows` boots the image in
QEMU for Windows, which it expects under `D:\qemu` (see
`docs/internals.md`, Tests).

Building the disk image uses `sudo` for one step (applying file ownership
before `mkfs.btrfs`); everything else runs as a normal user.

The first build compiles the toolchain and takes a while. Later builds reuse
it, and rebuild only the packages whose recipe, sources or dependencies
changed (`WBUILD_FORCE=1` rebuilds everything). Individual steps can be run
with `make toolchain`, `make kernel`, `make packages`, `make repo`,
`make rootfs` and `make image`.

`make repo` packs every package into `out/repo` and signs the index. The
first run creates a signing key in `~/.config/wormos/core.key` (keep it
safe and backed up); its public half is `keys/core.pub`, which is built
into the system. The root filesystem is assembled by installing the base
packages from that repository with `wh --root`.

The kernel, GRUB for UEFI and the boot images are signed for Secure Boot
with `~/.config/wormos/secureboot.key` (`WORMOS_SB_KEY`), which belongs to
`keys/secureboot.crt`. Without it the build makes a development key in
`build/host/secureboot/` and puts its certificate on the EFI system
partition, so your images boot with Secure Boot once that certificate is
enrolled (docs/internals.md, Secure Boot).

The development image has an empty root password (the tests log in with
it). `make release VERSION=1.5` builds the release images instead
(`scripts/release.sh`): root stays locked until the first-boot setup, the
disk is 20 GiB, and the output is the ISO and a qcow2 image in
`out/release`. `make publish-repo` writes the source archives
(`scripts/source-archives.sh`, into `out/sources`) and uploads them with
the repository to the package release (`scripts/publish-repo.sh`, needs
the GitHub CLI). Before uploading, `scripts/make-deltas.sh` downloads the
published version of every package that changed and makes a delta
against it with `tools/wdelta` (the files that differ), lists it in the
index and signs the index again.

The root filesystem is read-only; change it with `worm unlock` and
`worm lock`. (A plain `mount -o remount,ro /` would make the whole btrfs
filesystem read-only, including `/var` and `/home`.)

## Layout

| Path | Contents |
| --- | --- |
| `scripts/` | Build scripts (toolchain, kernel, root filesystem, disk image, ISO, QEMU) |
| `tools/wbuild` | Package builder |
| `tools/wpack`, `tools/wrepo` | Package files and the signed repository |
| `keys/` | Public key of the package repository |
| `recipes/` | Package recipes |
| `src/` | WormOS components: `worminit` (init), `wormrd` (initramfs init), `snap`, `wh`, `worm`, `bh`, the desktop shell, login, snapshot app and installer |
| `tests/` | Automated tests in QEMU |
| `kernel/` | Kernel configuration |
| `toolchain/` | Pinned toolchain sources and checksums |

## Package recipes

Each package is described by `recipes/NAME/recipe`, a short bash script
(see the comment at the top of `tools/wbuild` for every field, including
`depends`, `caps` for file capabilities and `groups`):

```bash
name=example
version=1.0
description="An example package"
source=(https://example.org/example-$version.tar.xz)
sha256=(...)

build() {
	./configure --prefix=/usr --host="$TARGET"
	make -j"$JOBS"
}

package() {
	make DESTDIR="$pkgdir" install
}
```

Build it with `tools/wbuild example`. Every source is verified against its
pinned SHA-256 checksum before use.
