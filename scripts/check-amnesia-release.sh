#!/bin/bash
# Check a WormOS Amnesia release's files as amnesia-update will use them:
# SHA256SUMS matches the ISO, the update manifest's signature verifies with
# keys/core.pub, and the manifest names this ISO with its size and SHA-256.
#
# Usage: scripts/check-amnesia-release.sh DIR   (ISO, SHA256SUMS, amnesia.update, .sig)

. "$(dirname "$0")/env.sh"

dir=${1:?usage: check-amnesia-release.sh DIR}
iso=$(ls "$dir"/wormos-*-amnesia-x86_64.iso 2>/dev/null | head -1)
[ -n "$iso" ] && [ -f "$dir/SHA256SUMS" ] && [ -f "$dir/amnesia.update" ] &&
	[ -f "$dir/amnesia.update.sig" ] || die "$dir lacks the ISO, SHA256SUMS or the manifest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

(cd "$dir" && sha256sum -c --quiet --ignore-missing SHA256SUMS) || die "SHA256SUMS does not match"

eval "$(sed -n '/^\(name\|version\|source\|sha256\)=/p' "$WORMOS_ROOT/recipes/monocypher/recipe")"
fetch "${source[0]}" "${sha256[0]}"
unpack "$SOURCES/$(basename "${source[0]}")" "$tmp/mono"
cc -O2 -I"$tmp/mono/src" -I"$tmp/mono/src/optional" -o "$tmp/wsign" \
	"$WORMOS_ROOT/src/wh/wsign.c" "$WORMOS_ROOT/src/wh/sig.c" \
	"$tmp/mono/src/monocypher.c" "$tmp/mono/src/optional/monocypher-ed25519.c"
"$tmp/wsign" verify "$WORMOS_ROOT/keys/core.pub" "$dir/amnesia.update" > /dev/null ||
	die "the manifest's signature does not verify with keys/core.pub"

get() { sed -n "s/^$1 = //p" "$dir/amnesia.update" | head -1; }
[ "$(get iso)" = "$(basename "$iso")" ] || die "the manifest names $(get iso), not $(basename "$iso")"
[ "$(get size)" = "$(stat -c %s "$iso")" ] || die "the manifest gives another size for the ISO"
[ "$(get sha256)" = "$(sha256sum "$iso" | cut -d' ' -f1)" ] || die "the manifest gives another SHA-256"
# Since 1.11, update checks read amnesia.latest (the version alone).
if [ -f "$dir/amnesia.latest" ]; then
	"$tmp/wsign" verify "$WORMOS_ROOT/keys/core.pub" "$dir/amnesia.latest" > /dev/null ||
		die "amnesia.latest's signature does not verify with keys/core.pub"
	[ "$(sed -n 's/^version = //p' "$dir/amnesia.latest")" = "$(get version)" ] ||
		die "amnesia.latest names another version than the manifest"
fi
msg "WormOS Amnesia $(get version): the ISO and its signed manifest agree"
