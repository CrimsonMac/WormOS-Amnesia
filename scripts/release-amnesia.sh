#!/bin/bash
# Build the WormOS Amnesia release ISO into out/release:
#
#   wormos-VERSION-amnesia-x86_64.iso   live only: forgets everything, Tor
#                                       from the first packet
#   amnesia.update, .sig                the signed update manifest
#   amnesia.latest, .sig                its version alone (for checks)
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
# The update manifest, signed with the WormOS key: amnesia-update on a
# running Amnesia uses it to bring its stick up to this version.
"$WORMOS_ROOT/tools/amnesia-manifest" "$DEST/$NAME.iso" "$VERSION" "$DEST/amnesia.update"
SECRET=${WORMOS_SIGNING_KEY:-$HOME/.config/wormos/core.key}
"$BUILD/host/wsign" sign "$SECRET" "$DEST/amnesia.update"
"$BUILD/host/wsign" verify "$WORMOS_ROOT/keys/core.pub" "$DEST/amnesia.update" > /dev/null ||
	die "the update manifest does not verify"
# Just the version, also signed: update checks read this instead of the
# whole manifest (which has a line for every block).
printf 'version = %s\n' "$VERSION" > "$DEST/amnesia.latest"
"$BUILD/host/wsign" sign "$SECRET" "$DEST/amnesia.latest"
(cd "$DEST" && sha256sum "$NAME.iso" > SHA256SUMS)
# Signed like WormOS's (scripts/verify-release.sh checks it with OpenSSL).
"$BUILD/host/wsign" sign "$SECRET" "$DEST/SHA256SUMS"
"$WORMOS_ROOT/scripts/verify-release.sh" "$DEST" > /dev/null || die "SHA256SUMS.sig does not verify"
msg "WormOS Amnesia $VERSION ready in $DEST:"
ls -lh "$DEST"
