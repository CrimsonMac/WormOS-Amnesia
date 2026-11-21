# WormOS Amnesia

A live edition of WormOS for one-off tasks: it runs from memory only,
sends every connection through Tor from the moment it starts, and forgets
everything when it stops. Nothing can make it keep anything: there is no
installer, root cannot log in, and no disk is ever touched.

Before the desktop, a start screen asks how to reach Tor (directly, or
through snowflake, obfs4, meek or pasted bridges; Tor waits for the
answer, so a network that blocks Tor never sees a direct attempt).
There is no administrator at all: nothing can change the system or reach
a disk, yet programs can still be installed into memory (`wh install`,
`worm kit install`). `amnesia-update` brings the USB stick up to the latest
release, downloading only what changed. At shutdown the session is
deleted and its memory wiped as it is freed; pulling out the USB stick it
started from shuts it down the same way.

This repository is WormOS with Amnesia on top: the `wormos-amnesia`
package (`recipes/wormos-amnesia`), its plan ([docs/amnesia.md](docs/amnesia.md))
and its tests. Everything below the line is WormOS's own README, which
applies here too.

```
make iso-amnesia                       # out/wormos-amnesia.iso
tests/editions-test.py amnesia         # boot it and check what Amnesia promises
make release-amnesia VERSION=1.9       # the release ISO and its signed update manifest
scripts/publish-amnesia.sh             # publish them for amnesia-update
tests/amnesia-update-test.py OLD.iso NEW.iso   # check the updater
```

---

# WormOS

A lightweight, security-focused Linux operating system built around instant, tamper-resistant snapshots.

> **Status:** 1.7: a read-only system with snapshots, packages, a
> firewall, sandboxes, Tor and Tor Browser, tool kits and a light Wayland
> desktop with its own panel, login screen, snapshot timeline and sound.
> It runs live from an ISO and installs onto a disk, or as a ready-made
> disk image in QEMU and virt-manager. Three editions: the standard one,
> Barebones (the operating system alone) and Amnesia (a live system that
> forgets everything and sends all traffic through Tor; a preview).

## Philosophy

- **Testing:** a safe place to try software, write code and experiment. If something breaks, go back.
- **Secure:** hardened defaults, a firewall enabled out of the box, and privacy tools built in.
- **Reversible:** the base system is immutable, and every state can be snapshotted and restored in seconds.
- **Isolated:** risky work runs in disposable sandboxes that cannot reach the host or its snapshots.

## Features

- `snap`, a simple snapshot tool, plus a graphical timeline:
  ```
  snap now                       # snapshot the system now
  snap now -c "Installed nginx"  # snapshot with a comment
  snap list                      # list snapshots
  snap rewind                    # return to the last snapshot
  ```
- Separate system and home snapshots, built on Btrfs, with fast rewinds.
- Snapshots protected from the running system, verified at boot, and restorable from the boot menu.
- Blackholes: disposable sandboxes for untrusted software and malware analysis (`bh` / `blackhole`), with several sessions each, an optional network, and a virtual machine mode with its own kernel.
- An nftables firewall that denies inbound traffic by default (`worm fw`).
- Tor built in, Tor Browser with signature-verified installation, and an optional system-wide anonymous mode.
- Hardened defaults: MAC address randomization, encrypted DNS, a hardened kernel and no telemetry (full-disk encryption in the installer is planned).
- Tool kits (forensics, network analysis, blue team, reverse engineering, web testing, passwords, penetration testing), installed on demand into containers (`worm kit`).
- An independent init system and package manager (`wh` / `wormhole`), with
  automatic snapshots before every change (`snap auto on|off`).
- A lightweight Wayland desktop.
- A live ISO that runs from memory and installs WormOS onto a disk.
- Editions: WormOS (everything above, with everyday tools and Tor
  Browser), WormOS Barebones (the operating system alone) and WormOS
  Amnesia (live only, forgets everything, Tor from the first packet).

## Trying it

Download a release from the release page and check it against
`SHA256SUMS`. All of them start with UEFI or BIOS firmware (Secure Boot
off).

### Editions

