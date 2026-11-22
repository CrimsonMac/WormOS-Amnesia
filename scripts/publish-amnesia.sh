#!/bin/bash
# Publish a WormOS Amnesia release: tag vVERSION and a GitHub release on
# the Amnesia repository with the ISO, SHA256SUMS, the update manifest,
# amnesia.latest (its version alone) and their signatures. amnesia-update
# reads them and the ISO's blocks from the latest release
# (releases/latest/download/), so every release must carry all of them.
#
# Needs the GitHub CLI (gh), logged in with write access, and the tag
# vVERSION pushed.
#
# Usage: scripts/publish-amnesia.sh VERSION NOTES-FILE (after make release-amnesia)

. "$(dirname "$0")/env.sh"

AMNESIA_REPO=${AMNESIA_REPO:-CrimsonMac/WormOS-Amnesia}
VERSION=${1:?usage: publish-amnesia.sh VERSION NOTES-FILE}
NOTES=${2:?usage: publish-amnesia.sh VERSION NOTES-FILE}
DEST=$OUT/release
iso=$DEST/wormos-$VERSION-amnesia-x86_64.iso

command -v gh > /dev/null || die "gh is missing (the GitHub CLI)"
[ -f "$iso" ] && [ -f "$DEST/amnesia.update" ] && [ -f "$DEST/amnesia.update.sig" ] &&
	[ -f "$DEST/SHA256SUMS" ] || die "no release $VERSION in $DEST (run make release-amnesia first)"
grep -qx "iso = $(basename "$iso")" "$DEST/amnesia.update" || die "the manifest names another ISO"
grep -qx "version = $VERSION" "$DEST/amnesia.update" || die "the manifest is not for $VERSION"
grep -qx "version = $VERSION" "$DEST/amnesia.latest" && [ -f "$DEST/amnesia.latest.sig" ] ||
	die "amnesia.latest is missing or not for $VERSION"
git -C "$WORMOS_ROOT" ls-remote --exit-code --tags origin "v$VERSION" > /dev/null ||
	die "push the tag v$VERSION first"
(cd "$DEST" && sha256sum -c --quiet SHA256SUMS) || die "SHA256SUMS does not match"

gh release create "v$VERSION" -R "$AMNESIA_REPO" --title "WormOS Amnesia $VERSION" \
	--notes-file "$NOTES" "$iso" "$DEST/SHA256SUMS" "$DEST/amnesia.update" "$DEST/amnesia.update.sig" \
	"$DEST/amnesia.latest" "$DEST/amnesia.latest.sig"
msg "Published: https://github.com/$AMNESIA_REPO/releases/tag/v$VERSION"
