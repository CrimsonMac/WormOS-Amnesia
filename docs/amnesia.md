# WormOS Amnesia

WormOS Amnesia is a live edition of WormOS for one-off tasks: it starts
from the ISO, runs entirely from memory, sends all of its traffic through
Tor from the moment it starts, and forgets everything when it stops. It is
the Barebones system plus Tor and Tor Browser, with stricter settings and a
grey look of its own. It is not meant for real work: nothing can be kept.

This document is the plan: what Amnesia promises, what the first version
does, and what comes next. The first version ships as
`wormos-VERSION-amnesia-x86_64.iso` and is marked as a preview until the
second stage below is done.

## Promises

1. **Nothing is kept.** Every start is the same system, exactly as built.
   Files, downloads, settings, history, installed programs and Tor's state
   are gone when the computer stops. There is no switch, setting or
   command that changes this, for the user or for a program running as the
   user.
2. **Nothing touches a disk.** The system never mounts, writes or reads
   the computer's disks. The ISO itself is read-only and is let go of once
   the system is in memory.
3. **Everything goes through Tor.** From the first packet: the firewall
   rules that send every TCP connection and DNS query to Tor, and drop
   everything else (other UDP, ICMP, IPv6), are loaded before any network
   interface comes up. If Tor is not connected, nothing gets out.
4. **Little to identify the machine.** A random MAC address on each start,
   no host name or client name in DHCP, the same user name and time zone
   (UTC) for everyone, no swap.
5. **Honest limits.** Amnesia cannot protect against a compromised
   computer (firmware, hardware keyloggers, a hostile hypervisor when run
   in a VM), against what the user reveals themselves, or against attacks
   on Tor itself. It says so in its welcome text.

## How it works (first version)

- **Edition.** `scripts/build-rootfs.sh` with `WORMOS_VARIANT=amnesia`
  installs the base system, Tor, Tor Browser and the `wormos-amnesia`
  package, and leaves out the installer and the everyday tools.
  `scripts/mkiso.sh` sees `/etc/worm/variant` and builds an ISO with a
  single menu entry (and a no-network one), no installer, and extra kernel
  options: `lockdown=confidentiality` (kernel memory stays unreadable,
  even to root), `init_on_free=1` (freed memory is wiped at once; the
  kernel also defaults to it), `page_alloc.shuffle=1`,
  `randomize_kstack_offset=on`, `vsyscall=none`.
- **Memory only.** As for every WormOS ISO, the initramfs (wormrd) copies
  the system image into a RAM-backed loop device, unmounts the ISO and
  starts from memory; `/var` and `/home` are subvolumes of the same
  in-memory filesystem. The root filesystem is read-only as on every
  WormOS system; the user's home folder, `/tmp` and `/var` are writable,
  so downloading and working inside the session behave normally.
- **Start screen.** Before the desktop, the login screen (wormlogin and the
  greeter in `amnesia` mode) asks how Tor should connect: directly, through
  the Tor Project's default snowflake, obfs4 or meek bridges, or through
  pasted bridge lines (`worm anon bridges`, with the transports Tor Browser
  ships). Tor waits for the answer (`14-anon-hold` creates
  `/run/worm/anon-hold`, which `worm anon start` respects; wormlogin removes
  it and starts Tor), so a network that blocks Tor never sees a direct
  attempt. Without a graphics device there is no screen and Tor never
  starts. The screen also offers an administrator password for this
  session only; empty (the default) means none.
- **No administrator by default.** `live-setup` gives the live user an
  empty password but, unlike the other ISOs, leaves root as the image has
  it: locked in releases. Without root,
  nothing can mount a disk, install software into the system, change the
  firewall or turn anonymous mode off; snapshots cannot be exported.
  Blackholes still work: they need no root. The live user is in Tor's
  group, so `worm anon status` (open to everyone since 1.7) can show how
  far Tor has connected. An administrator set on the start screen is real
  root: it can install programs into memory, but could also write to disks,
  the USB stick Amnesia started from included. The screen says so; the
  promises above hold for sessions without one.
- **Tor.** `wormos-amnesia` ships `/etc/worm/anon`, so the firewall loads
  the anonymous-mode rules at boot (`05-firewall`), before the network
  service brings up an interface (`30-network`); Tor starts once the start
  screen has been answered. `worm anon off` refuses on this edition. Tor keeps its
  state in `/var/lib/tor`, in memory: new guards on every start, so
  sessions cannot be linked through them. Tor Browser uses the system Tor
  (`TOR_PROVIDER=none`, SOCKS on 9050), as in anonymous mode elsewhere.