| | WormOS | WormOS Barebones | WormOS Amnesia |
|---|---|---|---|
| File | `wormos-1.7-x86_64.iso`, `.qcow2` | `wormos-1.7-barebones-x86_64.iso` | `wormos-1.7-amnesia-x86_64.iso` |
| What | the full system | the operating system alone: desktop, network, firewall, snapshots, packages, sandboxes | the Barebones system with Tor and Tor Browser, live only |
| Tor, Tor Browser | yes (anonymous mode off until you turn it on) | no (`wh install tor tor-browser`) | always on, everything goes through Tor |
| Everyday tools | yes (below) | no (`worm kit install basic`) | no |
| Installs onto a disk | yes | yes | never |
| Keeps your changes | yes | yes | never: everything is gone at shutdown |
| Look | violet | violet | grey, panel at the bottom |

WormOS Amnesia is meant for one-off tasks, not for work: see
[docs/amnesia.md](docs/amnesia.md) for what it promises, how it works
and what comes next. Root cannot log in on it, so nothing can make it keep
anything or reach a disk.

- **The ISO** (`wormos-1.7-x86_64.iso`): a live system and an installer.
  Boot a virtual machine from it (as a CD/DVD), or write it to a USB stick
  as it is (`dd`, or a tool such as Ventoy or balenaEtcher) and start a PC
  from the stick. WormOS copies itself into memory and starts the desktop
  as the user "live", without a password; nothing is written to any disk.
  Give it at least 2 GB of memory (4 GB is comfortable): everything you
  change lives in memory too, until you restart.

  The installer opens by itself ("Install WormOS" in the launcher). Choose
  a disk of at least 8 GB: it is **erased** and gets WormOS, which then
  fills it. Restart without the ISO; WormOS asks for your name and
  passwords, as in the disk image below. The live system's snapshots and
  files are not carried over.

- **The disk image for QEMU / virt-manager** (`wormos-1.7-x86_64.qcow2`): an
  installed system, ready to start. In virt-manager,
  import an existing disk image and choose a generic recent Linux. From
  the command line:
  ```
  qemu-system-x86_64 -machine q35 -accel kvm -cpu host -m 2048 -smp 2 \
      -drive file=wormos-1.7-x86_64.qcow2,if=virtio \
      -nic user,model=virtio-net-pci -device virtio-vga \
      -device virtio-keyboard-pci -device virtio-tablet-pci \
      -device virtio-serial-pci \
      -chardev qemu-vdagent,id=vdagent,name=vdagent,clipboard=on,mouse=off \
      -device virtserialport,chardev=vdagent,name=com.redhat.spice.0
  ```
  The last three lines share the clipboard with QEMU's window (when QEMU
  is built with clipboard support) and with VNC viewers. virt-manager's
  SPICE display shares it without them. For UEFI firmware, add
  `-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd`.
- **QEMU on Windows:** install QEMU from https://qemu.weilnetz.de/w64/
  (it includes the UEFI firmware). For speed, turn on the Windows feature
  "Windows Hypervisor Platform" (Settings, Optional features, More Windows
  features) and restart. Then, in PowerShell, in the folder with the
  image:
  ```
  & "C:\Program Files\qemu\qemu-system-x86_64.exe" -machine q35 `
      -accel whpx,kernel-irqchip=off -m 2048 -smp 2 `
      -drive file=wormos-1.7-x86_64.qcow2,if=virtio `
      -nic user,model=virtio-net-pci -device virtio-vga -display sdl `
      -device virtio-keyboard-pci -device virtio-tablet-pci
  ```
  If QEMU says WHPX is not available, use `-accel tcg` instead: it works
  without the Windows feature, but much more slowly. Ctrl+Alt+G releases
  the mouse.
- **VMware is not supported for now.** It cannot be tested at the moment,
  so there is no VMware bundle and the VMware guest tools are left out.
  The ISO may still work in VMware, but nothing is promised; QEMU,
  virt-manager (KVM) and real PCs are the supported targets.

On the first start of an installed system (from the ISO or the disk
image), WormOS asks for your name, a user name, a password for it and a
password for root. Then the desktop starts; later, log in on the login
screen.

## The desktop

