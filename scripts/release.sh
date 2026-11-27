#!/bin/bash
# Build the release images of WormOS VERSION into out/release:
#
#   wormos-VERSION-x86_64.iso         live system and installer (CD/DVD or USB
#                                     stick, UEFI or BIOS firmware)
#   wormos-VERSION-x86_64.qcow2       ready-made disk for QEMU, virt-manager
#   wormos-VERSION-barebones-x86_64.iso  the operating system alone
#   wormos-VERSION-amnesia-x86_64.iso    live only, forgets everything, Tor
#   SHA256SUMS, SHA256SUMS.sig  checksums, signed with the WormOS key
#
# VARIANTS (default "barebones amnesia") picks the extra editions.
#
# Release images differ from development images in two ways: root has no
# password until the first-boot setup sets one, and the disk is larger
# (DISK_MB, default 20 GiB; the files stay small, the disk grows on use).
#
# Usage: scripts/release.sh VERSION

. "$(dirname "$0")/env.sh"

VERSION=${1:?usage: scripts/release.sh VERSION}
DISK_MB=${DISK_MB:-20480}
DEST=$OUT/release
NAME=wormos-$VERSION-x86_64

command -v qemu-img > /dev/null || die "qemu-img is missing (apt install qemu-utils)"
grep -qx "VERSION_ID=$VERSION" "$WORMOS_ROOT/recipes/base-files/files/etc/os-release" ||
	die "os-release does not say VERSION_ID=$VERSION"

rm -rf "$DEST"
mkdir -p "$DEST"

WORMOS_RELEASE=1 "$WORMOS_ROOT/scripts/build-rootfs.sh"
IMAGE=$DEST/$NAME.img DISK_MB=$DISK_MB "$WORMOS_ROOT/scripts/mkimage.sh"

msg "Writing $NAME.qcow2"
qemu-img convert -c -O qcow2 "$DEST/$NAME.img" "$DEST/$NAME.qcow2"

rm -f "$DEST/$NAME.img"

ISO=$DEST/$NAME.iso "$WORMOS_ROOT/scripts/mkiso.sh"

# The other editions: ISOs only.
for variant in ${VARIANTS-barebones amnesia}; do
	WORMOS_RELEASE=1 WORMOS_VARIANT=$variant "$WORMOS_ROOT/scripts/build-rootfs.sh"
	ISO=$DEST/wormos-$VERSION-$variant-x86_64.iso "$WORMOS_ROOT/scripts/mkiso.sh"
done

(cd "$DEST" && sha256sum *.iso "$NAME.qcow2" > SHA256SUMS)
# Signed with the key that signs the package index (keys/core.pub, .pem):
# scripts/verify-release.sh checks it, with OpenSSL alone.
[ -x "$BUILD/host/wsign" ] || die "no wsign (make repo builds it)"
"$BUILD/host/wsign" sign "${WORMOS_SIGNING_KEY:-$HOME/.config/wormos/core.key}" "$DEST/SHA256SUMS"
"$WORMOS_ROOT/scripts/verify-release.sh" "$DEST" > /dev/null || die "SHA256SUMS.sig does not verify"
msg "Release $VERSION ready in $DEST:"
ls -lh "$DEST"
