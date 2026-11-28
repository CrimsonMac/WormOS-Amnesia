# WormOS user guide

Everything WormOS can do, section by section: the editions, the desktop and
terminal, snapshots, packages, the firewall, anonymous mode and Tor Browser,
blackholes and tool kits. For an overview, see the [README](../README.md);
for building WormOS yourself, [building.md](building.md); for how it works
inside, [internals.md](internals.md).

## Trying it

Download a release from the
[release page](https://github.com/CrimsonMac/WormOS/releases/latest) and
check it: from 1.17, `SHA256SUMS` is signed with the WormOS key, and
[`scripts/verify-release.sh`](../scripts/verify-release.sh) checks the
signature and the files with OpenSSL alone
(`sh verify-release.sh DIR core.pem`, with
[`keys/core.pem`](../keys/core.pem)). All of them start with UEFI (Secure
Boot on or off) or BIOS firmware.

### Editions

| | WormOS | WormOS Barebones |
|---|---|---|
| File | `wormos-1.10-x86_64.iso`, `.qcow2` | `wormos-1.10-barebones-x86_64.iso` |
| What | the full system | the operating system alone: desktop, network, firewall, snapshots, packages, sandboxes |
| Tor, Tor Browser | yes (anonymous mode off until you turn it on) | no (`wh install tor tor-browser`) |
| Everyday tools | yes (below) | no (`worm kit install basic`) |

Both run live from the ISO and install onto a disk. WormOS Amnesia, a
live-only system that forgets everything and sends all traffic through
Tor, is developed in its own repository,
[CrimsonMac/WormOS-Amnesia](https://github.com/CrimsonMac/WormOS-Amnesia),
on top of this one.

- **The ISO** (`wormos-1.10-x86_64.iso`): a live system and an installer.
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

  **Encrypt the disk** (an option in the installer) encrypts everything on
  it, system, home folders and snapshots, with a passphrase (LUKS2). At
  every start the boot menu asks for it once; the system then opens the
  disk by itself with a key kept inside the encrypted disk. Without the
  passphrase nobody can read the disk, and nobody can recover it for you.
  (`install-system install DISK --encrypt` reads it from standard input.)

- **The disk image for QEMU / virt-manager** (`wormos-1.14.1-x86_64.qcow2`): an
  installed system, ready to start. In virt-manager,
  import an existing disk image and choose a generic recent Linux.

  **Smooth drawing.** virt-manager's plain SPICE and VNC displays show
  half-sent frames when the screen changes quickly (a scrolling
  terminal): QEMU sends SPICE changes as strips 32 pixels wide, and when
  output outruns encoding its VNC server sends part of a frame and the
  rest later. A line then rolls across the window. WormOS's own frames
  are complete; the fix is a display that hands whole frames over on the
  GPU:

  - *SPICE with OpenGL* in virt-manager (hosts with Intel or AMD
    graphics): in the machine's details, *Display Spice*, set *Listen
    type* to *None* and tick *OpenGL*, choosing the GPU your desktop runs
    on; in *Video Virtio*, tick *3D acceleration*.
  - *QEMU's own window* (any host, including NVIDIA's driver, where
    SPICE with OpenGL fails: QEMU 11.1 hands the viewer frame handles
    that are not DMA-BUFs, so the window stays black or virt-manager
    stops responding with `failed to create EGL image from DMA-BUF`).
    Shut the machine down in virt-manager, then start the same disk
    with:
    ```
    qemu-system-x86_64 -machine q35,accel=kvm,vmport=off -cpu host -smp 4 -m 4G \
        -drive file=wormos-x86_64.qcow2,if=virtio,format=qcow2 \
        -device virtio-vga-gl -display gtk,gl=on,zoom-to-fit=off \
        -device virtio-tablet-pci -device virtio-keyboard-pci \
        -nic user,model=virtio-net-pci \
        -audiodev pipewire,id=snd0 -device ich9-intel-hda \
        -device hda-duplex,audiodev=snd0 \
        -device virtio-serial-pci \
        -chardev socket,id=vdagent0,path=/tmp/wormos-clipboard.sock,server=on,wait=off \
        -device virtserialport,chardev=vdagent0,name=com.redhat.spice.0 &
    tools/qemu-clipboard /tmp/wormos-clipboard.sock
    ```
    The screen follows the window size and sound goes through PipeWire.
    `vmport=off` matters: otherwise QEMU uses VMware's mouse emulation,
    which WormOS reads as a relative mouse, and the window keeps moving
    the host pointer to the guest's. QEMU's window cannot share the
    clipboard on a Wayland desktop (it may set and read it only in narrow
    cases), so the guest's agent port goes to a socket and
    `tools/qemu-clipboard` (from this repository; Python with GObject)
    moves text between it and the desktop's clipboard, through KDE's
    Klipper or `wl-copy`/`wl-paste`; it ends with QEMU. Ctrl+Alt+G releases
    the pointer, Ctrl+Alt+F switches to full screen.

  In virt-manager, *View* → *Scale Display* → *Auto resize VM with
  window* makes WormOS use the window's size instead of being stretched
  to it (it is off by default).

  With an NVIDIA card, libvirt also hides the card's device files from
  QEMU (`eglInitialize failed`); allow them once, as root:
  ```
  cat >> /etc/libvirt/qemu.conf <<'EOF'
  cgroup_device_acl = [
      "/dev/null", "/dev/full", "/dev/zero", "/dev/random", "/dev/urandom",
      "/dev/ptmx", "/dev/userfaultfd",
      "/dev/nvidia0", "/dev/nvidiactl", "/dev/nvidia-modeset",
      "/dev/nvidia-uvm", "/dev/nvidia-uvm-tools"
  ]
  EOF
  systemctl restart libvirtd
  ```

  From the command line:
  ```
  qemu-system-x86_64 -machine q35 -accel kvm -cpu host -m 2048 -smp 2 \
      -drive file=wormos-1.10-x86_64.qcow2,if=virtio \
      -nic user,model=virtio-net-pci -device virtio-vga \
      -device virtio-keyboard-pci -device virtio-tablet-pci \
      -device virtio-serial-pci \
      -chardev qemu-vdagent,id=vdagent,name=vdagent,clipboard=on,mouse=off \
      -device virtserialport,chardev=vdagent,name=com.redhat.spice.0
  ```
  The last three lines share the clipboard with VNC viewers (and with
  QEMU's window on an X11 desktop; on Wayland, see `tools/qemu-clipboard`
  above). virt-manager's
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
      -drive file=wormos-1.10-x86_64.qcow2,if=virtio `
      -nic user,model=virtio-net-pci -device virtio-vga -display sdl `
      -device virtio-keyboard-pci -device virtio-tablet-pci
  ```
  If QEMU says WHPX is not available, use `-accel tcg` instead: it works
  without the Windows feature, but much more slowly. Ctrl+Alt+G releases
  the mouse.
- **VirtualBox:** create a 64-bit "Other Linux" machine with EFI or BIOS,
  2 GB of memory or more, graphics controller VMSVGA (3D acceleration on
  for OpenGL ES drawing) and boot the ISO. WormOS draws the pointer itself
  there; `worm vm shares NAME` mounts a shared folder at `/media/NAME`. VirtualBox's
  shared clipboard and window resizing need its Guest Additions, which
  WormOS does not ship: set the screen size in Settings > Display.
- **VMware is not supported for now.** It cannot be tested at the moment,
  so there is no VMware bundle and the VMware guest tools are left out.
  The ISO may still work in VMware, but nothing is promised; QEMU,
  virt-manager (KVM) and real PCs are the supported targets.

### Secure Boot

WormOS starts with Secure Boot on, the way most Linux distributions do:
the firmware starts a small loader (shim) that PCs trust out of the box,
and shim starts WormOS's boot loader and kernel, which are signed with
WormOS's own key. The firmware does not know that key yet, so the first
start with Secure Boot on asks you to add it, once:

1. A blue screen (shim's MokManager) says the verification failed. Press a
   key, then choose **Enroll key from disk**.
2. Pick the disk (on a USB stick or CD, the WormOS one; on an installed
   system, the `WORMOS-EFI` partition), then `EFI`, `BOOT`, `wormos.cer`.
3. Check that it is "WormOS Secure Boot 2026", choose **Continue**, **Yes**
   and **Reboot**.

From then on WormOS starts like any other system, and the firmware still
refuses anything not signed by Microsoft's or WormOS's key. The kernel
runs locked down (no unsigned modules, no access to its memory) either way.

What it does not cover: the boot menu's settings and the initramfs are not
signed, so someone with the disk in hand could change how the signed
kernel starts. An encrypted install keeps the initramfs and the system on
the encrypted part, which leaves only the boot loader's menu file.
Releases from 1.17 on are signed; the certificate is
[`keys/secureboot.crt`](../keys/secureboot.crt) (builds without that key
sign with a key of their own, made for the build).

### In a virtual machine

WormOS finds the hypervisor at every start and sets itself up for it
(`worm vm` shows what it found, what it set and tips for the host):

- **Base settings per hypervisor** ship in the image
  (`/usr/share/worm/vm/qemu.conf`, `virtualbox.conf`, `hyperv.conf`...):
  how the desktop draws (OpenGL ES on a 3D-capable virtual GPU, software
  otherwise), whether the pointer is drawn into the picture (virgl,
  VMSVGA, Hyper-V), the SPICE agent, the guest agent, shared folders and
  disk tuning. Change one with `worm vm set KEY VALUE` (as root, kept in
  `/etc/worm/vm.conf`; `worm vm reset KEY` undoes it) or in
  Settings > Virtual machine.
- **Guest agent:** give the VM a guest agent channel (virt-manager: *Add
  Hardware* → *Channel* → `org.qemu.guest_agent.0`, or `-device
  virtserialport,name=org.qemu.guest_agent.0` with a chardev) and
  virt-manager shows WormOS's IP addresses, *Shut Down* works through the
  agent, and `virsh domtime --sync` sets the clock after the host slept.
  WormOS's agent only reports and shuts down: it has no commands to run
  programs or read and write files in the guest.
- **Shared folders:** folders the host shares over virtiofs or 9p
  (virt-manager: *Add Hardware* → *Filesystem*) appear under `/media/TAG`
  at start; `worm vm shares` mounts ones added later. VirtualBox names
  its shared folders only to its Guest Additions, so mount them by name:
  `worm vm shares NAME` (as root) gives `/media/NAME`.
- **Tuning:** virtual disks get no I/O scheduler (the host has one) and
  writeback starts earlier, so small VMs do not stall on large writes;
  the memory balloon gives unused memory back to the host when its
  free page reporting is on.

On the first start of an installed system (from the ISO or the disk
image), WormOS asks for your name, a user name, a password for it and a
password for root. Then the desktop starts; later, log in on the login
screen, which shows the version in its bottom left corner (as does the
title of `welcome --full`).

## The desktop

The desktop is [labwc](https://labwc.github.io), a light stacking Wayland
compositor, with the [foot](https://codeberg.org/dnkl/foot) terminal. It
idles at well under 100 MB of memory.

Everything everyday has a window, so the terminal is there when you want
it, not because you need it:

- **Welcome** opens the first time you log in: a short tour of snapshots,
  programs, privacy and the keyboard shortcuts, each page with a button
  that opens the program it is about. It is in the launcher and the
  desktop menu; Super+/ opens its list of keyboard shortcuts.
- **Files** (`wormos-files`, Super+E): your folders on the left (home, the
  user folders, the trash, the whole computer, drives and shared folders
  under `/media`), the folder as icons, a path bar to go back up. Double
  click opens a folder, or a file with its program; typing in the search
  field filters the folder. Right click (or the keys) to open, rename (F2),
  copy (Ctrl+C), cut (Ctrl+X), paste (Ctrl+V), move to the trash (Delete;
  in the trash, Delete removes for good), make a folder (Ctrl+Shift+N),
  show hidden files (Ctrl+H) or open a terminal there. Alt+Left goes
  back, Alt+Up to the parent folder. Pictures show as thumbnails. Drag
  files to another folder or onto a place in the sidebar to move them
  (hold Ctrl to copy), or drop files from other programs into the window.
  Other programs open folders in it. *Open in a blackhole* (right click)
  opens a file you are not sure about in a sandbox (see Blackholes).
- **Viewer** (`wormos-viewer`) shows pictures and PDF documents: + and −
  zoom, 0 fits the window, Left and Right go through pages or through the
  other pictures in the folder, R rotates, F11 is full screen. PDFs are only
  drawn: links, forms and scripts in them do nothing.
- **Text Editor** (`wormos-editor`) edits text files: Ctrl+S saves,
  Ctrl+F finds, Ctrl+L goes to a line, undo and redo, word wrap. Closing
  with unsaved changes asks first. Files, Viewer and Text Editor are the
  default programs for folders, pictures and PDFs, and text
  (`/etc/xdg/mimeapps.list`; your own `~/.config/mimeapps.list` wins).
- **Software** (`wormos-software`): Explore lists popular programs and
  searches every source (WormOS's own packages, then Debian's and Kali's);
  Installed lists what is there; Updates looks for newer versions and
  installs them all at once. Installing or removing asks for your
  password. Programs from Debian and Kali are ready in moments, and so are
  WormOS packages that only add files (new programs); WormOS packages that
  change what is there (updates, removals) are applied at the next restart
  (`wh --later`), so nothing closes under you: the window says so and
  offers to restart. Administrators
  also get a notification when WormOS has updates (a few minutes after
  logging in, then daily; a click opens Software's Updates; `touch
  ~/.config/wormos/no-update-notice` turns it off).

- The Super key (or the button at the left of the panel) opens the
  launcher: start typing to search every program and tool by name,
  command, keywords or description, the best match first; the arrow keys
  choose and Enter starts. Its sidebar has Recent, All programs, the usual
  categories and, once tools are installed, Kali's sections
  (Reconnaissance, Credential Access, Discovery, Forensics and so on).
  Programs with a window show as tiles; command line tools as rows that
  open a terminal running the tool's help, ready to use it.
  The launcher also has **quick actions**, found by searching: "snapshot"
  takes one, "screenshot" (also in five seconds), "do not disturb",
  "wallpaper", "keyboard", "time zone", "virtual machine" and the other
  pages of Settings.
- Right click on the background opens the menu: Files, terminals,
  applications installed from wormholes, Software, snapshots, Settings,
  screenshots, Welcome, log out.
- The **quick settings** (the power button at the right of the panel):
  Settings, a terminal, a screenshot, Snapshots, a system monitor, *Do not
  disturb* (only urgent notifications show), the display scale, panic,
  log out, restart and shut down.
- Keys: Super+Enter a terminal, Super+Q closes a window, Alt+Tab
  switches, Super+Arrow keys snap windows, Super+I Settings, Super+E Files,
  Super+S a snapshot, Super+N do not disturb, Super+/ all the shortcuts, Print
  a screenshot in `~/Pictures` (its path is copied, and a notification
  says where it went), Shift+Print one in five seconds.
- **Settings** (`wormos-settings`, Super+I) has pages for the display
  (scale, the screen's mode), appearance (wallpaper: WormOS's, a plain
  colour or a picture; the panel at the top or bottom), keyboard (layout,
  key repeat), mouse and touchpad (speed, natural scrolling, left-handed,
  tap to click), sound (output and input devices and their volume),
  notifications, date and time (time zone, the panel clock's format),
  network and privacy (addresses, anonymous mode, automatic snapshots,
  the firewall), the virtual machine and About (version, hardware, the
  computer's name, your password). Changes to the system ask for the root
  password. `worm status` and `worm fw status` work for administrators
  (the group `wheel`) without `su`. `wormos-settings --page sound` opens one page.
- Sound goes through [PipeWire](https://pipewire.org), which also serves
  programs written for PulseAudio or ALSA (including those installed from
  Debian or Kali). The speaker button in the panel sets the volume (scroll
  on it, or use the volume keys) and picks the output (speakers,
  headphones, HDMI) and the microphone; so does Settings > Sound
  (`wpctl status` and `wpctl set-default ID` in a terminal). Both list
  every connector that can be used by a plain name, the speakers and the
  headphones both while headphones are plugged in, and update by themselves
  when something is plugged in or out.
- **Wi-Fi**: the network button in the panel lists the networks in range
  (strongest first); click one, type its passphrase if it has one, and
  WormOS connects and remembers it (it reconnects by itself later). The
  switch there turns Wi-Fi off and on. Every start uses a new random MAC
  address for Wi-Fi (scans always use random ones), so networks cannot
  recognise the computer, and DNS stays with WormOS's encrypted resolver,
  not the network's. Wi-Fi starts only behind the firewall and follows
  `worm panic`. In a terminal: `iwctl station wlan0 get-networks`,
  `iwctl station wlan0 connect NAME` (enterprise networks too). Intel,
  MediaTek (MT7921, MT7922, MT7925) and Realtek (rtw88, rtw89) Wi-Fi and
  Bluetooth adapters have their drivers and firmware; Realtek wired
  network cards too.
- **Bluetooth** (Settings > Bluetooth): switch it on, search for devices,
  then Connect pairs, trusts and connects one (headphones, speakers,
  mice and keyboards that pair without a code); Forget removes it.
  Bluetooth audio goes through PipeWire like any other output. It starts
  switched off at every start. In a terminal: `bluetoothctl`. Like Wi-Fi,
  Bluetooth is for administrators (the group `wheel`, which the first user
  is in); other accounts cannot pair or connect devices.
- A laptop's second graphics card (NVIDIA or AMD, next to Intel graphics)
  has no driver in WormOS; it is switched off at start instead of draining
  the battery. A screen connected to a port wired to that card stays dark.
- Laptop sound through Intel's audio DSP (Smart Sound, on most Intel
  laptops since about 2020) uses Sound Open Firmware, so the speakers and
  built-in microphones work; USB webcams work as cameras.
- To reach the computer from another one over SSH, `wh install
  openssh-server`, put your public key in `~/.ssh/authorized_keys` and open
  the port with `worm fw allow 22`. Logins take keys only, never passwords.
- On a laptop the brightness keys and the slider in the quick settings
  (the power button in the panel) set the screen's brightness; it comes
  back at the next start. Touchpads and touchscreens on I2C (most laptops'
  built-in ones) work like a mouse.
- On a laptop the panel shows the battery's charge (and whether it is
  charging), and warns once when it falls to 10%.
- Ctrl+Alt+F2 switches to a second console (for logging in as root);
  Ctrl+Alt+F1 returns to the desktop.
- Administration: administrators (members of the group `wheel`, as the
  user made at setup is) confirm with **their own password**: in the root
  terminal, for `sudo COMMAND`, in Settings, Snapshots, Software and the
  other programs that change the system. `su -` still asks for root's
  password, which the setup also sets. Graphical
  programs open from the root terminal and from `sudo` (they keep the
  desktop's display), not from a plain `su -`; programs from wormholes run
  as well from your own terminal or the launcher.
- `passwd` (or `worm passwd`) changes your own password, although `/etc`
  is read-only: it asks for the current one first.
- Time: the first-boot setup asks for your time zone; `worm time` shows the
  time, the zone and the last clock check, `worm time zone Europe/Berlin`
  changes the zone (as root; `worm time zones` lists them). The clock is
  checked over HTTPS every six hours (the `Date` of an authenticated
  answer, which unlike plain NTP cannot be forged on the way, and which
  also works through Tor). In anonymous mode, a clock too far off for Tor
  to connect (days or years, as on a computer whose clock battery is
  empty) is set from the skew Tor's relays report, and Tor starts again. The live system never changes the computer's
  hardware clock.
- Copy and paste between WormOS and the machine it runs on works with
  virt-manager (SPICE) and QEMU (see above). In virt-manager the screen
  follows the window when View, Scale Display, "Auto resize VM with
  window" is on (the default), from the login screen on. In a terminal, `clip copy`
  copies its arguments or standard input and `clip paste` prints the
  clipboard (`ls | clip copy`).
- Configuration files: `/etc/xdg/labwc` and `/etc/xdg/foot/foot.ini`.
  The session merges every labwc `rc.xml`: the system's, the Settings
  program's (`~/.config/wormos/xdg/labwc/rc.xml`) and your own
  `~/.config/labwc/rc.xml`, which needs only what it changes and wins;
  `~/.config/labwc/environment` holds the keyboard layout.
- Windows: drag one to a screen edge for half the screen, into a corner
  for a quarter, to the top to maximize; hold Ctrl, Alt or Shift while
  dragging to drop it into a third. Screen edges and other windows' edges
  hold a moving window briefly, so windows line up. From the keyboard:
  Super+Left/Right/Up for halves, two of them in a row (Left then Up) for
  a quarter, Super+Alt+Left/Up/Right for thirds, Super+Down to restore,
  Super+F to maximize, Super+C to centre. Super with the left mouse
  button moves a window from anywhere in it, with the right button
  resizes it; the size shows while resizing.
- Display scale (Settings > Display, or the quick settings): Automatic (200% from 2000
  pixels high), 75% to 200%. Below 100% everything gets smaller and more
  fits on the screen. On a host that scales its own desktop (KDE or GNOME
  at 125%), virt-manager gives the VM its window size in scaled pixels and
  the host enlarges the picture again, so WormOS looks big: 80% there
  gives the same amount of room as the host's own windows. The choice is
  kept in `~/.config/wormos/display` (`scale = 0.8`).
- Programs for X11 (Java programs such as Ghidra and Burp Suite, older
  toolkits, and anything installed from Debian or Kali that does not
  speak Wayland) run in Xwayland, which starts with the first one.
- Screen resolution: Settings > Display lists each screen's modes; in a
  terminal, `wlr-randr` lists them and `wlr-randr --output NAME
  --custom-mode 1920x1080` sets any size. To keep a setting, add the
  command to `~/.config/labwc/autostart`.
- The command line tools that come with WormOS have menu entries and
  icons: Vim, nano, Midnight Commander, disk usage (ncdu), Python, tmux,
  SQLite, GDB, system information (fastfetch), htop and btop, plus the
  WormOS guide, its command list, the system status and the virtual
  machine's details.

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
  mounted read-only), `@var` (logs and state), `@home`, `@wormholes` and
  `@snapshots`.
- System snapshots include the wormholes (the Debian and Kali systems and
  the programs installed there), so a rewind brings those back too, and
  wh takes a snapshot before installing or removing a program from a
  wormhole, which `wh undo` goes back to. Wormholes have no manifest: they
  are convenience, not part of the verified system.
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

The Software program (see The desktop) does all of this in a window. In a
terminal, `wh` (short for wormhole) installs software from the signed WormOS
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

A change that only adds files (a new program, with no package upgraded or
removed) is also copied into the running system, so it works at once; the
staged system, with the same files, takes over at the next restart.

The package index is signed with Ed25519 and checked on every use; packages
must match the SHA-256 the index lists, and an index older than the one
already downloaded is refused. Packages are unpacked by wh itself, which
never writes outside the system it installs into.

Upgrades are quick on the network and on the disk: all packages download
at once; a package whose installed version is the one published before
comes as a delta holding only the files that changed (the full package
if a file it leaves out was changed on this system); files that did not
change are left in place, so they take no extra space next to the
snapshots; and downloaded packages are deleted once installed.

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
*wormhole*. Its commands appear on your `PATH` as if installed natively,
and it gets an entry in the launcher, as in Kali's menu, from the
`tool-menus` package (built from Kali's `kali-menu`, which has the menu
entries of the tools Kali and Debian carry): programs with a window start
directly, command line tools open a terminal that runs the tool and stays
open, and tools that need root ask for the root password first. Each
lands in its section of the launcher.

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

Programs from wormholes work like the system's own, whatever they are
built with (GTK, Qt, Java, Electron, plain X11):

- they open on the desktop: Qt, GTK and SDL programs use Wayland when the
  wormhole has their Wayland support and Xwayland otherwise;
- wh adds what such programs need but Debian only recommends (the SVG
  icon loader and icon theme for GTK programs, the Wayland and SVG
  plugins for Qt programs; `/usr/share/wh/runtime` lists these rules);
- a program that opens a link, a file or a terminal, or that asks for root
  with `sudo` or `pkexec` (GParted, `msfdb init`), reaches the desktop:
  links and files open with the desktop's programs, terminals open on the
  desktop, and root requests open the root terminal, which asks for the
  root password and runs the program there;
- they see your home folder, `/tmp`, disks under `/mnt` and `/media`, and,
  run as root, root's home; elsewhere they see their distribution's own
  files (they say so when started from another folder);
- every package with commands gets a launcher entry: its own, Kali's (with
  Kali's helper scripts, such as John the Ripper's list of `*2john`
  helpers, run inside the wormhole), or else one that lists its commands;
- a package split in parts (`vlc`, `keepassxc`) or with commands behind
  Debian's alternatives (`nc`) exports them all, and a full program
  replaces BusyBox's small version of the same name (`ipcalc`, `nc`) on
  your `PATH`. Library packages (`python3-pefile`) are for programs in the
  wormhole: `whc debian python3` runs Debian's Python with them.

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
cannot carry (UDP, ICMP, IPv6) is blocked. Each destination address gets
its own Tor circuit, so an exit relay cannot link connections to different
sites. The one exception is root's
DHCP renewal of the local address. Connections that were open before the
switch are cut, not carried on outside Tor. The rules load before the network comes
up and stay in place while Tor connects, so nothing leaks in between.
The setting is kept across restarts and is part of system snapshots.
Programs can also use Tor's SOCKS port directly (127.0.0.1:9050).

`worm anon new-circuits` (or "New Tor circuits" in the panel's protection
menu) asks Tor for fresh circuits: connections opened from then on cannot
be linked to earlier ones by the relays they go through. Pages already
open keep their circuits until they are reloaded. Administrators need no
password for it, and Tor accepts it once every 10 seconds. Tor Browser
uses the system's Tor in anonymous mode, so this is how to get new
circuits for it too. While Tor connects, the protection menu shows how
far it has got, for users who may read Tor's log (the group `tor`).

Where Tor is blocked, it can connect through bridges, with the transports
that come with Tor Browser:

```
worm anon bridges snowflake   # the Tor Project's default bridges of a kind:
worm anon bridges obfs4       #   snowflake, obfs4 or meek
worm anon bridges custom      # paste lines from bridges.torproject.org
worm anon bridges auto        # ask the Tor Project which bridges work here
worm anon bridges auto ir     #   ... or in the country given (two letters)
worm anon bridges off         # connect directly again
```

With bridges, the transports look up their servers' names before Tor is
connected, through the local resolver (encrypted DNS), as Tor Browser does
with the system's resolver; snowflake also needs UDP for Tor's own user.
Everything else still goes through Tor or is blocked.

`auto` asks the Tor Project's bridge service, as Tor Browser's Connection
Assist does, which transports work from where you are. The question goes
through a meek tunnel to a large content delivery network, so a censor
would have to block that whole network to stop it. If nothing is blocked,
Tor connects directly; otherwise the answer's bridges are set up as above.
Without an answer, the meek bridges stay set.

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

## Locking the screen and sleep

**Super+L**, or **Lock** in the panel's power menu, locks the screen; your
password opens it again. Closing a laptop's lid does nothing for the first
10 seconds (time to open it again if you closed it by mistake); after that
WormOS locks the screen and the laptop sleeps. Opening the lid wakes it at
the lock screen. Users without a password (the live system) are never
locked out: the laptop just sleeps.

The lock is meant to hold against someone with the computer in hand:

- The lock screen uses the Wayland session lock: the compositor shows
  nothing else and gives no input to anything else, keyboard shortcuts
  included. If the lock screen crashed, the screens would stay covered and
  a new lock screen would appear.
- Each wrong password costs two seconds; after 5 in a row the next try
  waits 30 seconds, after 10 two minutes, after 20 ten minutes.
- The same holds for every other place that checks a password (the login
  screen, the root terminal and `sudo`, Settings, Software, Snapshots,
  `worm passwd`): checks of one account's password run one after another,
  however many are started at once, and wrong ones in a row make each next
  try wait longer (2, then 5, 15 and 30 seconds). A program running as you
  cannot guess your password any faster than you could type it.
- The laptop sleeps only once the screen is locked, never before.
- Release images: GRUB's menu entries boot, but cannot be edited and GRUB's
  command line cannot be opened, so the kernel cannot be started with a
  shell instead of the system; the initramfs never opens a rescue shell.
  The text console (Ctrl+Alt+F2) asks for a password like the login screen.

There is no way around a forgotten password on a release system: no reset,
no recovery shell. What stays possible with the computer in hand: booting
other media, or taking the disk out, reads an unencrypted disk; choose the
encrypted install (and a firmware password, with Secure Boot) to close that
too. Rewinding the system to an older snapshot from the boot menu also
brings back that snapshot's passwords.

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
bh open --gui lab -- mousepad /inbox/notes.txt   # programs with windows
bh list                   # your blackholes, running or not, and how
bh close lab              # stop it and delete everything in it
```

Inside, programs run as root of the blackhole only: outside, they are an
unprivileged user. They see the system's programs read-only, but not
`/home`, `/root`, `/var`, the disks or the snapshots, nor the host's
processes. There is no network except loopback unless you ask for one.
Any number of blackholes can run at once, each with as many sessions as
you open; a blackhole closes when its first session ends.

**Windows from a blackhole.** `bh open --gui` lets the programs inside open
windows on the desktop, through a display of the blackhole's own that the
compositor marks as sandboxed (Wayland's security context): those windows
cannot take screenshots or record the screen, read the clipboard in the
background or set it as a clipboard manager, list or control other
windows, or type and move the pointer for you. They always get WormOS's
own title bar. Copy and paste still work while you use the window, as in
any program. With `--from debian` the programs come from the Debian
wormhole's system (install them there first, with Software or
`wh install`). Programs for Wayland work (GTK, Qt, most modern ones); X11
programs do not, and there is no GPU inside, so drawing is by the CPU.
`--gui` does not combine with `--vm`.

**Open in a blackhole.** In Files, right click a file and choose *Open in
a blackhole*: the file is copied into a new blackhole's `/inbox` and its
usual program opens it there (from its wormhole when it comes from Debian
or Kali), with no network and nothing of your files. When the program
closes, the blackhole and the copy are deleted. Use it for downloads and
attachments you are not sure about.

The layers:

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
worm kit install web passwords   # several at once
worm kit install all          # every kit
worm kit remove forensics     # remove it again (several, or all, too)
```

| Kit | For |
|---|---|
| `basic` | the everyday tools of the standard edition and a few more (coreutils, GNU text tools, 7-Zip, wget, iproute2, mtr, iperf3, socat, lspci, lsusb, htop, vim, git, Python, dig...): mostly WormOS packages |
| `forensics` | disk images, deleted files, metadata, hashes (sleuthkit, testdisk, foremost, binwalk, exiftool, yara...) |
| `network` | lookups, routes, throughput, captures (whois, traceroute, mtr, iperf3, socat from WormOS; dig, tshark, ngrep...) |
| `blueteam` | malware scanning and audits (clamav, lynis, chkrootkit, rkhunter, yara...) |
| `reverse` | disassemblers and debuggers (rizin and its window Cutter, gdb, ltrace, upx, pwntools...; radare2 is in neither Debian 13 nor Kali) |
| `web` | web application scanners and fuzzers (sqlmap, gobuster, ffuf, feroxbuster...) |
| `passwords` | crackers and hash tools (john, hashcat, hydra, medusa...) |
| `pentest` | from Kali: Metasploit, exploit-db, NetExec, Impacket, Responder... |
| `windows` | Windows programs: pefile, LIEF, YARA, oletools, Detect It Easy, rizin, Wine (run samples with `bh open --vm --from debian`) |

Each tool comes from the first source that has it (Debian, then Kali) and
runs in that distribution's wormhole, which shares your home folder: your
files are the kits' workspace, and the base system stays untouched. A tool
no source has is reported and skipped. Tools that are WormOS packages
(most of `basic`, a few of `network`) are installed last, all in one
change with one quick restart (also when several kits are installed at
once), and removing the kit keeps those that were
there before it. Open samples you do not trust in a
blackhole, not with a kit's tools on your files.

Use these tools only on systems and data you are allowed to examine.