The desktop is [labwc](https://labwc.github.io), a light stacking Wayland
compositor, with the [foot](https://codeberg.org/dnkl/foot) terminal. It
idles at well under 100 MB of memory.

- Right click on the background (or Super+Space) opens the menu: terminals,
  applications installed from wormholes, snapshots, screenshots, log out.
- Super+Enter opens a terminal, Super+Q closes a window, Alt+Tab switches,
  Super+Arrow keys maximize or snap windows, Print saves a screenshot in
  `~/Pictures`.
- Sound goes through [PipeWire](https://pipewire.org), which also serves
  programs written for PulseAudio or ALSA (including those installed from
  Debian or Kali). The speaker button in the panel sets the volume (scroll
  on it, or use the volume keys); `wpctl status` lists devices and
  `wpctl set-default ID` picks the output.
- Ctrl+Alt+F2 switches to a second console (for logging in as root);
  Ctrl+Alt+F1 returns to the desktop.
- Administration is done as root: `su -` in a terminal, or the menu's root
  terminal.
- `passwd` (or `worm passwd`) changes your own password, although `/etc`
  is read-only: it asks for the current one first.
- Time: the first-boot setup asks for your time zone; `worm time` shows the
  time, the zone and the last clock check, `worm time zone Europe/Berlin`
  changes the zone (as root; `worm time zones` lists them). The clock is
  checked over HTTPS every six hours (the `Date` of an authenticated
  answer, which unlike plain NTP cannot be forged on the way, and which
  also works through Tor).
- Copy and paste between WormOS and the machine it runs on works with
  virt-manager (SPICE) and QEMU (see above). In virt-manager the screen
  follows the window when View, Scale Display, "Auto resize VM with
  window" is on (the default), from the login screen on. In a terminal, `clip copy`
  copies its arguments or standard input and `clip paste` prints the
  clipboard (`ls | clip copy`).
- Settings: `/etc/xdg/labwc` (copy files to `~/.config/labwc` to change
  them; `environment` sets the keyboard layout) and `/etc/xdg/foot/foot.ini`.
- Screen resolution: `wlr-randr` lists outputs and their modes, and
  `wlr-randr --output NAME --custom-mode 1920x1080` changes one. To keep a
  setting, add the command to `~/.config/labwc/autostart`.

### The terminal

Terminals run [zsh](https://www.zsh.org) with a configuration in
`/etc/zsh/zshrc` (your own goes in `~/.zshrc`):

- As you type, the rest of an earlier command appears in grey
  ([zsh-autosuggestions](https://github.com/zsh-users/zsh-autosuggestions)):
  Right or End takes it, Ctrl+Right takes one word. Commands are coloured
  as you type: green when they exist, red when they do not.
- History: 50,000 commands in `~/.zsh_history`, shared between open
  terminals as you go. Up and Down go through earlier commands that start
  with what is typed; Ctrl+R searches. Commands starting with a space are
  not kept.
- Tab completes with a menu (arrows or Tab to move), ignoring case, also
  for `snap`, `wh`, `bh`, `worm` and `clip` (snapshot numbers, packages,
  blackholes).
- Ctrl+Backspace and Ctrl+Delete delete a word, Ctrl+Left and Ctrl+Right
  move by one. Words stop at `/`, so Ctrl+Backspace removes one part of a
  path.
- The prompt shows the newest snapshot and anything waiting for a
  restart, `[snap 14] tester@wormos:~$`; root's ends in `#` and says
  `[unlocked]` while the system is writable. Inside a blackhole it starts
  with `[bh:NAME]`, inside a git repository the branch follows the folder
  (`~/src/app (main)$`). A failed command's status and the time of
  commands that ran for 5 seconds or more appear on the right.
- A missing command suggests `wh search`.

Root gets the same shell everywhere: the menu's root terminal, `su`,
`su -` and logins on the consoles. Systems upgraded from 1.5 or older,
where root still had the plain `sh`, move to zsh with the upgrade.

Graphics: Intel GPUs (with the i915/xe firmware) use OpenGL ES through Mesa; other virtual GPUs (virtio, QXL, bochs) use a
fast software renderer, and so does any GPU on which OpenGL ES fails to
start.
Programs installed from Debian or Kali that support Wayland open on the
desktop as well (they see the system's fonts); X11-only programs do not
work yet.

## Snapshots

`snap` manages snapshots of the system (`/`) and of `/home`. The default
scope is the system, so rewinding never touches your files unless asked to.

```
snap now -c "Before trying nginx"   # take a snapshot with a comment
snap list                           # ID, date, scope, type, comment
snap diff 3                         # what changed since snapshot 3
snap restore 3 /etc/nginx           # bring back one file or folder
snap browse 3                       # read-only shell inside snapshot 3
snap rewind                         # go back to the latest snapshot
snap rewind 3 --home                # rewind /home only (--all for both)
snap verify                         # check snapshots for tampering
snap delete 3 / snap prune --keep 10 / snap pin 3 / snap comment 3 "..."
snap auto on|off                    # snapshots before package changes
```

How it works:

- The disk is a single btrfs filesystem with the subvolumes `@` (the system,
  mounted read-only), `@var` (logs and state), `@home` and `@snapshots`.
- Snapshots are read-only btrfs snapshots stored in `@snapshots`, which the
  running system never mounts. `snap` mounts it in a private mount namespace
  only for the duration of a command.
- Every snapshot has a manifest with the checksum, owner, mode and extended
  attributes of each file. Rewinds verify the target first and refuse a
  snapshot that was modified.
- A rewind is applied at a soft reboot: the init system stops userland, the
  initramfs init (`wormrd`) swaps the subvolume and the system starts again
  without restarting the kernel. The state being left is snapshotted first,
  so a rewind can itself be undone.
- The boot menu has a "Rewind to a snapshot" submenu with the latest system
  snapshots, which rewinds before anything from the installed system runs.

These protections raise the bar, but software running as root on the same
machine can always write to the disk directly. Untrusted code belongs in an
isolated sandbox: a blackhole (see below).

## Packages

`wh` (short for wormhole) installs software from the signed WormOS
repository. Changes never touch the running system: they are made in a
staged copy, the current system is snapshotted, and a quick restart (the
kernel keeps running) switches over.

```
wh update                 # download the package index
wh search tcp             # find packages
wh install tcpdump        # install, with whatever it needs
wh install --try nmap     # trial: undone at the restart after next...
wh keep                   # ...unless you keep it
wh remove tcpdump
wh upgrade
wh history                # every change, with its snapshot
wh undo                   # go back to before the last change
```

The package index is signed with Ed25519 and checked on every use; packages
must match the SHA-256 the index lists, and an index older than the one
already downloaded is refused. Packages are unpacked by wh itself, which
never writes outside the system it installs into.

The repository is published at
[CrimsonMac/wormos-packages](https://github.com/CrimsonMac/wormos-packages/releases/tag/core),
together with a source archive for every package built from upstream
code (its recipe, patches and upstream archives).

### Included tools

Besides the desktop, the image comes with everyday command-line tools, all
built from source and kept in the core repository:

| | |
|---|---|
| Shells and terminal | zsh, bash, tmux, screen, less, htop, btop, ncdu, tree, Midnight Commander (`mc`), fastfetch, bc |
| Editors | nano, vim (also `vi`) |
| Text | GNU grep (with `-P`), sed, gawk, find and xargs, diff, cmp, patch |
| Development | git, Python 3 (with `venv` and pip), sqlite3, jq, strace, gdb, binutils (objdump, readelf, nm, strings) |
| Analysis | YARA, ssdeep |
| Archives | tar, cpio and unzip from libarchive (zip, 7z, ISO, xz, zstd...), 7-Zip (`7z`), zip, xz, bzip2, zstd, gzip, lz4, pigz, pv |
| Network | curl, wget, aria2, links (a terminal web browser), torsocks and proxychains (one program through Tor), ip, ss and tc (iproute2), ethtool, whois, traceroute, OpenSSH client (ssh, scp, sftp), rsync, nmap with ncat and nping, tcpdump, wg (WireGuard) |
| Disks and devices | lspci, lsusb, mkfs.fat/fsck.fat, mtools, mkfs.ext4/e2fsck/resize2fs (e2fsprogs), mkfs.exfat/fsck.exfat, smartctl, hdparm, dmidecode |
| Other | file, lsof, gpg, Tor Browser |

They come with the package `wormos-standard`, which recommends them
rather than requiring them: each can be removed with `wh remove`, and wh
remembers that and does not bring it back. When a release adds programs to
the standard edition, `wh upgrade` installs them. (From 1.7, the first
`wh upgrade` brings the new wh and the second the new programs.) `worm
help` lists WormOS's own commands. The commands that also exist in BusyBox (grep, sed, diff, ip...)
take over its small versions; removing the package brings BusyBox's back.
WormOS Barebones has none of these; `worm kit install basic` adds them all.

The repository has more, installed with `wh install`: GNU coreutils, mtr,
iperf3, socat, ngrep, nethogs, macchanger, hexedit, cmatrix, FFmpeg
(`ffmpeg`, `ffprobe`) and sqlmap among them (`wh search` lists everything).

### Software from Debian and Kali

The core repository holds the base system and the tools that need direct
access to the machine. Everything else comes from existing distributions:
when a package is not in core, `wh install` looks in Debian stable, then in
Kali, and installs it into a container for that distribution, a
*wormhole*. Its commands appear on your `PATH` as if installed natively.

```
wh install fastfetch          # not in core: found in Debian, installed there
wh install --from kali hashid # pick the source
wh install wine32:i386        # a 32-bit package (Debian, with i386 added)
wh remove fastfetch
```

`wh search` looks in Debian and Kali too (`--from` picks one source, such
as `--from core`). Before their wormholes exist, it reads their signed
package lists, which root's first search downloads and `wh update` keeps
current.

A wormhole is set up the first time it is needed, from the distribution's
own signed archive (checked with `gpgv` against its archive keyring, which
WormOS ships), and from then on it is managed by its own apt. These
installs take effect immediately, without a restart.

Wormholes share your home folder and the network: they make software
available, they are not a security boundary. They live on a separate
subvolume mounted `nosuid`, so nothing inside one can gain root on the
system. Run untrusted software in a blackhole instead.

### Manual changes

To change the read-only system by hand:

```
worm unlock               # make / writable (only the root mount)
worm lock                 # read-only again, and snapshot the changes
```

## Firewall

Inbound traffic is dropped unless it answers a connection this machine
started (outbound is not filtered). Pings get no answer, so the machine
does not show itself on the network; only the ICMP messages connections
need (errors, IPv6 neighbour discovery) come in. The rules load before any
network interface comes up, and the network is only started once they
are in place: if they cannot be loaded, the machine stays offline. DHCP
requests do not name the system or its DHCP client.

```
worm fw status            # active? which ports are open?
worm fw allow 22          # open a TCP port
worm fw allow 51820/udp   # UDP, or a range such as 6000-6010
worm fw deny 22           # close it again
```

Open ports are stored in `/etc/worm/firewall`, so they are part of system
snapshots: rewinding brings back the firewall of that time.

## Anonymous mode

```
worm anon on              # all traffic of the machine through Tor
worm anon status          # is it on, has Tor connected yet?
worm anon off
```

In anonymous mode only Tor itself reaches the network, and only with TCP.
Every other TCP connection, from any program, user, wormhole or root, is
sent through Tor, DNS queries are answered by Tor, and everything Tor
cannot carry (UDP, ICMP, IPv6) is blocked. The one exception is root's
DHCP renewal of the local address. Connections that were open before the
switch are cut, not carried on outside Tor. The rules load before the network comes
up and stay in place while Tor connects, so nothing leaks in between.
The setting is kept across restarts and is part of system snapshots.
Programs can also use Tor's SOCKS port directly (127.0.0.1:9050).

Where Tor is blocked, it can connect through bridges, with the transports
that come with Tor Browser:

```
worm anon bridges snowflake   # the Tor Project's default bridges of a kind:
worm anon bridges obfs4       #   snowflake, obfs4 or meek
worm anon bridges custom      # paste lines from bridges.torproject.org
worm anon bridges off         # connect directly again
```

With bridges, the transports look up their servers' names before Tor is
connected, through the local resolver (encrypted DNS), as Tor Browser does
with the system's resolver; snowflake also needs UDP for Tor's own user.
Everything else still goes through Tor or is blocked.

## Tor Browser

Tor Browser comes with the image (`/opt/tor-browser`, verified against the
Tor Browser Developers' signature when the image is built). It is in the
launcher and the menu, and `tor-browser` starts it from a terminal. The
first start copies it into `~/.local/share/tor-browser`, where it keeps
its settings and updates itself; on the system's btrfs the copy shares
the files' data and takes a moment.

It runs as your own user, never as root: started from a root terminal
(`su -`), it starts as the user who opened that terminal. In anonymous
mode it uses the system's Tor instead of its own.

On a system without the bundled copy, the first start downloads the
current release from torproject.org (about 150 MB), checks its signature
against the key that comes with WormOS and unpacks it;
`tor-browser --download` does the same without a window.

Anonymous mode hides where traffic comes from, not who you are: logging
in to an account still identifies you. For browsing, Tor Browser
protects against much more than routing alone.

## When something goes wrong

```
worm status               # the state of the system's protections
worm panic                # cut the network, stop all blackholes, snapshot
worm panic off            # bring the network back
```

On the desktop, the power menu (top right) has **Panic: cut the network**,
which does the same for administrators with a second click and asks for
no password. Bringing the network back needs root (`worm panic off`).

`worm panic` drops all traffic and takes the network devices down, stops
every blackhole of every user (keeping their files), and saves the system
and homes as a pinned snapshot for later analysis. It finds running
blackholes through the kernel's own lock table, so nothing a blackhole's
owner wrote can hide one from it.

The boot menu has a **safe mode** entry: it rewinds to the newest
known-good snapshot and starts without network. Snapshots left behind by a
rewind and those saved by `worm panic` never count as known-good. Restart
normally to leave safe mode.

## Privacy and hardening

- DNS goes to a local resolver (unbound) that validates DNSSEC and sends
  queries encrypted (DNS over TLS) to Quad9. The DNS servers a network
  announces are not used. To use other resolvers, put a `forward-zone` in
  `/etc/unbound/unbound.conf.d/` (after `worm unlock`).
- Network devices get a random MAC address at every boot, and IPv6 uses
  temporary addresses for outgoing connections. VMware and Hyper-V guests
  keep the virtual card's address: their virtual switches drop traffic for
  any other. So do vmxnet3 cards, which ignore a new address.
- The kernel is locked down (no unsigned modules, no access to kernel
  memory), io_uring is off, and kernel pointers and logs are hidden.
  Settings are in `/usr/lib/sysctl.d/50-wormos.conf`; override them in
  `/etc/sysctl.d/`.
- Users see only their own processes. Programs cannot run from `/dev`,
  `/run` or `/dev/shm`.

## Blackholes

A blackhole is a disposable sandbox for software you do not trust. Things
go in, nothing escapes.

```
bh open                   # a temporary blackhole, deleted when you leave
bh put lab sample.bin     # copy a file into the blackhole "lab" (/inbox)
bh open lab               # a shell inside it; its files stay between sessions
bh open lab               # again, in another terminal: a second session in it
bh open lab -- ./x        # run one command inside
bh open --net test        # with a network, for trying out a program
bh open --vm lab          # a virtual machine: its own kernel
bh list                   # your blackholes, running or not, and how
bh close lab              # stop it and delete everything in it
```

Inside, programs run as root of the blackhole only: outside, they are an
unprivileged user. They see the system's programs read-only, but not
`/home`, `/root`, `/var`, the disks or the snapshots, nor the host's
processes. There is no network except loopback unless you ask for one.
Any number of blackholes can run at once, each with as many sessions as
you open; a blackhole closes when its first session ends. The layers:

- **Namespaces:** own users, mounts, processes, network, IPC and host name.
- **Capabilities:** root inside keeps only those that act on the
  blackhole's own files and processes; it cannot configure its network
  (no packet filters, routes or traffic control), use raw sockets or
  ptrace the blackhole's init, kernel code that has often been exploited.
- **seccomp:** refuses mounts, new namespaces, kernel keyrings, BPF,
  io_uring, modules, terminal injection and line disciplines, 32-bit
  system calls, and every socket family but UNIX, IPv4, IPv6 and routing
  netlink (no vsock, which crosses network namespaces to the hypervisor).
- **Landlock:** file access stays inside the blackhole, and no signals or
  abstract UNIX sockets reach outside it.
- **Its own terminal:** programs get a pseudo-terminal of the blackhole;
  `bh` relays keys and output, so nothing inside can type into your shell
  or use the terminal's ioctls, and output cannot set your clipboard,
  show desktop notifications or plant links (only harmless terminal
  sequences such as titles and colours pass).
- **Limits** (each blackhole, also those of normal users): at most 4096
  processes and threads (`--pids N`), half the memory (`--memory 2G`),
  no swap, and half the CPU weight of everything else, so a busy
  blackhole cannot make the desktop sluggish.
- Root's blackholes keep their files where other users cannot reach them.

**Network** (`--net`): for a blackhole used as a throwaway test
environment, not for malware. The blackhole gets an address of its own
and reaches the internet through [pasta](https://passt.top), which sends
its traffic as ordinary connections of yours: the firewall, anonymous
mode and `worm panic` cover it. Name lookups go through the system's
encrypted resolver. Nothing can connect in, and the local network and
this computer stay out of reach (`--lan` lets the local network in, never
this computer).

**Virtual machines** (`--vm`): a namespace blackhole shares the kernel with
the host, so a kernel exploit can escape it. With `--vm`, programs run in
a small virtual machine ([kvmtool](https://git.kernel.org/pub/scm/linux/kernel/git/will/kvmtool.git))
on a kernel of their own, seeing the same files as before (the
blackhole's own, the system read-only). The virtual machine monitor itself
runs inside a locked-down blackhole, so even escaping the guest lands in
the sandbox. It needs hardware virtualisation (KVM); inside a virtual
machine, turn on nested virtualisation. Defaults: a quarter of the memory
(at most 4 GiB, `--memory`), 2 CPUs (`--cpus`). One console per virtual
machine; Ctrl+A X stops it. `--net` works here too.

**Other systems** (`--from debian`, `--from kali`): the blackhole gets the
programs of a wormhole instead of WormOS's, for tools that only exist
there; set the wormhole up first with `wh install`.

### Windows malware

Most malware targets Windows. WormOS can take it apart and run much of it
without Windows:

- `worm kit install windows` installs inspection tools (pefile, LIEF,
  YARA, oletools, Detect It Easy, osslsigncode, UPX, rizin, exiftool,
  ssdeep) and Wine with its 32-bit part, all in the Debian or Kali
  wormhole. The inspection tools read the files; nothing runs.
- Run samples in a virtual machine blackhole with Debian's system:

  ```
  bh put lab sample.exe
  bh open --vm --from debian lab -- wine /inbox/sample.exe
  ```

  32-bit Windows programs (still most malware) need 32-bit system calls,
  which a namespace blackhole refuses outright ("Bad system call"), and
  once Wine's 32-bit half is installed, Wine starts 32-bit helpers for
  64-bit programs too. In a virtual machine nothing is filtered, and
  whatever breaks out of Wine still only has the guest's kernel. Without
  `wine32`, 64-bit programs also run in a plain namespace blackhole
  (`bh open --from debian`), which starts faster but shares the kernel
  with the host. Add `--net` only to watch a sample talk; it never
  reaches your local network.
- Measured in a nested test VM: Wine's `cmd.exe` answers in about 3 s in
  a namespace blackhole and 20 to 30 s in a virtual machine the first
  time (creating Wine's prefix over 9p).
- Not yet: blackholes have no graphical display, so only console output
  is visible (graphical programs still run; `WINEDEBUG=+relay` and friends
  show what they do). Malware that looks for Wine or a virtual machine may
  behave differently, and for full behaviour (Windows services, drivers,
  the real registry) a real Windows virtual machine is still the
  reference.

## Tool kits

Tool kits are sets of tools for one kind of work, installed when you need
them instead of shipped with the system:

```
worm kit list                 # the kits
worm kit show forensics       # what one contains
worm kit install forensics    # install it (as root)
worm kit remove forensics     # remove it again
```

| Kit | For |
|---|---|
| `basic` | the everyday tools of the standard edition and a few more (coreutils, GNU text tools, 7-Zip, wget, iproute2, mtr, iperf3, socat, lspci, lsusb, htop, vim, git, Python, dig...): mostly WormOS packages |
| `forensics` | disk images, deleted files, metadata, hashes (sleuthkit, testdisk, foremost, binwalk, exiftool, yara...) |
| `network` | lookups, routes, throughput, captures (whois, traceroute, mtr, iperf3, socat from WormOS; dig, tshark, ngrep...) |
| `blueteam` | malware scanning and audits (clamav, lynis, chkrootkit, rkhunter, yara...) |
| `reverse` | disassemblers and debuggers (radare2, gdb, ltrace, upx, pwntools...) |
| `web` | web application scanners and fuzzers (sqlmap, gobuster, ffuf, feroxbuster...) |
| `passwords` | crackers and hash tools (john, hashcat, hydra, medusa...) |
| `pentest` | from Kali: Metasploit, exploit-db, NetExec, Impacket, Responder... |
| `windows` | Windows programs: pefile, LIEF, YARA, oletools, Detect It Easy, rizin, Wine (run samples with `bh open --vm --from debian`) |

Each tool comes from the first source that has it (Debian, then Kali) and
runs in that distribution's wormhole, which shares your home folder: your
files are the kits' workspace, and the base system stays untouched. A tool
no source has is reported and skipped. Tools that are WormOS packages
(most of `basic`, a few of `network`) are installed last, all in one
change with one quick restart, and removing the kit keeps those that were
there before it. Open samples you do not trust in a
blackhole, not with a kit's tools on your files.

Use these tools only on systems and data you are allowed to examine.

## Roadmap

1. Minimal bootable base system (kernel, libc, init)
2. Snapshot system and package manager
3. Security baseline (firewall, sandboxing, hardening)
4. Graphics stack and Wayland compositor
5. Desktop shell and snapshot timeline
6. Tool kits (first set done)
7. Bootable ISO and installer (live ISO and disk installer done; disk
   encryption still to come)

## Building

WormOS is built entirely from source with its own cross toolchain
(`x86_64-wormos-linux-gnu`). Development targets x86_64 and is tested in
QEMU with UEFI firmware.

How the pieces fit together (image, boot, snapshots, packages, wormholes)
is described in [docs/internals.md](docs/internals.md).

### Requirements

A Debian 13 (or similar) host with roughly 35 GB of free disk space:

```
sudo apt install build-essential bison flex gawk texinfo m4 help2man \
    libncurses-dev libssl-dev libelf-dev libgmp-dev libmpc-dev rsync curl xz-utils bzip2 \
    btrfs-progs dosfstools mtools fakeroot grub-efi-amd64-bin grub-pc-bin bc \
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

### Build and run

```
make            # toolchain, kernel, packages, repository, root filesystem and disk image
make run        # boot the image in QEMU on the serial console (Ctrl-A X quits)
make run-gui    # boot the image in a QEMU window
make test       # boot the image and run automated checks
```

`make test-wormholes` checks the Debian and Kali wormholes against the real
archives; it needs internet access and takes several minutes.

`make test-upgrade FROM=wormos-1.5.2-x86_64.qcow2` sets up an older release
image and upgrades it to the packages just built.

`make test` also runs `tests/desktop-test.py`: the VM gets a virtio GPU,
keyboard and tablet, and the test types on the graphical console through
the QEMU monitor like a user would. It answers the first-boot setup, logs
in (which starts the desktop), types into the terminal, opens another with
Super+Enter, checks a screendump of the real screen, and compares memory
use and system size with their budgets. `tests/desktop-test.py --release
out/release/wormos-1.7-x86_64.qcow2` runs the same checks on a release
image, where root can only log in once the first-boot setup is done.

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

`make iso-barebones` and `make iso-amnesia` assemble the root filesystem
again as that edition and build its ISO (`out/wormos-barebones.iso`,
`out/wormos-amnesia.iso`); `tests/editions-test.py barebones` and
`tests/editions-test.py amnesia` boot them and check what makes each
edition (for Amnesia: Tor from the start, nothing reaching the host past
it, the grey screen). `tests/iso-test.py out/wormos-barebones.iso` runs
the installation checks on Barebones too. `make release VERSION=X`
builds all three editions.

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

The development image has an empty root password (the tests log in with
it). `make release VERSION=1.5` builds the release images instead
(`scripts/release.sh`): root stays locked until the first-boot setup, the
disk is 20 GiB, and the output is the ISO and a qcow2 image in
`out/release`. `make publish-repo` writes the source archives
(`scripts/source-archives.sh`, into `out/sources`) and uploads them with
the repository to the package release (`scripts/publish-repo.sh`, needs
the GitHub CLI).

The root filesystem is read-only; change it with `worm unlock` and
`worm lock`. (A plain `mount -o remount,ro /` would make the whole btrfs
filesystem read-only, including `/var` and `/home`.)

### Layout

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

### Package recipes

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

## License

WormOS's own code (everything in this repository that does not come from
another project) is free software under the GNU General Public License,
version 3 or (at your option) any later version: see [LICENSE](LICENSE).

WormOS is built from many other projects, each under its own license
(GPL, LGPL, MIT, BSD, MPL and others). Their license texts are installed
in `/usr/share/licenses/NAME`, and the source of every package built from
upstream code (its recipe, patches and the upstream archives) is published
next to the package in the [package repository](https://github.com/CrimsonMac/wormos-packages/releases/tag/core).
See [docs/legal.md](docs/legal.md) for details, trademarks and the privacy
statement.

WormOS comes with **no warranty**, to the extent permitted by law (see
sections 15 and 16 of the license). Its security features reduce risk;
they do not make running untrusted software safe, and no system is
impenetrable: read the limits stated in each section above. To report a
security problem, see [SECURITY.md](SECURITY.md).
