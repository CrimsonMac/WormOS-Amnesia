# WormOS build entry points. See docs/building.md for details.

# In build order: later packages build against earlier ones.
PACKAGES := base-files busybox worminit wormrd snap \
	zlib zstd openssl ca-certificates curl monocypher \
	libgpg-error npth libassuan libksba libgcrypt gnupg wormhole-keyrings \
	libmnl libnftnl nftables libxcrypt tzdata wh worm passt kvmtool bh expat unbound libevent tor \
	libpcap tcpdump dbus \
	kmod eudev seatd linux-firmware \
	libffi wayland wayland-protocols libdrm pixman xkeyboard-config \
	libxkbcommon libevdev mtdev libinput hwdata libdisplay-info mesa \
	libpng freetype fribidi fontconfig pcre2 glib harfbuzz libxml2 cairo pango \
	libepoxy xorgproto xcb-proto libxau xtrans libxcb xcb-util-wm libx11 libxext libxfixes \
	libxrender libxrandr libxkbfile xkbcomp libfontenc libxfont2 libxcvt libxshmfence xwayland \
	font-inter font-hack wlroots labwc tllist fcft foot grim swaybg wlr-randr xcompose clip \
	libjpeg-turbo libtiff shared-mime-info gdk-pixbuf graphene gtk4 \
	gtk4-layer-shell dconf hicolor-icon-theme adwaita-icon-theme wormos-gtk-theme \
	alsa-lib libsndfile pipewire lua wireplumber worm-shell wormos-snapshots \
	atk gtk3 tor-browser-runtime tor-browser \
	ncurses zsh-autosuggestions zsh-syntax-highlighting zsh \
	xz bzip2 readline bash less nano vim htop tmux file tree ncdu jq lsof strace \
	wireguard-tools sqlite libarchive git openssh rsync python nmap binutils \
	grep sed gawk findutils diffutils patch coreutils wget zip 7zip iproute2 ethtool \
	pciutils libusb usbutils dosfstools whois traceroute mtr iperf3 socat \
	gmp mpfr gdb e2fsprogs exfatprogs mc btop fastfetch lz4 pigz pv yara ssdeep \
	smartmontools hdparm dmidecode bc screen mtools hexedit links aria2 ngrep \
	macchanger torsocks proxychains-ng cmatrix nethogs ffmpeg sqlmap tool-menus \
	tor-browser-bundle licenses \
	wormos-desktop wormos-installer wormos-standard wormos-amnesia \
	wormos-login

all: image

toolchain:
	scripts/build-toolchain.sh

kernel: toolchain
	scripts/build-kernel.sh

packages: toolchain
	tools/wbuild $(PACKAGES)

repo: packages kernel
	tools/wrepo

rootfs: repo
	scripts/build-rootfs.sh

image: rootfs
	scripts/mkimage.sh

# The live and installation ISO (out/wormos.iso).
iso: rootfs
	scripts/mkiso.sh

# The other editions' ISOs (out/wormos-barebones.iso, out/wormos-amnesia.iso).
# They assemble the root filesystem again, as that edition.
iso-barebones: repo
	WORMOS_VARIANT=barebones scripts/build-rootfs.sh
	scripts/mkiso.sh

iso-amnesia: repo
	WORMOS_VARIANT=amnesia scripts/build-rootfs.sh
	scripts/mkiso.sh

run:
	scripts/run-qemu.sh

run-gui:
	scripts/run-qemu.sh --gui

test:
	tests/boot-test.py
	tests/desktop-test.py

# Needs internet access: sets up the Debian and Kali wormholes.
# DNS=ADDR uses that name server in the VM instead of QEMU's relay.
test-wormholes:
	tests/wormhole-test.py $(if $(DNS),--dns $(DNS))

# Needs internet access (several GB): installs every tool kit and graphical
# programs from Debian and Kali, starts every menu entry on the desktop and
# runs every exported command. KITS=blueteam,web picks kits.
test-apps:
	tests/apps-test.py $(if $(KITS),--kits $(KITS))

# Needs internet access: installs Tor Browser's libraries, downloads the
# browser from torproject.org and starts it.
test-torbrowser:
	tests/torbrowser-test.py

# Boots out/wormos.iso, checks the live system, installs it onto a disk and
# boots that.
test-iso:
	tests/iso-test.py

# Upgrades an older release image (FROM=wormos-1.3-x86_64.qcow2) to out/repo.
test-upgrade:
	tests/upgrade-test.py $(FROM)

# Deltas, unchanged files and the download cache, on out/wormos.img.
test-delta:
	tests/delta-test.py

# On a Windows PC, from WSL: QEMU for Windows.
test-qemu-windows:
	tests/qemu-windows-test.py

# The WormOS Amnesia release ISO in out/release (VERSION must match os-release).
release-amnesia: repo
	scripts/release-amnesia.sh $(VERSION)

# Release images in out/release (VERSION must match os-release).
release: repo
	scripts/release.sh $(VERSION)

# The public package repository: source archives, then the upload (gh).
sources: repo
	scripts/source-archives.sh

publish-repo: sources
	scripts/publish-repo.sh

clean:
	rm -rf build/work build/pkgs build/pkginfo build/cache build/fsroot build/kernel build/image build/host out

distclean:
	rm -rf build out

.PHONY: all toolchain kernel packages repo rootfs image iso iso-barebones iso-amnesia run run-gui test test-wormholes test-apps test-torbrowser test-upgrade test-delta test-iso test-qemu-windows release release-amnesia sources publish-repo clean distclean
