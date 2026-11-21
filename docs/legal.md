# Legal notes

## Licenses

WormOS's own code is licensed under the GNU General Public License,
version 3 or later (`LICENSE` in this repository, and
`/usr/share/licenses/wormos/COPYING` on an installed system). That covers
the init system (`worminit`, `wormrd`), `snap`, `wh`, `worm`, `bh`, the
desktop shell, the login screen, the snapshot timeline, the Tor Browser
launcher, the installer, the recipes, the build scripts and the tests.

Everything else in a WormOS image is built from upstream projects under
their own licenses, among them:

- the Linux kernel (GPL-2.0), GNU libc and GCC's runtime libraries
  (LGPL-2.1 and GPL-3.0 with the GCC Runtime Library Exception), BusyBox
  (GPL-2.0), GnuPG and its libraries (GPL/LGPL), zsh and its plugins
  (MIT-style and BSD), bash, nano, readline, GNU coreutils, grep, sed,
  gawk, findutils, diffutils, patch and wget (GPL-3.0), iproute2,
  ethtool, pciutils and usbutils (GPL-2.0);
- 7-Zip (LGPL-2.1, with BSD-licensed parts and the unRAR license's
  restriction for its RAR decoder: it may not be used to rebuild a RAR
  compressor), Info-ZIP zip (Info-ZIP license);
- GTK, GLib, Pango, PipeWire, WirePlumber and other desktop libraries
  (LGPL), labwc and wlroots (GPL-2.0 and MIT), foot (MIT), Mesa (MIT);
- OpenSSL (Apache-2.0), curl (curl license), Tor (BSD-3-Clause), unbound
  (BSD-3-Clause), nftables (GPL-2.0), OpenSSH (BSD-style), Python (PSF);
- fonts: Inter (SIL Open Font License 1.1) and Hack (MIT, with Bitstream
  Vera parts);
- icons: Adwaita (LGPL-3.0 or CC-BY-SA-3.0);
- Intel GPU firmware from linux-firmware, redistributable under its own
  license (`/usr/share/licenses/linux-firmware`);
- the list of trusted certificate authorities from Mozilla (MPL-2.0).

The full texts are installed in `/usr/share/licenses/NAME` (the
`licenses` package collects them from the upstream archives the recipes
use; packages that install their own keep them).

### Source code

For every package built from upstream code, the package repository
([CrimsonMac/wormos-packages](https://github.com/CrimsonMac/wormos-packages/releases/tag/core))
publishes a source archive, `NAME-VERSION-RELEASE.src.tar`, next to the
binary package: the recipe, the WormOS patches and the unmodified upstream
archives, which together are the complete corresponding source. The build
scripts and the toolchain recipe are in `wormos-build-tools.tar` there.
These archives stay available for as long as the binary packages are
offered. The source of WormOS's own programs (init, snapshots, packages,
desktop shell and the rest) is this repository.

### Tor Browser

Tor Browser is included unmodified, as published and signed by The Tor
Project (its signature is checked when the image is built). Its licenses
are in `/opt/tor-browser/Browser/TorBrowser/Docs/Licenses`. WormOS's
launcher (`tor-browser`) and its icon are WormOS's own and do not use the
Tor logo.

### Software from Debian, Kali and tool kits

`wh install` and `worm kit install` download packages from Debian and
Kali at the user's request, from those projects' own archives, into
containers on the user's machine. That software is under its own licenses
and is not distributed by WormOS.

Some tool kits (passwords, web testing, penetration testing) contain tools
that can be used to attack computer systems. Using them against systems
or data without the owner's permission is illegal in most countries. They
are offered for security testing you are authorised to do, for teaching
and for defence.

## Trademarks

Linux is a registered trademark of Linus Torvalds. Debian is a registered
trademark of Software in the Public Interest, Inc. Kali Linux is a
trademark of OffSec Services Limited. Tor and the Tor onion logo are
trademarks of The Tor Project, Inc. Intel is a trademark of Intel
Corporation. GitHub is a trademark of GitHub, Inc. Other names belong to
their owners. WormOS is not affiliated with or endorsed by any of them;
the names are used only to say what WormOS works with.

## Privacy

WormOS collects nothing and has no telemetry, crash reporting or usage
statistics. The system contacts the network only:

- for DNS: the local resolver sends queries encrypted (DNS over TLS) to
  Quad9 (`9.9.9.9`, `149.112.112.112` and their IPv6 counterparts), or
  through Tor in anonymous mode;
- for DHCP, to get an address from the local network (without sending a
  host name or a client name);
- to check the clock: every six hours, a HEAD request over HTTPS to
  www.quad9.net (or www.torproject.org, www.debian.org if it does not
  answer), whose `Date` header is compared with the system clock;
- when you run `wh` or `worm kit`: the WormOS package repository on
  GitHub, and Debian's and Kali's archives for software from them;
- in anonymous mode: the Tor network;
- when you use Tor Browser: whatever you browse to, through Tor, and Tor
  Browser's own update checks; on a system without the bundled copy, its
  first start downloads it from torproject.org.

The services you contact (GitHub, Debian, Kali, Quad9, The Tor Project)
have their own privacy policies.

## No warranty

WormOS is provided "as is", without warranty of any kind, as stated in
sections 15 and 16 of the GNU General Public License. Snapshots,
blackholes, the firewall and anonymous mode reduce risk, within the limits
the README states; they do not guarantee that data cannot be lost, that
untrusted software cannot escape, or that you cannot be identified.

## Export

WormOS contains cryptographic software (OpenSSL, GnuPG, Tor, WireGuard
tools, OpenSSH). Some countries restrict its import, use or export; check
the rules that apply to you.
