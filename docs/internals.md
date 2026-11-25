# WormOS internals

How the build, the boot and the system tools fit together. Build commands are
in [building.md](building.md); what each feature does for the user, in the
[user guide](guide.md).

## Toolchain and packages

- `scripts/build-toolchain.sh` builds the cross toolchain for
  `x86_64-wormos-linux-gnu` into `build/tools`, with target headers and
  libraries in `build/sysroot` (binutils 2.47, gcc 15.3.0, glibc 2.44, Linux
  6.18.57 headers). Finished steps are stamped in `build/stamps/`.
- `scripts/build-kernel.sh` builds Linux 6.18.64 LTS (the toolchain keeps
  the 6.18.57 headers): x86_64_defconfig with
  `kernel/wormos.config` merged on top, out of tree in `build/kernel`, staged
  as the package `build/pkgs/linux`. The build warns about any option in the
  fragment that did not survive dependency resolution. `kernel/patches/*.patch`
  are applied to a freshly unpacked tree. There are none now: up to 1.10,
  `acpi-fixed-buttons.patch` gave the fixed-feature power and sleep buttons
  (QEMU's, most PCs') a platform device, since 6.18.57's button driver is
  a platform driver and was never bound to them; 6.18.64 does the same
  itself (`acpi_default_enumeration` in `acpi_bus_scan_fixed`).
- Kernel security: nftables only (built in, no iptables or xtables), lockdown
  forced to integrity mode, LSMs `lockdown,yama,integrity,landlock` (AppArmor
  is built but only enabled with `lsm=`), and modules must be signed with the
  key generated in `build/kernel/certs`, which is never packaged. KVM, vhost
  and vsock serve later microVM blackholes; virtio-mmio and virtio-fs let the
  same kernel run inside one. `/dev/mem`, `/proc/kcore`, kexec and
  hibernation are compiled out. The stack-erase GCC plugin needs the host's
  GMP and MPC headers.
- `tools/wbuild NAME` builds `recipes/NAME/recipe` (PKGBUILD-like) into
  `build/pkgs/NAME`, with metadata in `build/pkginfo/NAME.info`. It copies each
  package's `usr/include` and `usr/lib` into the sysroot, so `PACKAGES` in the
  Makefile is in build order. Recipe fields include `release`, `depends`,
  `caps`, `groups`, `users` and `sysroot_files`.
