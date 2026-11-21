#!/bin/bash
# Build the WormOS Amnesia release ISO into out/release:
#
#   wormos-VERSION-amnesia-x86_64.iso   live only: forgets everything, Tor
#                                       from the first packet
#   SHA256SUMS
#
# VERSION is the WormOS version Amnesia is built on (os-release). Release
# builds lock root, which Amnesia then never unlocks.
#
# Usage: scripts/release-amnesia.sh VERSION

. "$(dirname "$0")/env.sh"

VERSION=${1:?usage: scripts/release-amnesia.sh VERSION}
DEST=$OUT/release
NAME=wormos-$VERSION-amnesia-x86_64

grep -qx "VERSION_ID=$VERSION" "$WORMOS_ROOT/recipes/base-files/files/etc/os-release" ||
	die "os-release does not say VERSION_ID=$VERSION"

rm -rf "$DEST"
mkdir -p "$DEST"
WORMOS_RELEASE=1 WORMOS_VARIANT=amnesia "$WORMOS_ROOT/scripts/build-rootfs.sh"
ISO=$DEST/$NAME.iso "$WORMOS_ROOT/scripts/mkiso.sh"
(cd "$DEST" && sha256sum "$NAME.iso" > SHA256SUMS)
msg "WormOS Amnesia $VERSION ready in $DEST:"
ls -lh "$DEST"