- **Look.** The session reads `/etc/worm/variant`: `/etc/xdg/amnesia` comes
  first in `XDG_CONFIG_DIRS` (labwc's `rc.xml` and `autostart`, foot's
  `foot.ini`, GTK's `settings.ini`), `GTK_THEME=WormOS-Amnesia` (GTK's
  theme with grey colours; errors stay red), and `WORM_SHELL_PANEL=bottom`
  moves the panel to the bottom edge with its menus opening upwards.
  `/usr/share/wormos/shell/variant.css` turns the panel, launcher and
  notifications grey; the wallpaper is the WormOS one drawn in greys; the
  welcome logo is grey and the welcome terminal opens at every start with
  what Amnesia does and does not do (`/usr/share/wormos/guide-amnesia`).
- **Emergency shutdown.** wormrd links the device the system started from
  as `/dev/wormos-medium`; on Amnesia the service `04-medium`
  (`/usr/lib/worm/medium-watch`) checks every second that it is still
  there and powers off at once (`poweroff -f`, no shutdown sequence) when
  the USB stick is pulled out. A CD or DVD drive stays when its disc is
  ejected, so started from a disc there is nothing to watch.
- **Tests.** `tests/editions-test.py amnesia` boots the ISO from a USB
  stick (QEMU's usb-storage) and checks the
  edition, the kernel options, that Tor runs from the start as its own
  user and cannot be turned off, that a web server and a UDP listener on
  the host hear nothing from the VM, the desktop settings, that the screen
  is grey, and that pulling the stick out (`device_del`) turns the VM off.

## Next stages

### Stage 2: forgetting the memory too

- **Wipe the rest of memory at shutdown.** Done in the first version: the
  session's files (homes, temporary files, Tor's state, logs, sandboxes)
  are deleted after every program has stopped and trimmed back to the RAM
  disk (`/etc/worm/shutdown.d/50-forget`, run by worminit), caches
  dropped; the kernel zeroes pages as it frees them (`init_on_free=1`).
  Pulling the USB stick out goes through the same shutdown. Still to do:
  the system image itself (public data) and kernel memory are not wiped;
  switching back to a shutdown initramfs that detaches the RAM disk would
  free those too.
- **Encrypted memory filesystem.** The in-memory system and home on a
  dm-crypt device keyed by a random key made at each start (needs
  `CONFIG_DM_CRYPT`, which the LUKS work in the installer needs anyway),
  so a memory image taken from outside shows no file contents.
- **An administrator that cannot reach disks.** The optional
  administrator (done) is full root today. A later version could keep
  disks out of reach even for it, for example with a device cgroup policy
  set before the session starts.
- **Time.** Tor needs a roughly correct clock. Today the clock check runs
  over HTTPS (through Tor); a stage-2 check reads the time from Tor's
  consensus instead and warns when the clock is far off.

### Stage 3: more anonymity

- **Bridges** are done (snowflake, obfs4, meek and pasted lines, including
  webtunnel, on the start screen). Still to do: requesting bridges from
  inside Amnesia (moat), and Conjure.
- **Stream isolation:** different programs on different Tor circuits
  (separate SOCKS ports per program, `IsolateDestAddr`).
- **Uniform fingerprint:** the same screen size steps for Tor Browser
  (letterboxing is already on in Tor Browser), the same fonts, a fixed
  keyboard layout list, UTC everywhere.
- **No microphone or camera:** their kernel drivers left out of the
  Amnesia kernel command line (`modprobe.blacklist`), or the devices
  unbound at start.
- **Wi-Fi with a random MAC from the first probe** (WormOS has no Wi-Fi
  stack yet; when it gets one, Amnesia randomises before scanning).
- **Reproducible ISO**, so anyone can check that the published image is
  built from the published sources.

## What it will not do

- Keep anything, even when asked: no persistent storage, ever. A user who
  needs to keep a file has to send it somewhere over the network.
- Install onto a disk.
- Let programs reach the local network or the computer it runs on (in a
  VM: the host).
- Promise anonymity against everything. The welcome text, the guide and
  this document say what it covers.