- Build cache (`build/cache`): wbuild skips a package whose key is
  unchanged. `NAME.key` hashes the build scripts and toolchain
  (`build_env_hash` in `env.sh`: wbuild, env.sh, build-toolchain.sh,
  toolchain/sources), the recipe directory, the `localsrc` tree, files of
  this repository the recipe names (`$WORMOS_ROOT/...`), and the output hash
  (`NAME.out`, `tree_hash` of `build/pkgs/NAME`: names, modes, link targets,
  contents) of every package it builds against: its `depends` plus
  `NAME.deps`, found after each build from the pkg-config modules it asked
  for (a `pkg-config` wrapper logs them to `NAME.pclog`) and the libraries
  its files link against (readelf NEEDED), mapped to packages through
  `build/pkginfo/*.files`. glibc, gcc-libs and linux-headers count through
  the toolchain hash. A dependency that lies outside both (a header-only
  package found without pkg-config) must be listed in `depends`. Detected
  packages that come later in the same run are ignored: the sysroot still
  holds them from earlier runs (optional features such as pixman's gtk
  demos or Mesa's lua probe find them), but a build from scratch cannot
  use them. Declared `depends` must come earlier in `PACKAGES`.
  Builds are reproducible enough for this: wrappers make `ar` and `ranlib`
  deterministic (`D`), so a rebuilt dependency with the same files does
  not rebuild what uses it. The kernel has its own key (version,
  build-kernel.sh, kernel/wormos.config, toolchain). `WBUILD_FORCE=1`
  rebuilds anyway; `make clean` drops the cache.
- Autotools recipes must pass `--build="$(gcc -dumpmachine)"`; otherwise
  configure runs our cross binaries' host tools against the target libc.
- Nothing shipped names the build machine. wbuild compiles through small
  wrappers in `build/host/cc` that add `-ffile-prefix-map` (sources appear as
  `/usr/src/NAME`, sysroot headers as `/usr/include`); the flags stay out of
  CFLAGS because some packages, such as OpenSSL, record their CFLAGS. wbuild
  also rewrites sysroot paths in a package's text files
  (pkg-config files, `*-config` scripts) to plain target paths. wpack refuses
  a package in which any file still contains the repository path or the
  builder's home directory. Recipes whose configure scripts add run paths or
  record their command line (unbound) fix that themselves.
- `tools/wpack` writes `out/repo/NAME-VER-REL.wpkg`: a zstd pax tar with
  `.PKGINFO` first, stripped, root-owned, `go-w`.
- `tools/wrepo` packs everything, writes `out/repo/index` (with a `date`) and
  signs it with `build/host/wsign` (Ed25519, Monocypher). The secret key lives
  outside the repository in `~/.config/wormos/core.key`; the public key is
  `keys/core.pub`.
- All upstream sources were GPG-verified once; their SHA-256 sums are pinned in
  `toolchain/sources` and in the recipes.

## Image

- `scripts/build-rootfs.sh` assembles `build/fsroot/@` by installing the base
  `PACKAGES` with the host build of wh (`build/host/wh --root`) from
  `file://out/repo`, so the image has a complete `/usr/lib/wh/db`. It runs
  under fakeroot with state in `build/fakeroot.state`.
- `scripts/build-initramfs.sh` builds the initramfs with the kernel's
  `gen_init_cpio` (no root needed): wormrd, snap, busybox and the libraries
  they need (found via readelf NEEDED).
- `scripts/mkimage.sh` writes `out/wormos.img` without loop devices: GPT with
  an ESP (GRUB) and a btrfs partition holding `@`, `@var`, `@home`,
  `@snapshots` and `@wormholes`.
  - The image boots with UEFI and with BIOS firmware (SeaBIOS, VMware set
    to BIOS, VirtualBox's default). Partition 3 is a BIOS boot partition
    in the gap between the GPT's partition entries and the ESP (sectors
    40-2047, so partitions 1 and 2 keep their places); it holds GRUB's
    i386-pc `core.img`, and GRUB's boot sector is in the protective MBR
    (`scripts/bios-boot.py` does what grub-bios-setup does, on the image
    file). That GRUB finds the ESP by its FAT volume ID and reads the same
    `grub.cfg`.
  - It needs sudo for one step. mkfs.btrfs walks the tree with nftw, whose
    stat calls fakeroot cannot intercept. So mkimage dumps the owners fakeroot
    recorded (`find -printf` under fakeroot), copies the tree with sudo,
    replays owners and then modes with `scripts/apply-ownership.py` (which
    fails on any path without a record) and runs `sudo mkfs.btrfs` on the copy.

## Boot

- GRUB loads `/@/boot/vmlinuz` and `/@/boot/initramfs.img` with
  `root=UUID=<fs uuid> rootflags=subvol=@,compress=zstd ro`, kept in the
  exported GRUB variable `wormos_cmdline`.
- `src/wormrd/wormrd.c` is the initramfs `/init`. It finds the btrfs by UUID or
  label (reading the superblock itself), mounts the top level at `/top`, runs
  `snap --top /top boot [--rewind ID]`, mounts `@` read-only and switches root
  to worminit. A busybox rescue shell is included. It links the device it
  used as `/dev/wormos-root` (devtmpfs is one instance, so the link
  survives the switch to worminit's own `/dev` mount).
- `src/worminit/worminit.c` is PID 1. Services are key=value files in
  `/etc/worm/services/` with `type` oneshot, daemon or respawn.
- Subvolumes: `@` (root, read-only), `@var` (`/var`, not in system snapshots),
  `@home`, `@snapshots` (never mounted), `@wormholes`. `/root` is bind-mounted
  from `/var/roothome`. `/var`, `/home` and `/root` come from fstab, whose
  btrfs entries name `/dev/wormos-root` rather than `LABEL=WORMOS`: another
  disk with that label (a second WormOS disk, or an installed WormOS seen
  from the live ISO) must never be mounted instead. `10-filesystems` runs
  `/usr/lib/worm/mount-filesystems`, which makes the link from the root
  mount in `/proc/self/mountinfo` if it is missing (an initramfs older
  than the link), then `mount -a`.

## Live system and ISO

- `scripts/mkiso.sh` (`make iso`) runs `mkimage.sh` with `SHRINK=1` and
  `PARTS_DIR`: the btrfs is made with `mkfs.btrfs --shrink` (no free space)
  and the pieces are kept: `system.img`, `esp.img`, `head.img` (the first
  MiB of the disk image: protective MBR with GRUB's boot code, GPT,
  GRUB's `core.img` in sectors 40-2047), the fs UUID and the kernel
  command line. The ISO holds the kernel, the initramfs, `wormos/
  system.img.zst` (zstd; `--shrink` keeps whole allocated chunks, mostly
  empty, so the file is 1.2 GB raw and about half compressed),
  `system.size`, `esp.img.zst` and `head.img`. `grub-mkrescue` makes it a
  hybrid image (El Torito for BIOS and UEFI, MBR/GPT for USB sticks) with
  the volume ID `WORMOS_LIVE`; its menu boots `wormos.live` plus the
  installed system's command line without `root=`.
- `wormrd` with `wormos.live` scans block devices for an ISO 9660 primary
  volume descriptor with that volume ID (30 s for USB sticks), mounts it,
  unpacks `system.img.zst` with zstd (in the initramfs) into a file on a
  tmpfs sized to three quarters of the memory, skipping zero blocks, and
  extends the file to the tmpfs size (a hole). It attaches the file to a
  loop device (`LOOP_CTL_GET_FREE`, `LOOP_CONFIGURE`), unmounts the ISO and
  the tmpfs (the loop device keeps the file), grows the btrfs to the
  device (`BTRFS_IOC_RESIZE max`) and continues as on a disk. Discards
  reach the tmpfs, so deleted data frees memory. A soft reboot finds the
  device through `/dev/wormos-root`.
- `11-live` (`/usr/lib/worm/live-setup`) creates the user `live` (groups as
  the first-boot setup gives, no password), clears root's password (not on
  WormOS Amnesia, where root stays as the image has it), marks
  `/run/wormos/live` and puts a visible copy of the installer's desktop
  entry (`NoDisplay=true` in the system's) in the live user's
  applications. wormlogin then starts the live user's desktop without the
  greeter, every time; the session's autostart opens the installer.
- The installer: `wormos-installer` (`src/installer`, GTK 4) and the setuid
  backend `/usr/lib/worm/install-system` (`src/worm/install-system.c`),
  which refuses to run unless `/proc/cmdline` has `wormos.live` and the
  caller is root or in `wheel`. `list` prints whole disks of at least
  8 GiB that are not loop, RAM, CD, device-mapper or md devices, not the
  ISO and not mounted or used as swap. `install DISK` mounts the ISO again
  in a private mount namespace, opens the disk with `O_EXCL`, writes the
  protective MBR (GRUB's boot code from `head.img`), a fresh GPT (random
  disk and partition GUIDs, the layout of `mkimage.sh`, the root partition
  to the last MiB boundary before the backup table) and GRUB's `core.img`,
  the ESP and the system image (all written in full: stale data inside a
  FAT or btrfs image would be read as structure), re-reads the partition
  table (`BLKRRPART`) and grows the btrfs to its partition. The copy has
  the live system's fs UUID; the kernel mounts it under a temporary one
  (btrfs temp-fsid, single-device clones). The installed system starts
  with the first-boot setup.

## Editions

- `WORMOS_VARIANT` (standard, barebones, amnesia) picks the package list
  in `scripts/build-rootfs.sh`: the system (base, desktop, network,
  firewall, snapshots, packages, sandboxes, nano), plus the installer,
  Tor with Tor Browser and the everyday tools for standard; plus the
  installer for barebones; plus Tor, Tor Browser and `wormos-amnesia`
  (no installer) for amnesia. `wormos-desktop` no longer depends on Tor
  Browser, vim, tree, lsof or binutils (1.7), so systems upgraded from
  earlier releases keep them as installed packages.
- `/etc/worm/variant` names the edition (`VARIANT_ID`, `VARIANT`):
  written by build-rootfs for barebones, shipped by `wormos-amnesia`. The
  welcome text adds `VARIANT` to the name and prefers
  `/usr/share/wormos/guide-VARIANT_ID` for `welcome --full`; mkiso names
  the ISO after it (`out/wormos-VARIANT_ID.iso`) and its menu entries.
  `worm` has `variant_is()`.
- `make iso-barebones`, `make iso-amnesia`; `scripts/release.sh` builds the
  standard images first, then `VARIANTS` (default `barebones amnesia`),
  each as an ISO only.
- WormOS Amnesia (plan and promises: `docs/amnesia.md`):
  - mkiso adds `lockdown=confidentiality init_on_free=1
    page_alloc.shuffle=1 randomize_kstack_offset=on vsyscall=none` and
    leaves "install" out of the menu.
  - `wormos-amnesia` ships `/etc/worm/anon` (anonymous mode from the first
    start: `05-firewall` loads its rules before `30-network`, `16-anon`
    starts Tor), and `worm anon off` refuses there.
  - Root keeps the image's password (locked in releases), so nothing can
    mount a disk or change the system. `live-setup` adds the live user to
    the group `tor` instead, which can read `/run/tor` (the pid file and
    the notice log): `worm anon status`, which anyone may run, then
    reports Tor's progress; it tells a running Tor from a missing one with
    `kill(pid, 0)` (EPERM), since `/proc` hides other users' processes.
  - The look: the variant file sets `VARIANT_XDG=/etc/xdg/amnesia`,
    `VARIANT_GTK_THEME=WormOS-Amnesia` and `VARIANT_PANEL=bottom`;
    `session-env` turns them into `XDG_CONFIG_DIRS` (labwc's `rc.xml` with
    the theme `WormOS-Amnesia` and its own `autostart`, foot's `foot.ini`,
    GTK's `settings.ini` are found there first; labwc's `menu.xml` and
    `environment` still come from `/etc/xdg/labwc`), `GTK_THEME` and
    `WORM_SHELL_PANEL` (worm-shell anchors the panel at the bottom and
    opens its menus upwards). worm-shell loads
    `/usr/share/wormos/shell/variant.css` after its own CSS when it
    exists. The GTK theme is GTK's theme sources with
    `recipes/wormos-amnesia/files/_colors.scss`, built like
    `wormos-gtk-theme`; the wallpaper is `wallpaper.py --grey` (the WormOS
    one, desaturated with cairo's HSL saturation operator).
  - The autostart opens the welcome terminal at every start (the home
    folder is always new) and never the installer.
  - wormrd links the live boot medium as `/dev/wormos-medium`; Amnesia's
    `04-medium` service (`medium-watch`) polls `/sys/class/block/NAME`
    every second and runs `poweroff -f` when a USB stick goes away (not
    for `sr*`: a disc drive stays). `run-qemu.sh` with `USB_ISO` attaches
    an ISO as a USB stick (device `stick`) for the test.

## snap

- `src/snap/` mounts the top level in a private mount namespace. The store
  layout is documented at the top of `store.c`:
  `@snapshots/<id>/{info,system,home,system.manifest,home.manifest}` plus
  `pending`, `activate`, `trial`, `config`, `last-id`, `grub.cfg` and `.lock`.
- Manifests (`manifest.c`) record a SHA-256 and metadata per file; the manifest
  hash goes into `info`. Hashes are reused from the previous snapshot for
  unchanged inodes.
- `menu.c` writes `@snapshots/grub.cfg`, which the ESP grub.cfg sources (the
  "Rewind to a snapshot" submenu, `wormos.rewind=ID`).
- Soft reboot: a rewind writes `pending` and sends SIGHUP to PID 1. worminit
  kills everything, unmounts all but `/proc`, `/sys` and `/dev`, and execs
  `wormrd --soft-reboot`, which runs `snap boot` and `pivot_root`s into the new
  `@`. The kernel keeps running.
- worminit restarts a respawning service a second after it exits (after
  five exits within two seconds of starting, 30 s): a login that ends
  hangs its terminal up, which the kernel completes a moment later, and a
  getty restarted at once on the same port could open it first and stay
  silent.
- Shutdown (power off, halt, reboot; not a soft reboot): worminit stops
  every process, then runs each executable in `/etc/worm/shutdown.d` in
  name order (30 s each at most, then killed), then syncs and unmounts.
  WormOS Amnesia wipes the session's data there.
- Staging (`stage.c`): `snap stage begin|commit [--try]|abort|keep|status`
  works on `@.next`.
- `snap list --raw` prints one tab-separated line per snapshot for
  programs: ID, Unix time, scope, type, pinned, suspect, comment.
- `status.c` keeps a summary readable by everyone in `/run/snap-status`
  (`count`, `last`, `last_date`, `last_comment`, `auto`, `pending`),
  rewritten whenever the boot menu is and by `snap status`, which the
  service `11-snap-status` runs at boot. It is not written with `--top`.

## wh

- Recommends (since 1.8): a recipe's `recommends=(...)` becomes a
  `recommends =` line in the package and the index (older wh ignores it).
  When a package is installed or upgraded, `resolve` adds each package it
  recommends that is not installed, not declined and in the index; unlike
  depends, `wh remove` does not check them. Removing a package records it
  in `/usr/lib/wh/declined` (part of the system, so snapshots and undo
  cover it); installing it explicitly takes it off again.
- Live changes (since 1.9): with `/etc/wh/live` (an edition's package
  ships it; WormOS Amnesia does), install, remove and upgrade go straight
  into the running system: `stage_begin` remounts `/` writable (bind
  remount), `stage_commit` runs the hooks, records the history and makes
  `/` read-only again; no snapshot, no soft reboot. Files are replaced by
  rename, so running programs keep their old copies.
- Users (since 1.9): with `/etc/wh/users-may-install`, `wh install`,
  `reinstall`, `remove`, `update` and `upgrade` run by a user, and `worm
  kit install|remove`, hand over to `/usr/lib/wh/user` (`src/wh/user.c`,
  setuid root). It refuses without the flag, accepts only those commands,
  package, kit and source names (`[a-z0-9+.:-]`), replaces the environment
  and runs `wh --yes ...` or `worm kit ...` as root, logging who asked;
  installs, upgrades and kit installs run `wh update` first (the system's
  index may be as old as its image; WormOS Amnesia's always is). Amnesia's
  image index is re-signed with `date = 0` by `scripts/build-rootfs.sh`: it
  is built after the published index, which wh would otherwise refuse as
  older.
  Packages still come only from the signed repositories, so users can add
  programs but write nowhere else.
- Editions: `wh upgrade` also installs the edition's package when it is
  missing and not declined: `wormos-standard` without `/etc/worm/variant`,
  `wormos-VARIANT_ID` otherwise, if the index has it. `wormos-standard`
  recommends the standard edition's programs; each release that adds
  programs bumps it. Systems from before 1.8 upgrade with their own wh,
  which knows neither, so their second `wh upgrade` brings the programs
  (the welcome text says so while the package is missing).

- Downloads (since 1.11): `repo_fetch_all` fetches every package of a
  plan with one parallel curl (`download_many`) and checks each against
  the index; once the plan is installed, `repo_remove_downloads` deletes
  them, and each new download first clears what earlier changes left in
  `/var/cache/wh` (earlier releases kept every package downloaded).
- Deltas (since 1.11): an index entry may carry
  `delta = FROM-VERSION FILE SIZE SHA256`. When the installed version is
  FROM, wh downloads FILE, a `.wdelta`: a package of the new version
  (`tools/wdelta OLD NEW OUT`) whose `.PKGINFO` lists every file and its
  SHA-256 plus `delta-from = FROM`, but whose archive leaves out the
  regular files whose content and mode did not change. Before staging,
  `pkg_delta_usable` checks that each left-out file is still the one the
  installed version owns (same SHA-256, owned by root); if not, the full
  package is downloaded. `pkg_install` takes a left-out file as installed
  (after checking it again in the staged root). `scripts/make-deltas.sh`
  makes the deltas when the repository is published, against the
  published version, and drops those that save less than a third. Older
  wh ignores the line.
- Unchanged files (since 1.11): on an upgrade, a file the old version
  installed whose content, mode and owner are what the new version ships
  is left in place (`unchanged_file`): the package's copy is still read
  and checked, but nothing is written, so the file keeps sharing its
  blocks with the snapshots and no hook runs for it.
- `src/wh/`: `wh.c` (commands, transactions), `repo.c` (index, signatures,
  downloads via curl), `db.c` (installed database), `pkg.c` (tar reader using
  openat2 `RESOLVE_IN_ROOT`, capabilities, groups), `sig.c` and `wsign.c`
  (Ed25519). SHA-256 is shared with `src/snap`.
- Transactions: `snap stage begin` → wh mounts `@.next` privately at
  `/run/wh/next` → install → `snap stage commit [--try]` (prints the
  before-snapshot ID) → SIGHUP to PID 1 → `snap boot` renames `@.next` to `@`.
  Downloads and history use the live `/var`. With `--try`, the next boot rolls
  back unless `snap stage keep` was run.
- Hooks (`hooks.c`) rebuild caches that several packages contribute to.
  A package providing the tool ships `usr/share/wh/hooks/NAME.hook` with
  one or more `path =` lines and an `exec =` line. After a transaction
  that installed or removed anything beneath a path (or installed the hook
  itself), wh runs exec in the changed root, before committing it: words
  starting with `/` are taken inside the root, and the program runs through
  the root's own loader (`ld-linux-x86-64.so.2 --library-path ROOT/usr/lib`),
  so the same hook works for a staged system and for an image assembled on
  the build machine (without `LD_PRELOAD`, which the root's loader cannot
  satisfy; build-rootfs gives the files their owner afterwards). Hooks:
  `gsettings-schemas` (glib), `mime-database` (shared-mime-info),
  `desktop-mime` and `keep-accounts` (wh itself, below).
- `desktop-mime` (since 1.13) runs `/usr/lib/wh/mime-cache
  /usr/share/applications` (`src/wh/mime-cache.c`), which writes
  `mimeinfo.cache` from the entries' `MimeType` lines as
  update-desktop-database does: GLib finds the programs that open a type
  of file or link only through that cache, so before it `gio open`, "open
  with" and links clicked in programs found nothing (not even Tor Browser
  for https). wh runs it for `/usr/local/share/applications` after each
  wormhole install, removal and upgrade.
- base-files ships `/etc/passwd`, `/etc/group` and `/etc/shadow` with the
  system accounts only, so upgrading it used to replace a running system's
  accounts (users, root's password, accounts added by packages and the
  first-boot setup). The `keep-accounts` hook (`src/wh/keep-accounts.c`,
  `/usr/lib/wh/keep-accounts`, paths `etc/passwd`, `etc/group`,
  `etc/shadow`) writes the running system's version of each file into the
  staged root, followed by the staged entries whose names the running
  system lacks. It does nothing unless the running system is WormOS (on
  the build machine `/etc/os-release` says otherwise). Since 1.3's wh
  already runs hooks, the wh package of a newer release brings the hook
  along in the same upgrade.
  Caches that belong to one package's own folder, such as the Adwaita icon
  cache, are built when the package is built instead.
- A path owned by another package is a conflict, with one exception: a
  symlink to busybox owned by the `busybox` package (an applet link) is
  taken over by a package that installs the full program there, and wh
  drops the path from busybox's file list (`db_disown`). The busybox
  recipe leaves out the applets other packages replace (`REPLACED`: xz,
  bzip2, less, tree, lsof, libarchive's tar/cpio/unzip, vim's vi and xxd,
  binutils' strings); `wormos-desktop` depends on those packages. wh
  plans an upgrade in name order, so on a system that had the links
  (1.3), busybox is upgraded first and drops them, then the replacements
  install; a newer wh would also take the links over if the order were
  reversed.
- Since 1.7 the other direction works too, so full versions of BusyBox
  commands can come and go without `REPLACED` (GNU grep, sed, gawk,
  findutils, diffutils, patch, coreutils, iproute2, wget, dosfstools,
  whois, traceroute...): a new busybox skips an applet link whose path
  another package owns (`busybox_yields`), and removing a package gives
  busybox back the links it took (`busybox_restore`): busybox ships the
  list of its links in `/usr/share/busybox/links`, and wh recreates each
  removed path in that list as a link to busybox and adds it to busybox's
  file list (`db_adopt`). Paths still in `REPLACED` stay out of busybox,
  because 1.6's wh, which runs the upgrade to 1.7, fails on a link owned
  by another package.

## Wormholes

- `src/wh/foreign.c` and `src/wh/whc.c`. Sources are listed in
  `/etc/wh/sources` (shipped by the wh recipe): Debian trixie (main, updates,
  security) and kali-rolling.
- Setup is a small debootstrap of our own:
  1. Check InRelease with `gpgv --output X.part` and rename after success
     (GnuPG 2.5's gpgv never renames its own `.part` output).
  2. Fetch Packages (xz, then gz, then plain; Kali has no xz) and check it.
     The list is fetched once per run (finding a package and setting up its
     wormhole share it) and the copy from the last run is kept while it
     matches the signed checksum (`Packages.sha256` records which one the
     plain list came from).
  3. Select `Priority: required`, `Essential: yes` and apt, with dependencies.
  4. Download the missing `.deb`s with one curl (`--parallel`, six at a
     time over shared connections), check each, and unpack it with
     `tar_unpack` after creating the merged-/usr links.
  5. Run the dpkg second stage inside via `whc --root --dir --bind`.
  6. Copy the checked InRelease and Packages into apt's lists under apt's
     names (mirror without scheme, `/` as `_`), so `apt-get update` does
     not download them a third time, then run it.
- apt inside is pinned to our keyring copy (`signed-by`), recommends and
  translations (`Acquire::Languages "none"`) are off, manual pages and
  locales are excluded, dpkg runs with `force-unsafe-io` (no fsync per
  file; a broken wormhole is set up again rather than repaired) and
  `policy-rc.d` exits 101. Documentation folders are kept since 1.13:
  Kali's impacket-scripts run the scripts in
  `/usr/share/doc/python3-impacket/examples`, so all 60 impacket commands
  failed.
- The app runtime (`src/wh/runtime.c`, since 1.13), so that programs work
  without fixes of their own. `wormhole_runtime` runs with every repair
  (before each install, removal and upgrade), inside the wormhole:
  it writes the dpkg settings, the stand-ins below and, once for wormholes
  from before 1.13 (their settings still exclude `/usr/share/doc`), installs
  again the packages missing a file under `/usr/share/doc` that is a script
  or an example. `runtime_complete` runs after each install and upgrade:
  the rules in `/usr/share/wh/runtime` (`PATTERN = PACKAGE...`, from
  `src/wh/runtime`) add companions of installed packages that apt would
  only recommend: `librsvg2-common` and `adwaita-icon-theme` for GTK
  (icons are SVG; without the loader GTK programs draw none),
  `qt6-wayland`/`qtwayland5` and the SVG plugins for Qt. A companion the
  distribution lacks is skipped. A new kind of breakage is fixed with a
  line there, not in wh.
- Stand-ins (`/usr/local/lib/wormhole/bridge` inside each wormhole, linked
  as `sudo`, `pkexec`, `xdg-open`, `sensible-browser`, `x-www-browser`,
  `www-browser`, `gnome-www-browser` and `x-terminal-emulator` in its
  `/usr/local/bin`, which comes first on whc's `PATH`): a program that
  reaches out of its wormhole writes a request into the session's bridge,
  `$XDG_RUNTIME_DIR/wormhole-bridge` (or `$WORMHOLE_BRIDGE`, which the
  root terminal sets to the user's), a FIFO read by `/usr/lib/wh/bridge`
  (`src/wh/bridge.c`), which the session script starts as the user.
  Requests are NUL-separated fields ending with an empty one (dash's
  `printf '%s\0'`), written under `timeout 5` (nobody reading would block
  the program): `open WORMHOLE FOLDER TARGET` runs `gio open` (relative
  names are taken from FOLDER), `terminal WORMHOLE FOLDER [CMD]` runs
  `foot -- whc WORMHOLE CMD`, `root WORMHOLE FOLDER [CMD]` opens the root
  terminal (`root-terminal --cd FOLDER "whc WORMHOLE CMD"`, which asks for
  the root password). The bridge logs each request to
  `$XDG_RUNTIME_DIR/bridge.log`. Run as root (from the root terminal),
  `sudo` and `pkexec` just run the command (`runuser` for `-u USER`);
  without a session the stand-ins fall back to the distribution's own
  program or say why they cannot. The FIFO is 0600, so only the user and
  root can ask, and nothing runs with more rights than the user has.
- Repair (since 1.11.1): before every install, removal and upgrade,
  `wormhole_repair` recreates the empty man page folders (`man1`..`man9`,
  `info`; documentation stays excluded, but maintainer scripts link into
  them: update-alternatives fails without `man1`, so openjdk, and Ghidra
  with it, did not configure) and runs `dpkg --configure -a` and
  `apt-get -f install` for whatever an earlier change left half done. A
  failed install is tried once more after `apt-get update` and a repair.
- `NAME:i386` packages (32-bit, such as `wine32:i386`): `foreign_find`
  picks the first source without looking (its lists are amd64 only), and
  `foreign_install` runs `dpkg --add-architecture i386` and `apt-get
  update` in the wormhole once, when `dpkg --print-foreign-architectures`
  does not list i386 yet.
- `wh search` asks apt in an existing wormhole (`apt-cache search
  --names-only`). For a source without one it reads the checked
  `Packages` list in `/var/cache/wh/sources/NAME` (name matches only, like
  apt); root's search downloads a missing list first and `wh update`
  refreshes lists that exist. Those downloads run in a child process
  (`try_fetch`), so a failure (offline, a cut network) only skips the
  source. A user's search names the sources it could not search. `--from`
  restricts the search to one source, `core` included.
- wh's `run()` reads a program's output to the end even when it keeps only
  the beginning: closing the pipe early killed `apt-cache show` with
  SIGPIPE, so a package in an existing Debian wormhole looked missing and
  wh set up the Kali wormhole for it.
- Exported commands (since 1.13): those of the package and of the
  packages it depends on that come from the same source package
  (`same_source`: vlc's commands are in vlc-bin, keepassxc's in
  keepassxc-full, pev's in readpe; transitional packages and metapackages
  have no files). The record lists them (`packages =`). A file in a
  command folder counts when it is an executable regular file after links
  are followed inside the wormhole (`stat_inside`, `openat2` with
  `RESOLVE_IN_ROOT`): Debian's alternatives (`/usr/bin/nc ->
  /etc/alternatives/nc -> nc.openbsd`) used to resolve on the host and
  were skipped. A system command still wins, except a link to busybox
  (`system_command`): Debian's `ipcalc` or `nc` go in front of BusyBox's
  on users' `PATH` (`/usr/local/bin` first); services, whose `PATH` lacks
  `/usr/local/bin`, keep BusyBox's. The record also holds the package's
  summary (`description =`). A library package (`python3-*`, `*-perl`,
  `ruby-*`) without commands gets a hint to use it through `whc`.
- Besides commands, wh exports a package's desktop entries (to
  `/usr/local/share/applications`, unless the system has an entry of that
  name) and icons (`usr/share/icons` and `usr/share/pixmaps`, to the same
  places under `/usr/local/share`). An entry is only exported when its
  program is one of the commands just exported; it keeps only the
  `[Desktop Entry]` group, its `Exec` names the wrapper, and `Actions`,
  `TryExec` and `DBusActivatable` are dropped. Files are read inside the
  wormhole with `openat2` (`RESOLVE_IN_ROOT`), regular files of at most
  8 MiB only, and recorded with the commands, so removing the package
  removes them. `Exec=env VAR=VALUE PROGRAM` (Audacity's) is matched on
  PROGRAM and keeps the settings.
- A package with commands but no entry of its own and none from Kali's
  menu (hexyl, tshark, nasm, dig) gets `wormhole-PACKAGE.desktop`: its
  summary as the comment, its commands as keywords, and
  `exec-in-shell --about PACKAGE`, which shows the package, its summary and
  commands (from the record) in a terminal, then the user's shell.
- Kali keeps the menu entries of its tools, and of Debian's tools it
  carries, in `kali-menu`, not in the tools' packages. The `tool-menus`
  package (built from kali-menu's source archive) has them in
  `/usr/share/wormos/tool-menu/applications`, their icons in hicolor, and
  `sections` (generated from `kali-applications.menu`: every category to its
  top-level section, `CATEGORY = ORDER|NAME|ICON`). A package from any
  wormhole gets the entries whose `X-Kali-Package` names it, when their
  program is one it exported: `exec-in-shell "CMD"` entries become
  `/usr/lib/worm/exec-in-shell "CMD"` (prints the command, runs it, stays in
  the user's shell), `sudo CMD` and `pkexec PROG` become
  `exec-in-shell --root` (the root terminal's password prompt, then CMD as
  root), others keep their command. Field codes are dropped from terminal
  commands. Each gets `X-WormOS-Section`. When the package exported its own
  entries, Kali's for windowed programs are skipped. Entries running one of
  Kali's helper scripts (`/usr/share/kali-menu/helper-scripts/john.sh`,
  `metasploit-framework.sh`...; tool-menus ships them in
  `helper-scripts/`) become `exec-in-shell [--root] --in WORMHOLE --script
  NAME`, which runs the script's text inside the wormhole (`whc WORMHOLE
  sh -c`), where the files it lists are; a script using `sudo` or
  `pkexec` runs as root (the stand-ins then just run the command).
- `sudo` (`/usr/bin/sudo`, wormos-desktop, since 1.13) is the root
  terminal's path for one command: `root-terminal --once --cd $PWD
  "exec 'CMD' 'ARGS'..."` (arguments quoted for sh), so it asks for the
  root password, keeps the display and returns the command's status. Run
  by root it just runs the command. With `/etc/worm/no-root` (an edition without an
  administrator ships it: WormOS Amnesia, whose root is locked), `sudo` and
  the root terminal say so instead of asking for a password that cannot
  work (the root terminal waits for Enter when it has a terminal, so a
  wormhole program's root request shows why it failed).
- The root terminal runs `/usr/lib/worm/root-terminal [--once] [--cd FOLDER] [COMMAND]`: `su - root -c` running `root-shell` with
  the Wayland socket's full path and `DISPLAY` handed over, and
  `XDG_RUNTIME_DIR=/run/user/0`, so graphical programs open from it
  without root writing into the user's runtime folder. Xwayland admits
  only the user running it (no cookie file), so the script first runs
  `/usr/lib/worm/x11-allow-root` as the user, which adds
  `si:localuser:root` to the server's access list (ChangeHosts, as `xhost`
  would); Java programs such as Ghidra failed with "Authorization required".
- Containers live in subvolumes under `/var/lib/wormholes` (`@wormholes`,
  mounted `nosuid,nodev`, mode 0755). Exported commands are wrappers in
  `/usr/local/bin` (a link to `/var/usrlocal`) and never shadow `/usr/bin` or
  `/usr/sbin`. Records are in `/var/lib/wh/foreign/PKG`. Foreign installs are
  live, with no restart.
- `whc NAME CMD`: for a normal user, a user namespace mapping only their uid
  plus a mount namespace; for root, a mount namespace only (helpers in
  `src/common/ns.c`). It shares `/dev`, `/proc`, `/sys`, `/tmp`, `/home`,
  `/run`, resolv.conf, hosts, passwd and group (user mode) and the host
  network. `$SHELL` becomes `/bin/sh` when it names a shell the container
  lacks (WormOS's zsh), or `script`, `make` and editors could not run
  commands. Since 1.13 it also shares `/mnt` and `/media`, and `/root`
  when run by root (programs from the root terminal saw the container's
  `/root` and missed root's files); started from a folder that is not
  shared, it says so on stderr. Programs get the same environment whatever
  their toolkit (`app_environment`): `QT_QPA_PLATFORM=wayland` becomes
  `wayland;xcb`, `GDK_BACKEND` and `SDL_VIDEODRIVER` `wayland,x11` (every
  Qt program from Debian quit: qt6-wayland is only recommended),
  `_JAVA_AWT_WM_NONREPARENTING=1` (blank Java windows under labwc's
  Xwayland) and `NO_AT_BRIDGE=1` (no accessibility bus). The session sets
  the same fallbacks for the system's programs.

## Hardening and privacy

- Services in order: `01-sysctl` (`sysctl -p` on `/usr/lib/sysctl.d/*.conf`,
  then `/etc/sysctl.d/*.conf`), `02-loopback`, `05-firewall`,
  `10-filesystems`, `15-dns` (shipped by the unbound package), `16-anon`
  (tor), `25-net-privacy`, `30-network`. Loopback comes first (it was
  `20-loopback` before 1.7): Tor, started once by `16-anon`, gave up when
  it could not listen on 127.0.0.1, so anonymous mode came up after a
  restart with Tor not running and nothing getting out.
- Kernel command line (`scripts/mkimage.sh`): `slab_nomerge debugfs=off
  iommu.passthrough=0 iommu.strict=1 efi=disable_early_pci_dma`.
- worminit mounts `/proc` with `hidepid=invisible` and `/dev`, `/run` and
  `/dev/shm` `noexec`. `/tmp` stays executable.
- `/usr/lib/worm/net-privacy` gives every device with a `device` link in
  `/sys/class/net` a random locally administered unicast MAC and sets its
  `use_tempaddr` to 2 (the sysctl defaults only reach devices created
  later). On VMware and Hyper-V guests (DMI `sys_vendor` `VMware, Inc.`,
  or `Microsoft Corporation` with product `Virtual Machine`) it keeps the
  MAC: their virtual switches deliver frames only to the address they
  assigned, so a random one leaves the guest without a network. vmxnet3
  cards keep their MAC everywhere (they ignore a new one, also in QEMU).
- `/usr/lib/worm/dhcp-event` replaces busybox's udhcpc script. It records
  the announced DNS servers in `/run/worm/dhcp-dns` and, when unbound is
  installed, keeps `/run/resolv.conf` at 127.0.0.1 and ::1. Renewals use
  `ip addr replace` (the address is already there).
- unbound (`recipes/unbound/files/unbound.conf`) listens on loopback only,
  runs as the `unbound` user, forwards everything over TLS to Quad9 and
  validates DNSSEC with `/var/lib/unbound/root.key` (RFC 5011 updates),
  seeded from `/usr/share/unbound/root.key` (KSK-2017 and KSK-2024 DS
  records from IANA).
- Recipes can declare `users=(NAME)`: wh adds a system group and user with
  a locked password, no home and nologin (IDs from 999 down).

## Anonymous mode

- `worm anon` (`src/worm/anon.c`). The switch is `/etc/worm/anon` (part of
  system snapshots). `on` writes it (bind-remounting `/` if locked),
  reloads the firewall, then starts Tor; `off` removes it, reloads the
  firewall and stops Tor. At boot the firewall service loads the anonymous
  rules before the network, and `16-anon` (`worm anon start`, from the tor
  package) starts Tor once `/var` is mounted.
- Tor (`recipes/tor`) is built as a client only (`--disable-module-relay`),
  without GeoIP data. `/etc/tor/torrc`: user `tor`, data in `/var/lib/tor`,
  pid and notices log in `/run/tor`, SocksPort 9050, TransPort 9040
  (`IsolateDestAddr`: one circuit per destination address),
  DNSPort 5353 (IPv4 and IPv6 loopback), .onion names automapped into
  10.192.0.0/10. `worm anon status` reads the last "Bootstrapped N%" line
  of the log.
- Rules (`write_anon()` in `fw.c`), added to the `inet worm` table: an
  output filter (policy drop) accepting `oif lo`, packets the NAT chain
  redirected (`ct status dnat` to 127.0.0.1 or ::1; after a redirect `oif`
  still names the original device), Tor's UID for TCP only and root's DHCP
  renewals (`meta skuid 0`, source port 68 to 67), rejecting the rest (TCP
  with a reset). There is no `ct state established` rule, so connections
  opened before the switch are cut; and a NAT output chain
  returning Tor's packets, redirecting UDP port 53 to 5353 and every other
  IPv4 TCP connection outside 127.0.0.0/8 to 9040. The resolver's own
  upstream connections are redirected like any other. Without a `tor`
  user nothing is accepted or excepted, so the machine fails closed. A cut
  network (panic, safe mode) replaces the whole table and wins.

- `worm anon start` (the `16-anon` service) does nothing while
  `/run/worm/anon-hold` exists: an edition's start screen (WormOS Amnesia)
  first asks how Tor should connect, then removes it and starts Tor, so a
  network that blocks Tor never sees a direct attempt.
- Bridges (`worm anon bridges KIND`, `anon.c`): `/etc/tor/torrc.d/bridges.conf`
  (the torrc ends with `%include /etc/tor/torrc.d/`, an empty directory in
  the tor package) gets `UseBridges 1`, `ClientTransportPlugin
  meek_lite,obfs4,webtunnel,snowflake exec` Tor Browser's `lyrebird`, and
  one `Bridge` line per bridge: the defaults from
  `/usr/share/tor/bridges/KIND` (written when tor-browser-bundle is built,
  from `pt_config.json` in the browser's `omni.ja`) or lines from standard
  input (`custom`; control characters refused, an optional `Bridge `
  prefix dropped). A first comment line names the kind. The firewall
  reloads and a running Tor restarts. In anonymous mode with bridges, the
  output chain also accepts TCP to port 853 from the unbound user (the
  transports resolve their servers' names through it before Tor is up;
  Tor's own user is left out of the DNS redirect), and with snowflake UDP
  from Tor's user.

## Tor Browser

- `tor-browser` (`src/torbrowser/`, GTK 4, `org.wormos.TorBrowser`),
  `tor-browser-runtime` and `tor-browser-bundle` are in the image (the
  desktop depends on all three). Tor Browser's `libxul.so` needs GTK 3,
  ATK and the X11 client libraries (libX11, libX11-xcb, libxcb,
  libxcb-shm, libXext, libXfixes (since 16.0), libXrandr), even on
  Wayland.
- `tor-browser-bundle` is the release archive from dist.torproject.org
  (checksum pinned in the recipe, signature checked when the version was
  pinned), unpacked untouched to `/opt/tor-browser` with a `version`
  file: outside `/usr`, so wpack does not strip it and wbuild does not
  copy its libraries into the sysroot. When the user's copy is missing,
  the launcher copies the bundle into `~/.local/share/tor-browser/tor-browser`
  (its own recursive copy with `copy_file_range`, which shares extents on
  btrfs) and writes the version file; the browser updates that copy
  itself. A newer bundle in a later image does not replace an existing
  copy (it would lose the profile; the copy keeps itself current).
- The browser never runs as root. Started by root, the launcher takes the
  owner of its terminal (`ttyname(0)`; `su -` keeps the user's pty), copies
  the environment of one of that user's processes that has
  `WAYLAND_DISPLAY` (from `/proc/PID/environ`), changes to the user
  (`initgroups`, `setgid`, `setuid`, and checks that `setuid(0)` fails)
  and starts itself again. Without such a terminal or session it explains
  and exits 1. A user without a Wayland display (no `WAYLAND_DISPLAY` and
  no `$XDG_RUNTIME_DIR/wayland-0`) gets a message instead of GTK's
  "Failed to open display".
- `tor-browser-runtime` depends on `gtk3` (3.24, Wayland backend only; no
  X11 backend, so no cairo-xlib or accessibility bridge; Adwaita built in,
  dark, set in `/etc/gtk-3.0/settings.ini`), `atk` (at-spi2-core built with
  `atk_only`) and the X.org libraries (`libx11`, `libxcb`, `libxau`,
  `libxext`, `libxrender`, `libxrandr`; `xorgproto`, `xcb-proto` and
  `xtrans` only for building). libx11 leaves the Compose tables to
  `xcompose`. Package names are lowercase (wpack's rule).
- libxul is linked with immediate binding against fifteen `gdk_x11_*`
  functions (15.x also `cairo_xlib_surface_create`), which a Wayland-only
  GTK 3 lacks. `tor-browser-runtime` ships `/usr/lib/tor-browser/libx11-compat.so`,
  stand-ins that return zero, and the launcher preloads it. Firefox calls
  them only on an X11 display. A new Tor Browser release that needs more of
  them fails to start until the list grows (compare libxul's undefined
  symbols with the libraries, as when the list was made: `nm -D
  --undefined-only libxul.so` against `nm -D --defined-only` of the
  image's and the bundle's libraries; also compare `readelf -d` NEEDED,
  which is how 16.0's new libXfixes was found).
- First run on a system without the runtime (it was planned as an
  optional package): without the stand-ins the window offers to install the
  runtime through `/usr/lib/worm/install` (`src/worm/install.c`, setuid
  root): root or wheel only, root's password on standard input, and only
  packages that an installed package lists as an empty root-owned file in
  `/usr/share/worm/installable/` (tor-browser lists the runtime). It runs
  `wh update`, then `wh --yes --from core install NAME`, which restarts the
  system. Exit status 3 means a wrong password.
- Download (`--download` does the same without a window): the version
  from `aus1.torproject.org/.../download-linux-x86_64.json` (the older
  `downloads.json` is deprecated); the archive and signature
  URLs are built from it under `dist.torproject.org`, never taken from the
  list. A version older than `MIN_VERSION` in the source or than
  `~/.local/share/tor-browser/version` is refused. curl is limited to
  HTTPS. `gpgv --status-fd 1` must exit 0 and print a `VALIDSIG` line whose
  last field (the primary key) is `EF6E286DDA85EA2A4BA7DE684E2C6E8793298290`,
  with the keyring `/usr/share/wormos/tor-browser/tor-browser-developers.gpg`
  (the armoured key is in the recipe, from torproject.org's web key
  directory). busybox tar unpacks into `unpack/`, which replaces
  `tor-browser/`. `--check FILE SIG` runs only the signature check.
- Start: as Tor Browser's own `start-tor-browser` (a bash script, so not
  used): working directory and `HOME` are the bundle's `Browser` folder
  (`BB_ORIGINAL_HOME` keeps the real one), `Browser/.local/share` is a link
  to `/dev/null`, `GSETTINGS_BACKEND=memory`, output to `/dev/null`; then the
  bundle's `firefox` (a POSIX shell script) with `MOZ_ENABLE_WAYLAND=1`,
  `GDK_BACKEND=wayland`, `LD_PRELOAD` of the stand-ins and no `DISPLAY`.
- With `/etc/worm/anon`, the firewall lets only the `tor` user out, so the
  bundled tor could not connect: the launcher sets `TOR_PROVIDER=none`,
  `TOR_SKIP_LAUNCH=1` and `TOR_SOCKS_HOST`/`TOR_SOCKS_PORT` (127.0.0.1:9050),
  as Tails and Whonix do. There is no control port, so the circuit display
  is unavailable then. Otherwise the browser starts its own tor.
- The browser updates itself (signed MAR files) inside the user's folder.
  It runs like any program of the user, with Firefox's own sandbox
  (unprivileged user namespaces and seccomp); it is not in a blackhole.

## Panic and safe mode

- `worm panic` (`src/worm/panic.c`): `fw_cut()` writes the marker
  `/run/worm/network-cut` and loads a table dropping everything but
  loopback in input, output and forward; hardware devices go down (the
  DHCP script leaves them down while the marker exists). Then every
  running blackhole of every user is stopped: the bh holding its lock gets
  SIGKILL and its init follows through `PR_SET_PDEATHSIG`. The holder comes
  from `/proc/locks` (FLOCK entries with the lock file's inode; btrfs
  reports another device number there, so it is confirmed by a descriptor
  of that process on the same file), never from the `pid` file, which the
  blackhole's owner can write. It is only signalled when it belongs to the
  owner of the user's directory and is not PID 1; waiting for the lock is
  bounded. Root's blackhole cgroups get `cgroup.kill` and
  are removed. Last, `snap now --all --suspect` and `snap pin`.
- `worm panic off` removes the marker, reloads the normal rules, brings
  the devices up and sends SIGUSR1 to root's udhcpc (taking a device down
  dropped its routes).
- Snapshots marked `suspect = yes` in `info`: the "Before rewind" ones
  (`snap boot`) and `snap now --suspect` (panic).
- `menu.c` writes a safe mode entry before the "Rewind to a snapshot"
  submenu: the newest system snapshot not marked suspect, booted with
  `wormos.rewind=ID wormos.safe` (the current system before the first
  snapshot; the ESP grub.cfg has the same fallback when the store has no
  grub.cfg yet).
- With `wormos.safe`, worminit skips services marked `safe = no`
  (`25-net-privacy`, `30-network`) and `worm fw load` cuts the network. A
  soft reboot keeps the kernel command line, so safe mode lasts until a
  full restart.
- `worm status` (`status.c`) summarises the root mount, mode, snapshots
  (`snap list`, `snap auto`), staged package changes, firewall, the
  resolver, blackholes, lockdown and io_uring.

## Blackholes

- `src/bh/`: `bh.c` (commands, options, state), `box.c` (namespaces,
  init, sessions), `session.c` (bh's side of a session), `relay.c`
  (terminal and output relay), `sandbox.c` (Landlock, seccomp,
  capabilities), `limits.c` + `limits-helper.c` (cgroups), `net.c`
  (pasta, routes), `vm.c` + `vm-init.c` (virtual machines).
  `src/common/ns.c` holds the namespace helpers shared with `whc` (ID maps,
  binds that keep locked mount flags, pivot_root).
- State: `/var/lib/blackholes` is sticky and world-writable like `/tmp`;
  each user has `UID/` (0700, 0711 for root, refused unless owned by the
  caller) holding `NAME/` with `lock` (flocked by the bh that opened it),
  `pid`, `info` (outside ID, creation time), `mode` (what it was opened
  with, for `bh list`), `control` (the session socket, 0600), `rootfs/`
  and `inbox/`. Root's `NAME/` is `root:ID 0710`: the blackhole's ID
  reaches its root through the group, other users get nothing.
- IDs: root inside maps to one ID outside: the caller's own for a user,
  `0x40000000 + n` (allocated per blackhole, under a lock on `UID/`) for
  root. Root's supplementary groups are dropped first.
- Start: bh binds and listens on `control`, then `clone`s with new user,
  mount, PID, network, IPC and UTS namespaces; the parent writes the ID
  maps, puts the child in its cgroup (below), starts pasta with `--net`,
  then releases the child through a pipe. The child becomes root inside,
  re-arms `PR_SET_PDEATHSIG` (credential changes clear it), binds `rootfs`
  onto itself, binds `/usr` and `/etc` read-only (the host's, or
  `/var/lib/wormholes/SOURCE/usr` and `etc` with `--from`), writes the
  name server into its own `/run/resolv.conf` (base-files' `/etc/resolv.conf`
  points there; a wormhole's regular file gets it bound over), mounts
  `/proc`, a tmpfs `/dev` with nodes bound from the host, a devpts instance
  and `/dev/shm`, binds the inbox, pivots, unshares a cgroup namespace,
  brings up `lo`, adds the `--net` routes, sets `RLIMIT_NPROC`, makes
  itself non-dumpable, applies the sandbox, points its stdio at
  `/dev/null` and keeps only its two session sockets. Before the mounts,
  each mount point in the blackhole's root (`usr`, `etc`, `proc`, `dev`,
  `inbox`, `run`) is made a real directory again, so no mount follows a
  symlink left by an earlier session.
- Sessions: init serves requests (`struct request` in `bh.h`) on the
  socketpair with the bh that opened it and on `control`, where a later
  `bh open NAME` connects (`box_join`; options are refused then). A
  request carries the command, the terminal type and, for a terminal, its
  modes and size; without one, the program's three descriptors come along
  (`SCM_RIGHTS`). For a terminal, init opens a pseudo-terminal from the
  blackhole's devpts, forks the program as leader of a new session with
  the slave as its controlling terminal and sends the master back; bh
  relays (`relay.c`). Each program gets a clean environment, `/root` as
  its directory and the reduced capabilities. When a program exits, init
  sends its exit code; a connection that closes takes its program's
  process group with it. When the first session's program exits, init
  exits and the kernel kills everything in the PID namespace, joined
  sessions included. Up to 64 sessions at a time.
- Terminal (`relay.c`): bh puts the host terminal in raw mode, relays both
  ways (non-blocking master, input buffered so a large paste cannot
  deadlock) and passes `SIGWINCH` on, until the master reads EIO. Without
  a terminal as input (pipes, or a background job, which may not set raw
  mode or flush without SIGTTOU), input comes from `/dev/null` if it was a
  terminal, and output to a terminal goes through pipes that bh relays.
  Output is filtered on the way: operating system commands (OSC) pass
  only for titles (0, 1, 2), colours (4, 10, 11, 12, 104, 110 to 112),
  the working directory (7) and prompt marks (133), or without a number;
  the rest (52 clipboard, 8 links that open on the host when clicked, 9,
  99 and 777 desktop notifications, terminal extensions) are dropped up
  to their terminator. Everything else passes. At the end, input the terminal sent meanwhile
  (answers to queries from inside) is flushed so the host shell never
  reads it.
- Why no overlay of `/`: overlayfs clones its lower layer, and the kernel
  refuses to clone `/` from a user namespace because its submounts are
  locked. `/usr` and `/etc` have no submounts and can be bound alone. Host
  files are owned by unmapped IDs, so an overlay would not make them
  writable anyway.
- Sandbox: `PR_SET_NO_NEW_PRIVS`; a Landlock ruleset handling every file
  system right of the running ABI with one rule, everything beneath the new
  `/`, and from ABI 6 the scopes for abstract UNIX sockets and signals; a
  seccomp filter that kills non-x86_64 ABIs (i386, x32), refuses mounts,
  namespace calls (and `clone` with `CLONE_NEW*`; `clone3` gets ENOSYS so
  libc falls back to `clone`), keyrings, BPF, perf, io_uring, modules,
  kexec, reboot, swap, syslog, clock setting, iopl/ioperm, `modify_ldt`,
  fanotify, `pidfd_getfd`, `process_vm_writev`, file handles, the ioctls
  `TIOCSTI`, `TIOCLINUX`, `TIOCSETD` and `TIOCCONS`, and every socket
  family but `AF_UNIX`, `AF_INET`, `AF_INET6` and `AF_NETLINK` with
  `NETLINK_ROUTE` or `NETLINK_SOCK_DIAG` (no vsock, which network
  namespaces do not isolate, no packet sockets, no netfilter netlink);
  `socketpair` only for `AF_UNIX`.
- Capabilities (`sandbox_drop_caps`, in each program before exec): the
  bounding set, effective and permitted sets keep `CHOWN`, `DAC_OVERRIDE`,
  `FOWNER`, `FSETID`, `KILL`, `SETUID`, `SETGID` and `NET_BIND_SERVICE`
  (`CapBnd` 0x4fb); ambient ones are cleared. Root inside therefore lacks
  `CAP_NET_ADMIN` (tc, nf_tables, routes and interfaces in its namespace),
  `CAP_NET_RAW`, `CAP_SYS_ADMIN` and `CAP_SYS_PTRACE` (init is
  non-dumpable).
- Limits (`limits.c`): a cgroup `/sys/fs/cgroup/blackholes/UID-NAME` with
  `pids.max` 4096 (`--pids`; tasks, so threads count), `memory.max` half
  of the memory (`--memory`; with `--vm` the guest's memory plus 256 MiB),
  `memory.swap.max` 0 and `cpu.weight` 50 (half of everything else's;
  needs `CONFIG_CGROUP_SCHED`). Root writes it itself. A user cannot move
  a process between cgroups they do not own, so bh runs the setuid helper
  `/usr/lib/bh/limits create NAME PID MEMORY PIDS`, which only names
  cgroups after the caller's real UID, only moves a direct child of its
  caller that belongs to the caller (the child is waiting on bh, so its
  PID cannot be reused), clamps the values (64 MiB to all memory, 16 to
  65536 tasks) and ignores the environment; `remove NAME` deletes it
  (`cgroup.kill` first if something is left). `RLIMIT_NPROC` (same value)
  is the fallback; it is counted per user namespace and only
  `CAP_SYS_RESOURCE` in the initial namespace could raise it. Measured
  before 1.6: 1024 tasks capped one program at 1022 threads, too few for
  browsers, JVMs and parallel builds; 4096 still stops a fork bomb (4093
  `sleep`s in the test) well before the system notices.
- Network (`net.c`, `--net`): bh runs `pasta --config-net --ipv4-only
  --address 100.96.0.2 --netmask 29 --gateway 100.96.0.1 --dns-forward
  100.96.0.3 --no-map-gw --no-dhcp`, no port forwards either way,
  `--ns-ifname eth0`, with the child's PID: pasta joins its user and
  network namespaces, sets up the tap interface, goes to the background
  (as nobody when started by root) and exits when the child does. Traffic
  leaves as pasta's own sockets, so the host firewall, `worm anon` and
  `worm panic` apply as to any program. DNS to 100.96.0.3 goes to the
  host's first name server (unbound). Init adds `prohibit` routes
  (rtnetlink) for 0/8, 10/8, 100.64/10, 169.254/16, 172.16/12,
  192.0.0/24, 192.0.2/24, 192.168/16, 198.18/15, 198.51.100/24,
  203.0.113/24, 224/4 and 240/4 (the connected /29 is more specific) and
  for each of the host's IPv4 addresses, collected before the clone;
  `--lan` leaves out the ranges, never the host. Without `CAP_NET_ADMIN`
  nothing inside can change them. No IPv6.
- Virtual machines (`vm.c`, `--vm`): the first session's program is
  kvmtool's `lkvm` (static, `recipes/kvmtool`; its own embedded guest init
  is stripped before embedding, or it carries build paths). Init builds a
  jail instead of pivoting into the blackhole's root: a 16 MiB tmpfs
  mounted on `/tmp` in its private mount namespace with `share/` (the
  blackhole's root, binds included), `dev/` (the usual nodes, `kvm` and a
  devpts instance), `kernel` (`/boot/vmlinuz`, read-only), `lkvm`
  (read-only), `etc/resolv.conf` (for lkvm's user-mode network) and
  `tmp/` (lkvm's `$HOME`), then pivots there. No `/proc`: a symlink the
  guest makes in the share and opens over 9p would be followed by the
  monitor (kvmtool only refuses `/../` and a final symlink), and
  `/proc/self/mem` would be the monitor's memory. The rest of the
  sandbox (cgroup, seccomp, Landlock, capabilities, `--net` routes)
  applies to lkvm unchanged. lkvm runs with `--nodefaults`, the given
  memory and CPUs (default a quarter of the memory up to 4 GiB, 2 CPUs),
  a virtio console on the session's terminal, virtio-rng, `--9p
  /share,bhroot` and `--network mode=user` with `--net` (its user-mode
  stack turns the guest's connections into sockets in the blackhole's
  namespace, so pasta and the routes apply) or `mode=none`. The guest
  kernel is the host's (it has 9p built in for this) with `root=bhroot
  rootfstype=9p ... cache=loose`, `8250.nr_uarts=0` (probing kvmtool's emulated UART echoes bytes to
  the terminal), `pci=conf1` (kvmtool has no PCI host
  bridge, so the kernel's direct probe gives up without it) and
  `init=/run/bh-init`: init binds
  `/usr/lib/bh/vm-init` (static, so it runs with any `/usr`) there.
  `bh.name`, `bh.net`, `bh.rows`, `bh.cols`, `bh.term` and `bh.cmd` (the
  last two hex-encoded, arguments NUL-separated) pass the session on.
  vm-init mounts `/proc`, `/sys`, devtmpfs, devpts and `/dev/shm`,
  configures 192.168.33.15/24 via 192.168.33.1 with `--net`, runs the
  command on `hvc0` as root with the usual environment, writes its exit
  code to `/run/bh-status` and resets the machine (kvmtool has no ACPI, so
  a power-off would only halt it; it exits on a reset); init passes that code on
  (removing a stale one before the start). One console only: joins are
  refused. Ctrl+A X stops the machine (kvmtool). The terminal size is
  set once at the start. Inside the guest nothing is filtered (its own
  kernel), so 32-bit programs and `modify_ldt` work there, unlike in a
  namespace blackhole: that is where Wine runs Windows programs.
- `bh put` opens the sources first, then (as the blackhole's ID for root)
  creates each file in the inbox with `O_CREAT|O_EXCL|O_NOFOLLOW`: the
  blackhole may be running and controls what is in there. A running VM's
  guest may not see a new file at once (9p `cache=loose`).
- `bh close` sends SIGTERM to the bh holding the lock, which kills the
  blackhole's init; it then waits for the lock and deletes the tree with
  `*at` calls that never follow symlinks.

## worm

- `src/worm/`: `worm unlock` / `worm lock` bind-remount `/` (re-applying the
  statvfs flags), list processes with files open for writing (by mnt_id) on
  EBUSY, and run `snap now --auto` after locking.
- `fw.c` is the firewall. Open ports live in `/etc/worm/firewall` (`tcp 22`,
  `udp 6000-6010`); no package ships it, and a missing file means none.
  `worm fw allow|deny` applies the new rules first and only then rewrites
  the file, bind-remounting `/` writable for the moment if it was locked
  (refused while a package change is pending, as for `worm unlock`).
- The ruleset is generated into `/run/worm/firewall.nft` and loaded with
  `nft -f`: `destroy table inet worm` plus the new table, one transaction.
  Input policy drop with established/related, loopback, ICMP and ICMPv6
  errors (no echo requests: pings go unanswered), neighbour discovery with
  hop limit 255, DHCPv6 replies and the sets `open_tcp`/`open_udp`; a named
  counter `dropped` counts the rest. Forward policy drop. Output is not
  filtered.
- The service `05-firewall` runs `worm fw load` before the network. A bad
  port list loads the firewall with no open ports and exits 1. `30-network`
  runs `/usr/lib/worm/network`, which starts udhcpc (on `eth0`, or the
  first wired device) only once `inet worm` has an input chain, trying
  `worm fw load` once more first; otherwise it fails and worminit retries,
  so the machine stays offline without a firewall. udhcpc runs with
  `-V ''` (no vendor class naming the client) and sends no host name.
- nftables is built with mini-gmp and without the CLI, JSON or xtables.
- `powerkey` (`/usr/lib/worm/powerkey`, service `03-powerkey`) watches
  every input device with a power key (the ACPI power button, keyboards)
  and new ones through inotify on `/dev/input`, and powers off cleanly
  (SIGUSR2 to worminit) when it is pressed. That is what QEMU's
  `system_powerdown`, virt-manager's and VMware's "Shut Down" (without
  the tools) and a PC's power button send. The devices are not grabbed.

## Virtual machines

- `src/worm/vm.c` is `worm vm`. Detection: DMI (`/sys/class/dmi/id`
  `sys_vendor` and `product_name`: QEMU, innotek/VirtualBox, VMware,
  Microsoft "Virtual Machine", Xen, Parallels), then the CPU's hypervisor
  leaf 0x40000000 (`KVMKVMKVM`, `TCGTCGTCGTCG`, `Microsoft Hv`, with
  0x40000100 checked for KVM under Hyper-V enlightenments, `VBoxVBoxVBox`,
  `VMwareVMware`...). QEMU's accelerator is KVM or TCG from that leaf. The
  GPU is the first DRM card's driver (simpledrm only if nothing else);
  virtio-gpu's 3D is bit 0 (VIRTIO_GPU_F_VIRGL) of the virtio device's
  `features` below the PCI device, vmwgfx's a render node. Virtio ports by
  name (`/sys/class/virtio-ports/*/name`: `com.redhat.spice.0`,
  `org.qemu.guest_agent.0`), the balloon by its driver, shares from
  `/sys/fs/virtiofs/*/tag` and 9p's `mount_tag` (tags are checked: plain
  characters, they become folder names).
- Settings: built-in defaults, then `/usr/share/worm/vm/HYPERVISOR.conf`
  (from the worm package: the base settings each hypervisor gets in every
  image), then `/etc/worm/vm.conf`. Keys: `renderer` (auto, gles,
  software), `cursor` (auto, software, hardware), `scale`, `clipboard`,
  `agent`, `shares`, `tuning` (on/off). `auto` is resolved from what was
  found: OpenGL ES on i915/xe or a virtual GPU with 3D, software
  otherwise; the cursor in software on virtio-gpu with virgl, vmwgfx and
  vboxvideo. `worm vm set KEY VALUE` and `reset [KEY]` rewrite
  `vm.conf` (other lines and comments kept) with `/` writable for the
  moment, then run setup again.
- `worm vm setup` (service `13-vm`, after the filesystems) applies and
  writes `/run/worm/vm` (KEY=VALUE, mode 0644): the detection (the shares
  found as `share-tags`) and every setting resolved. Tuning in a VM: scheduler `none` and `add_random` 0
  for vd/sd/nvme/xvd disks, `vm.dirty_background_ratio` 5 and
  `vm.dirty_ratio` 10. Shares: a 1 MB tmpfs on `/media` (root is
  read-only) holds the mount points; virtiofs, or 9p with
  `trans=virtio,version=9p2000.L,msize=262144,access=any`, nosuid and
  nodev. VMware and Hyper-V balloons and VirtualBox's vboxguest/vboxsf are
  modules loaded there. `worm vm shares` mounts ones added later;
  `worm vm shares NAME...` mounts VirtualBox shared folders by name
  (vboxsf), which cannot be listed without its Guest Additions. The
  kernel's VirtualBox drivers need `CONFIG_VIRT_DRIVERS` (1.14's images
  lacked vboxguest and vboxsf; linux 6.18.64-8 has them).
- The session reads `/run/worm/vm` (`session-env`): `renderer` sets
  `WLR_RENDERER` (gles2 or pixman) before its own detection, `cursor`
  sets or unsets `WLR_NO_HARDWARE_CURSORS` after it, `scale` other than
  auto becomes `WORM_DEFAULT_SCALE`, which worm-shell uses when the user
  has no `~/.config/wormos/display`. labwc's `autostart` starts the clip
  agent unless `clipboard=off`.
- `src/worm/guest-agent.c` (`/usr/lib/worm/guest-agent`, service
  `45-guest-agent`, respawn) speaks the QEMU guest agent protocol on the
  port named `org.qemu.guest_agent.0` (`/dev/vportNpM`, found through
  sysfs, opened non-blocking). The stream is split into top-level JSON
  objects by a small scanner (strings and escapes tracked, 0xFF resets,
  64 KiB at most); a tiny parser reads `execute`, a top-level `id` (echoed
  back raw) and the arguments `id`, `time` and `mode`. Commands:
  guest-sync, guest-sync-delimited (0xFF before the answer), guest-ping,
  guest-info, guest-get-osinfo (`/etc/os-release`, uname),
  guest-get-hostname, guest-get-timezone, guest-get-time, guest-set-time
  (with a time, `clock_settime`; without, busybox `hwclock -s -u`; the
  hardware clock is never written), guest-shutdown (SIGUSR2, SIGUSR1 or
  SIGTERM to worminit as busybox poweroff/halt/reboot; no answer, like
  QEMU's agent), guest-network-get-interfaces (getifaddrs, MAC from
  sysfs), guest-get-users (users with a Wayland socket in `/run/user`),
  guest-fsfreeze-status ("thawed") and guest-get-vcpus. Anything else is
  CommandNotFound: no guest-exec, guest-file-* or SSH key commands, so
  the host cannot run programs or touch files through it. Without the
  port or with `agent=off` it sleeps and looks again every 10 seconds
  (5 while connected); POLLHUP means nobody is on the host's end. Its PID
  is in `/run/worm/guest-agent.pid` for `worm vm`.
- The kernel has VirtualBox's guest drivers as modules (`vboxguest`,
  `vboxsf`, `vboxvideo`; VMSVGA is vmwgfx), besides Hyper-V's and the
  virtio ones; virtio_balloon selects free page reporting.
- `worm hostname [NAME]` writes `/etc/hostname` (with `/` writable for
  the moment) and calls sethostname.

## Time and passwords

- `worm time` (`src/worm/time.c`). The time zone is the symlink
  `/etc/localtime` into `/usr/share/zoneinfo` (tzdata, compiled with the
  build machine's `zic -b slim`); no link means UTC. `worm time zone NAME`
  checks the name (no `..`, a regular file under zoneinfo) and swaps the
  link while `/` is writable for the moment. The first-boot setup passes
  the zone the greeter's searchable list (from `zone1970.tab`) chose as a
  seventh field of the `setup` request; `first-boot --create` makes the
  link.
- `worm time sync` sends a HEAD request over HTTPS (curl, TLS 1.2+, https
  only) to www.quad9.net, then www.torproject.org, then www.debian.org,
  parses the `Date` header, compares it with the middle of the request and
  sets the clock (and the hardware clock, in UTC, except on live systems:
  `/run/wormos/live`) when it is off by more than 2 s. The result goes to
  `/run/wormos/time` for `worm time`. The service `35-time`
  (`/usr/lib/worm/timesync`, `safe = no`) runs it 20 s after boot and
  every six hours; after a failure every 30 s for ten minutes, then every
  two. It works in anonymous mode through the TransPort. Before that, in
  anonymous mode while Tor is not connected, `anon_tor_skew()` reads
  Tor's log for "our clock is behind by 5 hours, 2 minutes" (NETINFO cells
  and directory answers from relays) or "Our clock is 3 hours, 0 minutes
  behind the time published in the consensus"; a skew between a minute
  and 40 years (a dead CMOS battery resets to 2000) sets the clock, Tor restarts (its log starts over) and
  the check reports failure so the HTTPS check follows in 30 s. Every
  correction is also logged (`worm time: clock corrected by ...`). Tor
  itself copes with a clock a few hours behind; days off (or ahead by
  more than a day) it cannot connect.
- `worm passwd` (`account.c`) reads the current and new password (twice)
  with echo off and feeds them to the setuid `/usr/lib/worm/passwd`
  (`passwd.c`), which takes the caller's account from the real UID, checks
  the current password (`password_ok`, a delay after a wrong one; root is
  not asked), refuses an empty password or one with `:`, becomes root and
  feeds the new one to `worm passwd-apply USER`, which runs `chpasswd -c
  sha512` while `/` is writable for the moment. zsh aliases `passwd` to it
  for users.
- The power helper also takes `panic` (wheel only, no password: it only
  protects) and runs `worm panic` as root; the panel's power menu has the
  entry with a second click to confirm. `/run/worm` is 0711 so the panel
  can see `network-cut` without listing the directory.

## Tool kits

- `worm kit` (`src/worm/kit.c`). Kits are files in `/usr/share/worm/kits`
  (from `recipes/worm/files/kits`): `title`, `description`, `included`
  (tools WormOS already has, shown only), one `tool = PACKAGE` per Debian
  or Kali package and one `core = PACKAGE` per core package. `list` and
  `show` work for everyone (`show` marks core packages already installed);
  `install` runs `wh update` first (the system's index may be as old as
  its image). `install` and `remove` need root and take several kits, or `all`
  (since 1.13): the wormhole tools of each kit are installed in turn (a
  tool an earlier kit of the same run installed is not installed again),
  then the missing core packages of every kit go into one change, with
  every kit's record written first (a core change soft-reboots, which
  would end a loop over kits); `remove all` removes the installed kits,
  and a tool is kept only when a kit that stays has it. For one kit,
  `install` runs `wh --yes install
  PACKAGE` for each `tool` in turn, so each comes from the first source
  that has it (Debian, Kali) and a missing one is reported and skipped.
  Then the core packages not installed yet go to one `wh --yes install`:
  a core change soft-reboots, so the kit's record
  (`/var/lib/worm/kits/NAME`, the packages it added) is written before,
  with them, and rewritten without them if wh fails. `remove` runs `wh
  --yes remove` for each recorded `tool` that no other installed kit
  recorded, then removes the recorded core packages in one change (core
  packages that were there before the kit are not recorded, so they stay).
  The `basic` kit is mostly core packages: the everyday tools of the
  standard edition and a few more, for Barebones. Wormhole tools share
  the home folder.

## Licenses

- `recipes/licenses` collects license files from every recipe's first
  upstream archive (names such as `COPYING*`, `LICENSE*`, `COPYRIGHT*`,
  `NOTICE` at the top of the tree, and files in a `LICENSES/` directory)
  into `/usr/share/licenses/NAME`, plus glibc, gcc-libs and linux from the
  toolchain archives, WormOS's own `LICENSE` as `wormos/COPYING`, and short
  notes for packages without a license file (libdrm, sqlite,
  ca-certificates, the keyrings, Tor Browser). Recipes that install their
  own license files are skipped, so no path has two owners.

## Devices and modules

- The kernel builds Intel's i915 and xe, VMware's vmwgfx, QXL, uinput and the
  VMware guest drivers as modules (zstd-compressed, signed); everything a
  VM needs to boot stays built in. `linux-firmware` ships only the Intel GPU
  firmware (`i915/`, `xe/`), zstd-compressed; the kernel decompresses it.
- kmod provides `modprobe`/`depmod` (busybox's module applets and mdev are
  left out). eudev is the device manager: the service `02-udev` starts
  `udevd`, replays the boot devices (`udevadm trigger`, which loads modules
  and firmware) and waits for it to settle. Network devices keep their
  kernel names (the `80-net-name-slot` rule is removed).
- The hardware database is compiled when the package is built: the eudev
  recipe runs its own `udevadm hwdb --update --usr` through the sysroot's
  loader (`/etc` is read-only on the target). The OUI (MAC vendor) table is
  left out.
- Device groups: `video` (`/dev/dri/card*`), `render` (render nodes, open
  to everyone), `input`, `kvm` and the usual others in base-files.
- seatd (`45-seatd`, `seatd -g video`) hands the GPU and input devices to
  the compositor, so it runs as the user.

## Graphics stack

- Build support for meson projects: `meson_build`/`meson_package` in wbuild
  with a generated cross file, and `host_env` plus `$NATIVE`
  (`build/host/native`) for tools the build runs on this machine
  (wayland-scanner, Mesa's `mesa_clc`). A wrapper keeps the target's
  pkg-config settings away from lookups for this machine.
- Mesa: gallium drivers iris and crocus (Intel), virgl and svga (virtual
  GPUs) and softpipe; EGL, GBM and OpenGL ES; no LLVM, no GLX, no Vulkan.
  The Intel drivers compile OpenCL kernels at build time with `mesa_clc`,
  which the Mesa recipe first builds for this machine from Debian's LLVM.
- Text: freetype, harfbuzz, fribidi, fontconfig (cache in
  `/var/cache/fontconfig`, created on first use), cairo, pango, glib.
  Fonts: Inter (sans-serif) and Hack (monospace), made the defaults by
  `/etc/fonts/conf.d/60-*.conf`.
- glibc ships the `C.UTF-8` locale (compiled by the target's localedef
  through its own loader); `/etc/profile` sets `LANG=C.UTF-8`.
- Xwayland (since 1.12): `xwayland` 24.1 (glamor and DRI3, no GLX since
  Mesa has none, no XDMCP or TCP, keymaps compiled by `xkbcomp` into
  `/tmp`, built-in fonts only) with libxkbfile, libxfont2, libfontenc,
  libxcvt and libxshmfence; wlroots and labwc are built with it (and
  xcb-util-wm). labwc starts Xwayland lazily, on the first X11
  connection, and sets `DISPLAY` for everything it starts. The session
  sets `WLR_XWAYLAND=/usr/lib/xwayland/xwayland-gles`, which runs it with
  `-glamor es`: glamor tries desktop OpenGL first, which Mesa here lacks,
  and libepoxy aborted Xwayland on a GPU (virgl); without a GPU, glamor
  gives up and Xwayland draws in software; wlroots reads
  the Xwayland path from its pkg-config file, so the recipe removes the
  sysroot from `build/include/config.h`. `/tmp/.X11-unix` is created by
  root at boot (`runtime-dirs`, mode 1777): created by the first
  compositor it would belong to that user. The login screen runs without
  Xwayland (`LABWC_NO_XWAYLAND=1` in `greeter-session`, a three-line labwc
  patch). Wormholes share `/tmp` and the environment, so their X11
  programs reach the user's Xwayland; blackholes have their own `/tmp`
  and no `DISPLAY`. Tor Browser stays on Wayland (`GDK_BACKEND=wayland`,
  no `DISPLAY`).

## Message bus

- `dbus` is the reference D-Bus daemon (expat only; no systemd, X11
  autolaunch, SELinux or AppArmor). The service `40-dbus` runs the system
  bus as the `messagebus` user on `/run/dbus/system_bus_socket`. There is
  no setuid activation helper: no system service is started on demand.
- `/etc/machine-id` links to `/var/lib/dbus/machine-id`, created by
  `dbus-uuidgen --ensure` at the first boot, so each installation has its
  own ID while `/etc` stays read-only.
- Each desktop login gets a session bus, started by the session script
  before labwc and stopped after it, listening on `$XDG_RUNTIME_DIR/bus`
  (`unix:runtime=yes`, also the default address clients try). Wormholes
  share `/run`, so their programs find both buses; blackholes see neither.
  labwc's `autostart` passes `WAYLAND_DISPLAY` and `XDG_CURRENT_DESKTOP` to
  the bus (`dbus-update-activation-environment`) for programs it starts on
  demand.

## GTK

- GTK 4.24 with the Wayland backend only: no X11, Vulkan, GStreamer,
  printing or desktop search. Its dependencies: gdk-pixbuf (PNG, JPEG and
  TIFF loaders built into the library, so no loader cache; no glycin),
  libjpeg-turbo (CMake, SIMD assembled with the build machine's nasm),
  libtiff, libepoxy (EGL only), graphene, shared-mime-info, cairo with
  cairo-gobject and harfbuzz with its subset library and GLib integration.
  SVG icons are drawn by GTK's own SVG renderer; there is no librsvg.
- `wormos-gtk-theme` compiles GTK's own theme sources (from the GTK
  release) with sassc, with `_colors.scss` replaced by the WormOS palette
  and rounder corners, into `/usr/share/themes/WormOS/gtk-4.0` (dark only;
  `gtk-dark.css` is the same file).
- `/etc/gtk-4.0/settings.ini` selects the theme, the Adwaita icons and
  cursors, Inter and the dark colour scheme. labwc uses the same cursors
  (`XCURSOR_THEME`). With wlroots' pixman renderer the session also sets
  `GSK_RENDERER=cairo`: GTK's OpenGL renderer would run on softpipe.
- dconf keeps GSettings values (its service is started on demand by the
  session bus). gtk4-layer-shell is there for the shell's panel.

## Desktop shell

- `src/shell/` is `worm-shell`, one GTK program for the panel, the
  application launcher and notifications, started by labwc's `autostart`.
  It is a single-instance GtkApplication (`org.wormos.Shell`): `worm-shell
  --launcher` (the Super key on its own, or Super+Space) toggles the
  launcher in the running shell; `--quit` stops it.
- `wayland.c` binds two wlroots protocols on GTK's own connection:
  wlr-foreign-toplevel-management (`toplevels.c`, the task list: click to
  focus or minimise, middle click to close) and wlr-output-management
  (`outputs.c`). The protocol XML is copied from wlroots and compiled with
  wayland-scanner.
- Panel (`panel.c`): a layer-shell surface along the top of each monitor
  with an exclusive zone. It takes no keyboard focus, except while one of
  its menus is open. Left: the launcher button and the windows; middle: the
  clock (a calendar on click); right: snapshots (from `/run/snap-status`),
  protection (firewall, anonymous mode, a cut network), network (a device
  with a carrier and its address) and the power menu (display scale, log
  out, restart, shut down; restart and shut down need a second click).
  Status is refreshed every five seconds (`status.c`).
- Display scale: `~/.config/wormos/display` holds `scale = auto` or a
  factor. Automatic is 2 for outputs at least 2000 pixels high and 1
  otherwise. The shell applies it when the setting changes and when an
  output appears or changes mode, so a scale set with wlr-randr lasts until
  then.
- Restart and shut down use `/usr/lib/worm/power` (from the worm package),
  setuid root: it takes only `off` or `reboot`, clears its environment,
  requires the caller (real user and groups) to be root or in `wheel`
  (`src/worm/admin.c`), and signals PID 1 as busybox `poweroff` and
  `reboot` do. Logging out runs `labwc --exit`.
- The snapshot menu also takes a snapshot (through the snapshots helper,
  below) and opens the Snapshots program.

## Snapshots program

- `src/snapshots/` is `wormos-snapshots` (GTK, `org.wormos.Snapshots`):
  the snapshots newest first, grouped by day, with badges for scope,
  automatic, pinned and suspect ones; a details pane to edit the comment,
  pin or unpin, show the changes since the snapshot (`snap diff`, system
  only), restore a file or folder, rewind (the scope can be chosen for
  snapshots holding both) and delete; and a header button that takes a
  snapshot with a comment and scope.
- Everything goes through `/usr/lib/worm/snapshots` (`src/worm/snapshots.c`),
  setuid root, for root and members of `wheel`. Listing, taking a snapshot,
  pinning, commenting and showing system changes run right away. Unpinning,
  deleting, rewinding, restoring and turning automatic snapshots on or off
  also need root's password on standard input, checked against
  `/etc/shadow` with libxcrypt (two seconds' wait after a wrong one; an
  empty field takes an empty password, a locked one none). A program
  running as the user can add snapshots but cannot remove protection or
  change the system. Arguments are checked (IDs, comments up to 200
  characters without control characters, absolute paths without `..`);
  the helper then clears the environment, becomes root completely and runs
  snap with an argument list of its own.
- The program passes the password from its dialog to the helper's input
  and keeps the dialog open after a wrong one (exit status 3).
- Launcher (`launcher.c`): an overlay layer surface with exclusive keyboard
  focus and a card: search field, sidebar of sections, content. Entries are
  the ones GIO lists (`g_app_info_should_show`). Sections: Recent (desktop
  ids in `$XDG_STATE_HOME/wormos/launcher-recent`, newest first, 12),
  All, the usual categories (the first freedesktop main category: Utility
  is Accessories, Network is Internet, AudioVideo is Multimedia...), and the
  security sections wh writes into tool entries (`X-WormOS-Section`),
  ordered and with icons from `/usr/share/wormos/tool-menu/sections`.
  Terminal=true entries show as rows with their description, the others
  as tiles; All leaves out the security command line tools. Search scores
  every word against name (start of the name, then of a word, then
  anywhere), program, generic name, keywords, description and section, and
  lists up to 60 results, best first; Up and Down move the selection from
  the search field, Enter starts it. Typing anywhere goes to the search
  field; Escape clears it, then closes.
- Notifications (`notify.c`): `org.freedesktop.Notifications` on the session
  bus (Notify, CloseNotification, GetCapabilities, GetServerInformation and
  the two signals), shown as cards in an overlay surface at the top right.
  Default timeout six seconds, none for critical ones; body markup is used
  when Pango accepts it.
- The look is `/usr/share/wormos/shell/shell.css`, on top of the GTK theme.
- User settings (`config.c`): `~/.config/wormos/shell` (a GKeyFile:
  `[panel] position, clock-12h, clock-seconds, clock-weekday, clock-date`,
  `[notifications] do-not-disturb`). The folder is watched
  (GFileMonitor): a changed `shell` file is reloaded and the panel
  rebuilt when its edge changed (`WORM_SHELL_PANEL`, set by editions,
  still wins), otherwise the clock and the do not disturb tile follow; a
  changed `display` file is read again and the scale applied.
- Quick settings: the power menu starts with a grid of tiles (Settings,
  Terminal, Screenshot 0.4 s after the menu closed, Snapshots, a system
  monitor in foot when btop or htop exists, and the *Do not disturb*
  toggle). With do not disturb, Notify still returns an ID but only
  critical notifications get a card. `worm-shell --do-not-disturb
  on|off|toggle` sets it from keys and the launcher.
- Quick actions: desktop entries with `X-WormOS-Action=true` (and
  `NoDisplay=true`, so other menus skip them) are loaded by the launcher
  into no section; search finds them as "Quick action". wormos-desktop
  ships them (`org.wormos.Action.*`: snapshot, screenshots, do not
  disturb, reload, every Settings page) and `/usr/lib/worm/quick-action`
  (snapshot through the snapshots helper and screenshots, with a
  notification through `gdbus`).
- Icons: an entry's icon that the theme lacks (a themed name) or whose
  file is missing is replaced by one for its section (applications-*,
  utilities-terminal for command line tools); GTK falls back to the
  symbolic Adwaita variants. wormos-desktop ships menu entries with
  `TryExec` (hidden where the program is missing, as in Barebones) and
  icons in the WormOS tile style for the preinstalled command line tools
  and WormOS's own commands (`files/apps`, `files/icons`).

## Settings program

- `src/settings/` is `wormos-settings` (GTK, `org.wormos.Settings`,
  single instance: `--page NAME` in a second call switches the running
  one). A sidebar of pages in a GtkStack; rows in rounded list boxes
  (`settings.css`). Pages that show live state (Display, Sound,
  Network & privacy, Virtual machine) refill on map.
- The user's settings are files the desktop reads: `~/.config/wormos/
  display` (scale), `shell` (panel edge, clock, do not disturb),
  `wallpaper` (`image = PATH` or `color = #RRGGBB`, shown by
  `/usr/lib/worm/wallpaper`, which starts a new swaybg and then stops the
  one in `$XDG_RUNTIME_DIR/swaybg.pid`; autostart uses it too),
  `~/.config/labwc/environment` (`XKB_DEFAULT_LAYOUT`, other lines kept)
  and `~/.config/wormos/xdg/labwc/rc.xml`: keyboard repeat and the libinput
  `default` and `touchpad` categories, each with every field (labwc uses
  the last category defined for a device type, unset fields fall back to
  libinput's defaults, not to earlier files). The session puts
  `~/.config/wormos/xdg` first in `XDG_CONFIG_DIRS` and runs labwc with
  `-m`, which merges rc.xml, menu, environment and themerc from every
  directory, lowest priority first: the system's, the Settings
  program's, then `~/.config/labwc`, so the user's own file wins and
  needs only what it changes. labwc then also runs every `autostart` it
  finds: the system's exits at once when the edition (`VARIANT_XDG` in
  `/etc/worm/variant`) has its own, and a user's `~/.config/labwc/autostart`
  runs in addition. The wallpaper script takes an edition's
  `VARIANT_WALLPAPER` as its default. Changes run `labwc --reconfigure` (labwc's
  PID from `LABWC_PID`, which its children inherit); sliders write 0.4 s
  after they rest.
- Display: the scale list of the panel; each screen's modes from
  `wlr-randr`, a choice runs `wlr-randr --output NAME --mode WxH@RHz`.
  Sound: `wpctl status` (Audio's Sinks and Sources, `*` the default),
  `wpctl set-default ID` and `set-volume ID V`. Date & time: zones from
  `zone1970.tab` plus UTC in a filtered list.
- System changes go through `/usr/lib/worm/settings` (`src/worm/
  settings.c`, setuid root, wheel only, like the snapshots helper):
  `firewall` and `anon-status` without a password; `zone NAME`,
  `anon on|off`, `hostname NAME`, `vm KEY VALUE` and `vm-reset KEY` with
  root's password on standard input (exit 3 when wrong; the dialog stays
  open). Arguments are letters, digits and a few separators; the helper
  clears its environment, becomes root and runs `worm` with its own
  argument list. Automatic snapshots go through the snapshots helper
  (`auto on|off`), the user's own password through `/usr/lib/worm/passwd`.
  With `/etc/worm/no-root` the dialog says root is locked instead, and
  the controls that change the system are insensitive.
- The helper also serves `worm` itself: run by a user, `worm status` and
  `worm fw status` go through it (`status`, `firewall`: no password, wheel
  only), and on a system without root (`/etc/worm/no-root`) so do
  `worm anon bridges KIND`, `worm time sync` and `worm panic [off]`
  (`bridges`, `time-sync`, `panic`, `panic-off`), which elsewhere need
  root's password: nobody could give it there, and they are safe for the
  session's own user. The launcher hides entries with
  `X-WormOS-Needs-Root=true` (the root terminal) on such systems.

## Desktop session

- `wormos-desktop` holds the session: `/usr/lib/worm/session`, started by
  wormlogin (below) or, for a console login on tty1, by
  `/etc/profile.d/desktop.sh` (which leaves it alone when `WORMOS_LOGIN`
  is set); logging out of the desktop ends the login. The environment it
  shares with the login screen (renderer choice, VMware cursor, GTK
  settings, `run_labwc` with the pixman fallback) is in
  `/usr/lib/worm/session-env`, which also reads `/run/worm/vm` (Virtual
  machines, above). At the first login it creates the
  standard folders (Desktop, Documents, Downloads, Music, Pictures, Videos,
  Public, Templates) and records them in `~/.config/user-dirs.dirs`. The
  session script checks
  `XDG_RUNTIME_DIR` (`/run/user/UID`, created by the `12-runtime-dirs`
  service and by the first-boot setup) and membership of `video`, sets the
  Wayland environment and `LIBSEAT_BACKEND=seatd`, and runs labwc with its
  output in `$XDG_RUNTIME_DIR/desktop.log`.
- Renderer: OpenGL ES when a card is driven by i915, xe or vmwgfx (Mesa
  svga, VMware with 3D acceleration) or is QEMU's virtio GPU with virgl
  (since 1.11.1: the DRM card's device is the PCI device, driver
  `virtio-pci`; the virtio device below it, driver `virtio_gpu`, has
  VIRTIO_GPU_F_VIRGL as bit 0 of its `features`; with virgl the cursor is
  drawn into the frame, `WLR_NO_HARDWARE_CURSORS=1`, because QEMU showed
  the cursor plane's image upside down, hotspot included: wlroots draws it
  with OpenGL ES and virglrenderer flips it again when QEMU reads it back);
  otherwise wlroots' pixman renderer
  (`WLR_RENDERER=pixman`), which beats OpenGL emulated on the CPU. With
  VMware's 3D acceleration off, EGL fails at once and the session falls
  back to pixman (below). wlroots checked every client's DMA-BUF by
  importing it and closing the GEM handle again; vmwgfx imports DMA-BUFs
  as its own surface objects, which GEM_CLOSE rejects, so every OpenGL
  client (GTK, the greeter) was refused and disconnected.
  `recipes/wlroots/files/vmwgfx-dmabuf-check.patch` skips that check on
  vmwgfx; the renderer still imports the buffer for real. wlroots does not fall back to pixman by itself
  when a render node exists, so if labwc fails within 15 seconds with
  OpenGL ES, the session starts it again with pixman (the first log is kept
  as `desktop.log.gles`). The session starts in the user's home folder.
  On vmwgfx it sets `WLR_NO_HARDWARE_CURSORS=1`: VMware's cursor plane
  stays invisible with wlroots.
- Screen size: the virtual GPUs (virtio-gpu, QXL, vmwgfx) change their
  preferred mode when the viewer's window is resized, but wlroots only reads
  an output's modes when it connects. `recipes/wlroots/files/
  follow-preferred-mode.patch` makes the DRM backend look at the preferred
  mode on every hotplug event for outputs with a virtual connector or the
  `hotplug_mode_update` property; when it changes, the mode is added to the
  output and requested through `request_state`, which labwc applies. A mode
  set by hand (wlr-randr) stays until the window size changes again.
- VMware is not supported for now: open-vm-tools (window size, time sync,
  power operations) is no longer built. The kernel's vmwgfx driver and
  the session's vmwgfx settings remain, so the ISO may still start there.
  libxcrypt is built with the strong hashes only.
- `xcompose` ships libX11's UTF-8 Compose table (`/usr/share/X11/locale`,
  mapped to every UTF-8 locale) so dead keys and the Compose key work.
- labwc's configuration is in `/etc/xdg/labwc` (`rc.xml`, `menu.xml`,
  `autostart`, `environment`), the theme in `/usr/share/themes/WormOS`,
  foot's in `/etc/xdg/foot/foot.ini`. `rc.xml` clears labwc's default
  left and middle button bindings on the background, so only the right
  button opens the menu. `autostart` starts swaybg with the wallpaper
  (2560x1440, drawn at build time with pycairo by `wallpaper.py`) and, once
  per user, a welcome terminal showing the full guide (`welcome --full`).
- foot starts login shells, so every new terminal reads `/etc/profile`;
  `/etc/profile.d/motd.sh` runs `welcome` (system details, main commands
  and a random line from `/usr/share/wormos/tips`) in interactive shells
  with `WAYLAND_DISPLAY` set, unless `~/.hushlogin` exists. It exports
  `WORMOS_GREETED` so shells started inside the terminal stay quiet; `su -`
  starts with a clean environment without `WAYLAND_DISPLAY`, so it stays
  quiet too. The menu's "Applications" entry is a pipe menu
  (`/usr/lib/worm/apps-menu`) listing desktop entries in wormholes whose
  program wh exported.
- Wormholes: whc shares `/run` (so the Wayland socket) and `/dev/dri`, and
  binds `/usr/share/fonts` read-only on `/usr/local/share/fonts/wormos`
  (the mount point is created when a wormhole is set up).
- Consoles: `/usr/lib/worm/console TTY` runs getty on tty1 and tty2. On
  tty1 it hands over to wormlogin when that is installed, unless
  `/run/wormlogin/console` exists; otherwise, while `/etc/passwd` has no
  user with a UID of 1000 or more, it first runs `/usr/lib/worm/first-boot`
  there (with the tty as controlling terminal).
- `first-boot` creates the first account: `worm unlock`, `adduser` (with
  the full name), the groups `wheel video input audio render kvm`,
  `chpasswd -c sha512` for the user and root, `worm lock -c "First boot:
  created NAME"` (which takes a snapshot), the runtime directory, and the
  keyboard layout in `~/.config/labwc/environment`. Interactively on the
  console, or with `--create NAME FULL-NAME LAYOUT` and the two passwords
  on standard input (for the graphical setup).

## Shell

- `zsh` 5.9.2 (`--disable-dynamic`: every module is linked in, so nothing
  is loaded at start) with `ncurses` 6.6 (wide characters; one library,
  with `libncurses.so`, `libtinfo.so` and the narrow `.pc` names pointing
  at it for builds). The terminal database holds about 30 entries: the
  consoles and common terminal emulators and multiplexers, compiled with
  the build machine's `tic`; `xterm-kitty` and `xterm-ghostty` link to
  ncurses' own kitty and ghostty entries.
- zsh reads `/etc/zsh`: `zprofile` runs `/etc/profile` in sh emulation
  (PATH, locale, the desktop on a console login, the welcome), `zshrc`
  is the interactive setup. Completions for other systems and X11 and the
  new-user wizard are not installed.
- `zshrc`: history (`share_history`, 50,000 lines), emacs keys plus the
  usual escape sequences, completion (`compinit` checks the files with
  `compaudit` only when its dump in `~/.cache/zsh` is older than a day),
  the prompt (`_wormos_prompt_state` in `precmd` reads `/run/snap-status`
  without starting a process, `$BLACKHOLE` and, for root, whether `/` is
  writable), window titles (with control characters shown as text), and
  last the plugins zsh-autosuggestions (history, then completion) and
  zsh-syntax-highlighting, which are left out with `TERM=dumb` or
  `ZSH_PLAIN`. It starts in about 25 ms on the build machine.
- Completions for our commands are in `/usr/share/zsh/site-functions`
  (`recipes/zsh/files/completions`). Snapshot IDs come from
  `snap list --raw` (root only), core packages from `/var/lib/wh/index`,
  installed ones from `/usr/lib/wh/db` and `/var/lib/wh/foreign`.
- foot sends `^H` for Ctrl+Backspace, which most programs take as
  Backspace; `foot.ini` binds it to `^W` (`[text-bindings]`), which
  deletes a word in zsh, busybox sh, readline, Python and vim. zsh binds
  both.
- The first-boot setup gives the new user `/usr/bin/zsh` (listed in
  `/etc/shells`), and root has it too (base-files' `/etc/passwd`), so `su`,
  `su -`, the root terminal (`su -`) and console logins all get the same
  shell with history, completion and highlighting. Root's shell was busybox
  `sh` before 1.6; the keep-accounts hook moves a running system's root from
  `/bin/sh` to base-files' shell when base-files is upgraded (only if that
  shell exists). The tests run `exec /bin/sh` after logging in on the serial
  console, where zsh's line editor would get in the way.
  wormlogin starts the session with `argv[0]` `-sh`, so zsh runs it in sh
  emulation (it only runs the session script).

## Clipboard

- `clip` (`src/clipboard/`) uses ext-data-control-v1, which labwc offers
  to every client and which needs no window or focus. `clip copy` sets the
  selection and stays in the background (`setsid`, output to `/dev/null`)
  until another program copies; `clip paste` prints the text. Text only,
  at most 16 MiB.
- `clip agent` (started by labwc's `autostart`) shares the clipboard with
  the host over the SPICE agent port `/dev/virtio-ports/com.redhat.spice.0`
  (group `video`, mode 0660, from `70-spice-agent.rules`). It speaks the
  vdagent protocol (spice-protocol's `vd_agent.h`): chunks of at most
  2048 bytes carrying messages, capabilities `MOUSE_STATE`,
  `MONITORS_CONFIG`, `REPLY`, `CLIPBOARD_BY_DEMAND`,
  `CLIPBOARD_SELECTION` and `GUEST_LINEEND_LF`. Each side announces a copy
  (grab) and sends the text when asked (request), so nothing crosses until
  someone pastes. The host's text is cached until its next grab. Sources
  the agent creates offer `application/x-wormos-clip` as well, so it never
  announces the host's own text back. Only the clipboard is shared, not
  the primary selection. Without the port it exits at once; when the host
  side is closed (`POLLHUP`) it reopens the port every two seconds.
- The same port reaches a SPICE client through the SPICE server
  (virt-manager adds the channel with SPICE graphics) and QEMU's own UIs
  through `-chardev qemu-vdagent,clipboard=on`, which `run-qemu.sh`
  always adds. QEMU's VNC server passes plain ClientCutText to the guest
  and the guest's copies to clients using the extended clipboard.
- The pointer: once any agent has the port open, the SPICE server sends
  every pointer event of a client in client mouse mode to the agent as
  `MOUSE_STATE` (position on display `display_id` in its pixels, buttons;
  the wheel as buttons 4 and 5), not to the USB tablet. The agent replays
  them through wlr-virtual-pointer-unstable-v1 (one pointer per output,
  created with that output, absolute motion against the output's current
  mode size); each wheel press is one discrete step. All buttons are
  released when the client disconnects or the port closes. QEMU's
  qemu-vdagent sends the same messages with `mouse=on`, but only to an
  agent that announces `MOUSE_STATE`; the desktop test runs that way
  (`QEMU_VDAGENT_MOUSE=on`) and checks QEMU's `info mice`.
- Display size: SPICE clients send the window size ("Auto resize VM with
  window") only to an agent that announces `MONITORS_CONFIG`. The SPICE
  server hands it to QEMU's graphics device (virtio-gpu's EDID, QXL's
  client monitors config), whose new preferred mode wlroots follows (see
  Graphics); it reaches the agent only when the device cannot take it, and
  the agent then just replies.
- The login screen runs `clip agent --no-clipboard` from the greeter's
  labwc `autostart` (pointer and display size, no clipboard); the user's
  agent opens the port once it is free.
## Sound

- Kernel: the HD Audio controller driver, codec drivers (generic,
  Realtek, Conexant, Analog Devices, IDT, Cirrus Logic, HDMI), USB audio,
  virtio-sound and the Ensoniq AudioPCI are modules that udev loads. The
  controller must be a module: the HD Audio core loads codec drivers only
  when it is modular itself (`request_codec_module()` is `#ifdef MODULE`). There
  is no Sound Open Firmware, so laptops whose microphones or speakers need
  Intel's audio DSP only get HD Audio and HDMI. `/dev/snd` belongs to the
  group `audio` (the first-boot setup adds the user to it).
- `alsa-lib` (no topology or Python), `pipewire` and `wireplumber` (with
  `lua` as a shared library and the session manager's scripting language).
  PipeWire is built for audio only: ALSA devices, the PulseAudio server
  (`pipewire-pulse`), the ALSA plugin, and `pw-cat`/`pw-play`/`pw-record`
  with `libsndfile` (uncompressed formats only, no external codecs). No
  Bluetooth, JACK, video, systemd or network audio (the RTP, VBAN, AES67,
  AVB, NetJACK and raw TCP modules are removed). alsa-lib reads
  `/etc/alsa/conf.d`, where links make PipeWire the default ALSA device.
- WirePlumber's `wireplumber.conf.d/90-wormos.conf` turns off Bluetooth,
  video capture, logind and the desktop portal parts.
- The session script starts `pipewire`, waits for `$XDG_RUNTIME_DIR/pipewire-0`,
  then starts `wireplumber` and `pipewire-pulse`, with their output in
  `$XDG_RUNTIME_DIR/sound.log`, and stops them when labwc ends. They run as
  the user, at normal priority (there is no RealtimeKit). The PulseAudio
  socket is `$XDG_RUNTIME_DIR/pulse/native`; wormholes share `/run`, so
  their programs find it. Blackholes see neither, and the login screen has
  no sound.
- Volumes and the chosen output are kept by WirePlumber in
  `~/.local/state/wireplumber`.
- The shell's volume button (`src/shell/volume.c`) runs `wpctl` on
  `@DEFAULT_AUDIO_SINK@`: `get-volume` with the five-second status refresh,
  `inspect` for the output's name when the menu opens, and `set-volume -l 1`
  or `set-mute` for changes. Changes run one at a time and a waiting level is
  replaced by a newer one; readings are ignored until all changes are done.
  The button is hidden while there is no output. The volume keys run
  `worm-shell --volume up|down|mute` (5% steps), as does scrolling on the
  button.
- VMware images have an HD Audio device (`sound.virtualDev = "hdaudio"`).

## Login

- `wormos-login` (`src/login/`): `wormlogin`, the login manager on tty1,
  and `wormos-greeter`, the login screen and first-boot setup. Its bottom
  left corner shows the system and version (`PRETTY_NAME` from
  `/etc/os-release` and `VARIANT` from `/etc/worm/variant`: "WormOS 1.14.1
  Amnesia"). `welcome --full` puts `VERSION_ID` after "WormOS" on the
  guide's title line (the logo's middle line, line 3).
- wormlogin runs the login screen as the system user `greeter` (with the
  groups video, input and render set directly, not in `/etc/group`; home
  and runtime directory `/run/wormlogin/greeter`):
  `/usr/lib/worm/greeter-session` starts labwc with the configuration in
  `/usr/share/wormos/greeter/labwc` (no key or mouse bindings, no menu) and
  the greeter as its session program, so labwc ends with it. There is no
  session bus; GSettings uses its memory backend.
- The greeter sends requests to `/run/wormlogin/socket` (SOCK_SEQPACKET,
  owned by greeter, mode 0600; wormlogin also checks the peer's user ID):
  NUL-separated fields, `login USER PASSWORD`, `setup NAME FULL-NAME
  PASSWORD ROOT-PASSWORD LAYOUT` (refused once any account exists) and
  `power off|reboot`, answered `ok` or `fail` and a message. wormlogin
  checks passwords against `/etc/shadow` (`src/common/password.c`, two
  seconds' wait after a failure, also for unknown users) and runs
  `first-boot --create` for the setup, reading all of its output.
- After a login or the setup, the greeter exits and wormlogin starts the
  desktop as the user: a new session with tty1 as controlling terminal,
  the user's groups (initgroups), `/run/user/UID`, a clean environment with
  `WORMOS_LOGIN=1`, and the user's shell as a login shell running
  `/usr/lib/worm/session`. When it ends, the login screen comes back.
- It logs logins, failed logins, the setup and sessions to the system log
  (authpriv, `logread`); the greeter's output is in `/run/wormlogin/greeter.log` and
  labwc's in `/run/wormlogin/greeter/labwc.log`.
- Fallback: without a graphics device or the greeter user, or after the
  login screen ends three times in a row without a login (labwc exits
  with status 0 when the greeter crashes, so the status is not used), wormlogin
  creates `/run/wormlogin/console` and runs the console login instead,
  until the next restart. tty2 always has a console login.
- `su` is a separate, minimal busybox build holding only `su`, installed
  setuid root; the main busybox binary has no setuid applets.

## Releases

- `scripts/release.sh VERSION` (`make release VERSION=X`) builds the root
  filesystem with `WORMOS_RELEASE=1` (root's password field becomes `!`
  until the first-boot setup), a 20 GiB image (`IMAGE` and `DISK_MB` are
  parameters of `mkimage.sh`) converted to a compressed qcow2 with
  qemu-img, and the ISO (`mkiso.sh`) from the same root filesystem.
  `SHA256SUMS` lists both. VMware is not supported for now: there is no
  VMware bundle and no open-vm-tools (the kernel keeps its VMware
  drivers).
- Package repository: `/etc/wh/repo` (set in `recipes/wh/recipe`) is the
  release `core` of the public repository `CrimsonMac/wormos-packages`,
  which holds packages only (releases of WormOS itself are on this
  repository's release page). `scripts/source-archives.sh`
  writes `out/sources/NAME-VER-REL.src.tar` for every package built from
  upstream sources (recipe directory and upstream archives; glibc and
  gcc-libs take theirs from the toolchain, linux and linux-headers the
  kernel archive, `kernel/wormos.config` and `kernel/patches`; linux-firmware only its
  recipe), plus `wormos-build-tools.tar` (wbuild, wpack, wrepo, env.sh, the
  toolchain and kernel scripts). Packages from local code get none.
  `scripts/publish-repo.sh` uploads packages and sources, then the index
  and its signature, and removes assets that are no longer current. A
  package already published under the same file name (version and
  release) is kept as published: the script downloads it over the local
  build and writes the index again (`tools/wrepo --index-only`). Released
  images carry the index of their build; 1.13's first republish replaced
  206 same-version packages with repacked bytes, and core installs on 1.13
  images failed ("does not match the package index") until `wh update`.
  `wpack` also packs the same files to the same bytes now: folders and
  `.PKGINFO` get a fixed time and `strip -p` keeps files' times (before,
  every repack made every package new).
- `mkimage.sh` copies only the data extents of the partition images into
  the disk image (`scripts/sparse-copy.py`, SEEK_DATA/SEEK_HOLE), so a
  20 GiB disk takes seconds.
- GRUB is installed at the removable path `\EFI\BOOT\BOOTX64.EFI`, so the
  images boot with fresh firmware settings (and in the BIOS boot partition
  for BIOS firmware). `run-qemu.sh` gives the disk
  `bootindex=0` so that added devices never shift the boot order.

## Tests

- Continuous integration (`.github/workflows`): `ci.yml` on every push
  (`scripts/lint.sh`; `scripts/ci-build.sh` builds `src/*` with the
  runner's GCC and Clang, `-Werror -Wno-error=format-truncation`, wh with
  Monocypher from its recipe's source, then `make -C tests/unit check`);
  `release-tests.yml` on each release, weekly and by hand (runs the
  release's own tests, checked out from its tag into `tagged/`, so checks
  added later for unreleased features do not fail it; downloads the
  release's assets, checks `SHA256SUMS`, opens `/dev/kvm` to the runner's
  user with a udev rule, runs `desktop-test.py --release`, `iso-test.py
  --live-only` on both ISOs and `editions-test.py barebones --release`,
  keeps `build/*.log` and screendumps when a test fails); `package-repo.yml`
  daily (`scripts/check-package-repo.sh` compares the signed index with the
  release's assets through the GitHub API, whose `digest` is each asset's
  SHA-256, so only the index is downloaded); `updates.yml` weekly
  (`scripts/check-updates.py` asks Repology's API, one request a second,
  for the newest release of each recipe's project; versions count once two
  repositories have them; long-term series stay on their series: Linux by
  major.minor, OpenSSL 3.5, Python 3.13, Lua 5.4, GTK 3). Jobs that only
  make sense for WormOS check `github.repository`, since the workflows also
  reach WormOS Amnesia's repository through merges.
- Unit tests (`tests/unit`): small C programs that include one source
  file and stub what it calls (`skew.c` includes `src/worm/anon.c`, whose
  `TOR_LOG` it redirects to a temporary file). An edition built on WormOS adds
  its own in `tests/unit/local.mk` (`TESTS += ...`), which the Makefile
  includes when it exists.

- `make test` (`tests/boot-test.py`) boots a throwaway copy of the image
  (`run-qemu.sh --snapshot`), logs in over serial, runs the checks and powers
  off with the ACPI power button (`system_powerdown` on the QEMU monitor,
  which `-nographic` puts on the same terminal: Ctrl-A c). Then it boots
  the image once more with BIOS firmware (`run-qemu.sh --bios`). It serves
  `out/repo` over HTTP to the VM at 10.0.2.2. A hang means a check is
  waiting for its 600 s timeout.
- `make test-upgrade FROM=IMAGE` (`tests/upgrade-test.py`) boots an older
  release image (qcow2, writes discarded), types its first-boot setup,
  runs `wh upgrade` against `out/repo` and checks the version, accounts,
  services and the busybox applets that moved to other packages.
- `make test-wormholes` (`tests/wormhole-test.py`) needs internet and takes
  several minutes: it sets up the Debian and Kali wormholes from the real
  archives.
- `tests/desktop-test.py` (also run by `make test`) adds a virtio GPU,
  keyboard and tablet and a QEMU monitor socket, types on tty1 with
  `sendkey`, reads the console text from `/dev/vcs1` over the serial
  console, and checks `screendump` output for the wallpaper's colours. It
  clicks on the background through QMP (`input-send-event` on the tablet)
  and compares screen regions, and resizes the screen through QEMU's VNC
  server (a minimal client sending ExtendedDesktopSize `SetDesktopSize`,
  which QEMU forwards to virtio-gpu like a window resize). With
  `--release IMAGE` it checks a release image: it types the first-boot
  setup blind, then logs in as root on the serial console with the
  password it set.
- It also types into the terminal to check zsh
  (Ctrl+Backspace, a suggestion from the history taken with End) and
  copies text both ways through a second VNC client using the extended
  clipboard.
- `tests/editions-test.py barebones|amnesia [ISO] [--release]` boots an
  edition's ISO with a GPU (the live desktop starts) and a monitor socket.
  Common checks (from memory, read-only, firewall, desktop as `live`),
  then Barebones: the variant, no Tor/Tor Browser/extra tools, the system
  commands all there, `worm anon on` asking for Tor, the network, the
  basic kit; Amnesia: the variant, no installer, the kernel options and
  lockdown, anonymous mode on, Tor as its own user, `worm anon off`
  refused, the session's `GTK_THEME`/`WORM_SHELL_PANEL`/`XDG_CONFIG_DIRS`,
  the grey wallpaper, the welcome text, the firewall's redirects. A web
  server and a UDP listener on the host must hear from Barebones and must
  hear nothing from Amnesia. A screendump's mean chroma (max minus min
  channel) must be low on Amnesia and not on Barebones. `--release` logs
  in as `live` (root is locked).
- `make test-iso` (`tests/iso-test.py [ISO]`) boots the ISO
  (`run-qemu.sh` with `ISO=`, an IDE CD with `bootindex=0`) with a fresh
  12 GiB qcow2 disk and 3 GiB of memory, logs in as root on the serial
  console of the live system (root has no password there), checks it,
  runs `install-system install vda`, then boots the disk without the ISO
  with UEFI and with BIOS firmware, and the ISO with BIOS firmware.
- `make test-qemu-windows` (`tests/qemu-windows-test.py`) runs the image in
  QEMU for Windows (`D:\qemu`), with the Windows Hypervisor Platform when
  available, UEFI and then BIOS firmware; serial console, monitor and QMP
  connect to the test over TCP the same way.
- Faster loop for snap: build natively (`make -C src/snap CC=gcc`) and use
  `--top` on a loopback btrfs image mounted with sudo.
