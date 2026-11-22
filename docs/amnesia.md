# WormOS Amnesia

WormOS Amnesia is a live edition of WormOS for one-off tasks: it starts
from the ISO, runs entirely from memory, sends all of its traffic through
Tor from the moment it starts, and forgets everything when it stops. It is
the Barebones system plus Tor and Tor Browser, with stricter settings and a
grey look of its own. It is not meant for real work: nothing can be kept.

This document is the plan: what Amnesia promises, what the first version
does, and what comes next. It ships as
`wormos-VERSION-amnesia-x86_64.iso`; the second stage below is done
(since 1.10), so it is no longer a preview.

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
  greeter in `amnesia` mode) asks for the keyboard layout (written to the
  live user's labwc environment, as that user) and how Tor should connect: directly, through
  the Tor Project's default snowflake, obfs4 or meek bridges, or through
  pasted bridge lines (`worm anon bridges`, with the transports Tor Browser
  ships). Tor waits for the answer (`14-anon-hold` creates
  `/run/worm/anon-hold`, which `worm anon start` respects; wormlogin removes
  it and starts Tor), so a network that blocks Tor never sees a direct
  attempt. Without a graphics device there is no screen and Tor never
  starts.
- **No administrator, ever.** `live-setup` gives the live user an empty
  password but, unlike the other ISOs, leaves root as the image has it:
  locked in releases. There is no way to become root: nothing can mount a
  disk, change the firewall or turn anonymous mode off, and snapshots
  cannot be exported. The live user is in Tor's group, so `worm anon
  status` shows how far Tor has connected.
- **Installing without root.** `wormos-amnesia` ships `/etc/wh/live` and
  `/etc/wh/users-may-install`: `wh install`, `remove`, `update` and
  `upgrade` and `worm kit install` by the live user go through wh's setuid
  helper (`/usr/lib/wh/user`), which accepts only those commands and
  package names, and changes go straight into the running system, with no
  restart. Packages come only from the signed repositories (core, Debian,
  Kali), through Tor like everything else, and live in memory until the
  computer stops. Blackholes need no root either.
- **Updates without a new ISO.** `amnesia-update` (also "Update Amnesia"
  in the launcher) brings the USB stick Amnesia started from up to the
  latest release. Each release on the Amnesia repository's release page
  (`scripts/publish-amnesia.sh`; the updater reads the latest one) carries,
  with its ISO, a manifest signed
  with the WormOS key: the ISO's size and SHA-256 and two checksums per
  8 KiB block (since 1.11; from 1.10 to 1.11 that found 63% of the new ISO
  on the stick, 64 KiB blocks found 30%), and `amnesia.latest`, the version
  alone, also signed, which `--check` and the update notice read instead
  of the 2 MB manifest. Amnesia runs from memory, so the stick is free: the
  updater rebuilds the new ISO in memory from the blocks the stick already
  holds, found at any offset with a rolling checksum as rsync and zsync
  do, downloads only the rest through Tor (HTTP range requests, running
  on over gaps of up to four found blocks: over Tor a request costs more
  than a few blocks), checks the whole against the signed SHA-256, writes
  only the 64 KiB pieces that differ from what the stick holds at that
  place (sticks read much faster than they write), and reads it back.
  The system image is compressed `--rsyncable`, so unchanged parts stay
  the same bytes. The updater is setuid but takes nothing from the user:
  a fixed URL and key, and it writes only to the device Amnesia started
  from, only if that is a USB stick (a disc cannot be rewritten). In a
  test (`tests/amnesia-update-test.py`) with 3 MB changed and data shifted
  in the middle, it reused 99% of the blocks.
- **Tor.** `wormos-amnesia` ships `/etc/worm/anon`, so the firewall loads
  the anonymous-mode rules at boot (`05-firewall`), before the network
  service brings up an interface (`30-network`); Tor starts once the start
  screen has been answered. `worm anon off` refuses on this edition. Tor keeps its
  state in `/var/lib/tor`, in memory: new guards on every start, so
  sessions cannot be linked through them. Tor Browser uses the system Tor
  (`TOR_PROVIDER=none`, SOCKS on 9050), as in anonymous mode elsewhere.
- **Clock.** Tor copes with a clock a few hours off, but not days or
  years (a computer whose clock battery is empty starts in 2000). While
  Tor cannot connect, `worm time sync` (every 30 s at first) reads
  the skew relays report in Tor's log ("our clock is behind by 5 hours"),
  sets the clock by it and restarts Tor; the HTTPS check follows once Tor
  is up. Amnesia never writes the computer's hardware clock.
- **Stream isolation.** Tor's TransPort gives every destination address
  its own circuit (`IsolateDestAddr`), so an exit relay cannot link the
  connections of different programs or to different sites; Tor Browser
  isolates its own sites through SOCKS.
- **No microphone, no camera.** The kernel has no camera drivers at all.
  A udev rule (`90-amnesia-microphone.rules`) gives the sound capture
  devices to root alone, so neither PipeWire nor any program can record;
  playback works. With no root, nothing can change that.
- **Update notice.** Once Tor is connected, the session checks once for a
  newer release (`amnesia-update --check`, signed manifest) and says so in
  a notification.
- **Look.** The session reads `/etc/worm/variant`: `/etc/xdg/amnesia` comes
  first in `XDG_CONFIG_DIRS` (labwc's `rc.xml` and `autostart`, foot's
  `foot.ini`, GTK's `settings.ini`), `GTK_THEME=WormOS-Amnesia` (GTK's
  theme with grey colours; errors stay red), and `WORM_SHELL_PANEL=bottom`
  moves the panel to the bottom edge with its menus opening upwards.
  `/usr/share/wormos/shell/variant.css` turns the panel, launcher and
  notifications grey; `/usr/share/wormos/greeter/amnesia.css` (loaded by
  the greeter in Amnesia mode after its own stylesheet) does the same for
  the start screen, and `/etc/zsh/prompt-colors` for the shell prompt; the
  wallpaper is the WormOS one drawn in greys; the welcome logo is grey and the welcome terminal opens at every start with
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
  is grey, and that pulling the stick out (`device_del`) turns the VM off;
  also the keyboard layout chosen on the start screen, the microphone,
  stream isolation and the update notice. `--skew -48` starts the VM with
  its clock two days ahead (kvmclock off, so the guest reads the RTC) and
  checks that the clock is set from Tor's reports.
  `tests/amnesia-update-test.py OLD.iso NEW.iso` (or `--synthetic`) runs
  the updater against a local server; `tests/unit/newer.c` checks its
  version comparison.
- **Continuous integration** (`.github/workflows`, besides WormOS's own):
  `amnesia.yml` runs the updater test on synthetic ISOs on every push, and
  on every release, every week and by hand checks the published release's
  files (`scripts/check-amnesia-release.sh`: SHA256SUMS, the manifest's
  signature, its ISO name, size and SHA-256) and boots the ISO through the
  edition test in QEMU with KVM. `sync-upstream.yml` checks every day for
  WormOS commits not merged here and whether they merge cleanly, in an
  issue titled "Merge WormOS" (the merge is done by hand: the workflow
  token may not push changes to workflows). The README is Amnesia's own:
  `.gitattributes` keeps it in merges (`git config merge.ours.driver
  true`).

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
- **Encrypted memory filesystem: not planned.** The key of a dm-crypt
  device made at each start would live in the same memory it protects: a
  memory image taken while the system runs, or a cold-boot attack after
  it stops, finds the key with the data. It would also cost memory (the
  image's empty space must be written through the cipher: about 660 MB
  more for a 1 GB image with 340 MB used). Wiping, which Amnesia does at
  every shutdown (above), is what defends against reading memory
  afterwards.
- **Updates offered by themselves.** Done: a notification once Tor is
  connected.
- **Time.** Done: the clock is set from Tor's skew reports when it is too
  far off for Tor to connect.

### Stage 3: more anonymity

- **Bridges** are done (snowflake, obfs4, meek and pasted lines, including
  webtunnel, on the start screen). Still to do: requesting bridges from
  inside Amnesia (moat), and Conjure.
- **Stream isolation:** done for destinations (`IsolateDestAddr`); still
  to do: separate SOCKS ports per program.
- **Uniform fingerprint:** the same screen size steps for Tor Browser
  (letterboxing is already on in Tor Browser), the same fonts, a fixed
  keyboard layout list, UTC everywhere.
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
