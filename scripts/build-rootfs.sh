#!/bin/bash
# Assemble the root filesystem by installing packages from the repository.
#
# Runs under fakeroot so files can be owned by root without real privileges.
# The ownership database is saved to build/fakeroot.state for mkimage.sh.
#
# The result is one directory per btrfs subvolume under build/fsroot:
#   @           the system root, mounted read-only
#   @var        /var, writable state and logs (not part of system snapshots)
#   @home       /home
#   @wormholes  containers of other distributions (/var/lib/wormholes)
#   @snapshots  snapshot store, never mounted by the running system

. "$(dirname "$0")/env.sh"

STATE=$BUILD/fakeroot.state
if [ -z "${FAKEROOTKEY:-}" ]; then
	rm -f "$STATE"
	exec fakeroot -s "$STATE" -- "$0" "$@"
fi

# The base system. Packages come from the signed repository (tools/wrepo)
# and are installed with wh, as on a running system.
#
# WORMOS_VARIANT picks the edition:
#   standard   (default) everything below
#   barebones  the operating system alone: no Tor, no Tor Browser, none of
#              the everyday tools (worm kit install basic adds them)
#   amnesia    the barebones system with Tor and Tor Browser and the amnesia
#              settings (wormos-amnesia): live only, nothing kept, all
#              traffic through Tor from the start
VARIANT=${WORMOS_VARIANT:-standard}
SYSTEM=(base-files glibc gcc-libs busybox worminit wormrd snap wh worm linux
	zstd curl gnupg wormhole-keyrings libmnl libnftnl nftables bh expat unbound
	kmod eudev seatd linux-firmware dbus wormos-login licenses tzdata cryptsetup
	wormos-desktop labwc wlroots mesa foot swaybg grim wlr-randr xcompose font-inter font-hack
	nano
	# GNU's versions of the everyday commands: BusyBox keeps only the shell,
	# DHCP, the console login and a few system tools.
	coreutils grep sed gawk findutils diffutils)
# The standard edition's programs come with wormos-standard, which
# recommends them (each stays removable on its own, and upgrades add what
# new releases bring).
case $VARIANT in
standard)  PACKAGES=("${SYSTEM[@]}" wormos-standard) ;;
barebones) PACKAGES=("${SYSTEM[@]}" wormos-installer) ;;
amnesia)   PACKAGES=("${SYSTEM[@]}" wormos-amnesia) ;;
*)         die "unknown variant '$VARIANT' (standard, barebones or amnesia)" ;;
esac
WH=$BUILD/host/wh

[ -x "$WH" ] && [ -f "$REPO/index" ] || die "run tools/wrepo first"

msg "Assembling root filesystem ($VARIANT)"
rm -rf "$FSROOT"
mkdir -p "$ROOTFS" "$FSROOT/@home" "$FSROOT/@snapshots" "$FSROOT/@wormholes"

cd "$ROOTFS" || die "cannot enter $ROOTFS"
mkdir -p boot dev etc home mnt proc root run sys tmp \
	usr/bin usr/lib usr/sbin usr/share \
	var/cache var/lib var/lib/blackholes var/lib/wormholes var/log var/spool var/roothome \
	var/usrlocal/bin
ln -s usr/bin bin
ln -s usr/sbin sbin
ln -s usr/lib lib
ln -s usr/lib lib64
ln -s lib usr/lib64
ln -s ../run var/run
ln -s ../run/lock var/lock
ln -s ../tmp var/tmp
# The root is read-only: locally added commands (such as the ones wh exports
# from wormholes) live on /var.
ln -s ../var/usrlocal usr/local

wh() { "$WH" --root "$ROOTFS" --repo "file://$REPO/" --key "$WORMOS_ROOT/keys/core.pub" "$@"; }
wh update
wh install "${PACKAGES[@]}"
# The edition, for the welcome text and the ISO (amnesia's comes with its
# package).
if [ "$VARIANT" = barebones ]; then
	cat > "$ROOTFS/etc/worm/variant" <<-EOF
	# The WormOS edition this system was built as.
	VARIANT_ID=barebones
	VARIANT="Barebones"
	EOF
fi
# Amnesia's index includes wormos-amnesia, so it is signed here, after the
# published core index; wh would refuse the published one as older and the
# live user could install nothing. Its date is set to 0: any published index
# is newer, and wh refuses older ones from the first update on.
if [ "$VARIANT" = amnesia ]; then
	sed -i 's/^date = .*/date = 0/' "$ROOTFS/var/lib/wh/index"
	"$BUILD/host/wsign" sign "${WORMOS_SIGNING_KEY:-$HOME/.config/wormos/core.key}" \
		"$ROOTFS/var/lib/wh/index"
	"$BUILD/host/wsign" verify "$WORMOS_ROOT/keys/core.pub" "$ROOTFS/var/lib/wh/index" > /dev/null ||
		die "the image's index does not verify"
fi
# The image keeps the index (newer ones must be at least as recent) but not
# the downloaded packages.
rm -rf "$ROOTFS/var/cache/wh"

"$WORMOS_ROOT/scripts/build-initramfs.sh"

# /var becomes its own subvolume; the root keeps an empty mount point.
mv "$ROOTFS/var" "$FSROOT/@var"
mkdir "$ROOTFS/var"

chown -R 0:0 "$FSROOT"
chmod 0700 root "$FSROOT/@var/roothome" "$FSROOT/@snapshots"
# Users run wormhole programs, so they must be able to reach them; the
# mount is nosuid, so set-ID files inside grant nothing.
chmod 0755 "$FSROOT/@wormholes"
# Shared like /tmp: each user's blackholes live in a directory of their own.
chmod 1777 "$FSROOT/@var/lib/blackholes"
chmod 1777 tmp
chmod 0600 etc/shadow
# Release images: root cannot log in until the first-boot setup has set its
# password. (Development images keep an empty one for the tests.)
if [ "${WORMOS_RELEASE:-}" = 1 ]; then
	sed -i 's/^root::/root:!:/' etc/shadow
fi

msg "Root filesystem ready ($(du -sh "$ROOTFS" | cut -f1))"
