#!/bin/bash
# Publish a WormOS Amnesia release for amnesia-update: the ISO, its update
# manifest and the manifest's signature become the assets of the release
# "amnesia" of the public packages repository (the URL built into
# amnesia-update). Older ISOs there are removed: the updater needs only
# the latest.
#
# Needs the GitHub CLI (gh), logged in with write access.
#
# Usage: scripts/publish-amnesia.sh (after make release-amnesia)

. "$(dirname "$0")/env.sh"

PACKAGES_REPO=${PACKAGES_REPO:-CrimsonMac/wormos-packages}
TAG=amnesia
DEST=$OUT/release

command -v gh > /dev/null || die "gh is missing (the GitHub CLI)"
iso=$(ls "$DEST"/wormos-*-amnesia-x86_64.iso 2>/dev/null | head -1)
[ -n "$iso" ] && [ -f "$DEST/amnesia.update" ] && [ -f "$DEST/amnesia.update.sig" ] ||
	die "no release in $DEST (run make release-amnesia first)"
grep -qx "iso = $(basename "$iso")" "$DEST/amnesia.update" || die "the manifest names another ISO"

if ! gh release view "$TAG" -R "$PACKAGES_REPO" > /dev/null 2>&1; then
	msg "Creating the release $TAG"
	gh release create "$TAG" -R "$PACKAGES_REPO" --title "WormOS Amnesia" \
		--notes "The latest WormOS Amnesia ISO and its signed update manifest, used by amnesia-update to bring an Amnesia USB stick up to date by downloading only what changed."
fi
msg "Uploading $(basename "$iso")"
gh release upload "$TAG" -R "$PACKAGES_REPO" --clobber "$iso" "$DEST/amnesia.update" "$DEST/amnesia.update.sig"
for old in $(gh release view "$TAG" -R "$PACKAGES_REPO" --json assets --jq '.assets[].name' | grep -- '-amnesia-x86_64\.iso$'); do
	[ "$old" = "$(basename "$iso")" ] || gh release delete-asset "$TAG" "$old" -R "$PACKAGES_REPO" -y
done
msg "Published: https://github.com/$PACKAGES_REPO/releases/tag/$TAG"
