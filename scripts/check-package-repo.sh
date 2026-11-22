#!/bin/bash
# Check the published package repository the way a WormOS system sees it:
# the index's signature verifies with the key in keys/core.pub, every
# package it lists is there with the size and SHA-256 it gives (GitHub
# records each asset's digest, so nothing is downloaded but the index),
# so are the deltas it names, every dependency is in the index, and every
# package built from upstream sources has its source archive next to it.
# CI runs it every day.
#
# Needs curl, a C compiler and the GitHub CLI (gh).
#
# Usage: scripts/check-package-repo.sh

. "$(dirname "$0")/env.sh"

PACKAGES_REPO=${PACKAGES_REPO:-CrimsonMac/wormos-packages}
TAG=core
url=$(grep -o "https://github.com/$PACKAGES_REPO/releases/download/$TAG/" "$WORMOS_ROOT/recipes/wh/recipe" | head -1)
[ -n "$url" ] || die "/etc/wh/repo does not point at $PACKAGES_REPO"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0
fail() { printf 'FAIL  %s\n' "$*"; failures=$((failures + 1)); }

# wsign, with Monocypher from its recipe's pinned source.
eval "$(sed -n '/^\(name\|version\|source\|sha256\)=/p' "$WORMOS_ROOT/recipes/monocypher/recipe")"
fetch "${source[0]}" "${sha256[0]}"
unpack "$SOURCES/$(basename "${source[0]}")" "$tmp/mono"
cc -O2 -I"$tmp/mono/src" -I"$tmp/mono/src/optional" -o "$tmp/wsign" \
	"$WORMOS_ROOT/src/wh/wsign.c" "$WORMOS_ROOT/src/wh/sig.c" \
	"$tmp/mono/src/monocypher.c" "$tmp/mono/src/optional/monocypher-ed25519.c"

msg "Index and signature"
curl -fsSL --retry 3 -o "$tmp/index" "${url}index"
curl -fsSL --retry 3 -o "$tmp/index.sig" "${url}index.sig"
"$tmp/wsign" verify "$WORMOS_ROOT/keys/core.pub" "$tmp/index" > /dev/null ||
	fail "the index's signature does not verify with keys/core.pub"
date=$(sed -n 's/^date = //p' "$tmp/index")
printf '  signed %s, %s packages\n' "$(date -u -d "@$date" '+%Y-%m-%d %H:%M UTC')" \
	"$(grep -c '^\[' "$tmp/index")"

msg "Packages against the published files"
gh api "repos/$PACKAGES_REPO/releases/tags/$TAG" --paginate \
	--jq '.assets[] | .name + " " + (.size | tostring) + " " + ((.digest // "") | sub("^sha256:"; ""))' \
	> "$tmp/assets"
declare -A asset_size asset_sum in_index
while read -r name size sum; do
	asset_size[$name]=$size
	asset_sum[$name]=$sum
done < "$tmp/assets"

pkg="" file="" sum="" size="" deps="" delta=""
check_entry() {
	[ -n "$pkg" ] || return 0
	in_index[$pkg]=1
	if [ -z "${asset_size[$file]:-}" ]; then
		fail "$pkg: $file is not published"
	elif [ "${asset_size[$file]}" != "$size" ]; then
		fail "$pkg: $file is ${asset_size[$file]} bytes, the index says $size"
	elif [ -n "${asset_sum[$file]}" ] && [ "${asset_sum[$file]}" != "$sum" ]; then
		fail "$pkg: $file does not have the SHA-256 the index gives"
	fi
	# A delta (only the files changed since an older version) must match too.
	if [ -n "$delta" ]; then
		set -- $delta
		if [ $# -ne 4 ] || [ "${asset_size[$2]:-}" != "$3" ]; then
			fail "$pkg: its delta $2 is missing or has another size"
		elif [ -n "${asset_sum[$2]}" ] && [ "${asset_sum[$2]}" != "$4" ]; then
			fail "$pkg: its delta $2 does not have the SHA-256 the index gives"
		fi
	fi
	printf '%s %s\n' "$pkg" "$deps" >> "$tmp/deps"
	# Built from upstream sources: its source archive must be there too.
	local r=$WORMOS_ROOT/recipes/$pkg/recipe
	if [ -f "$r" ] && grep -q '^source=(' "$r" && ! grep -q '^localsrc=' "$r" &&
	   [ -z "${asset_size[${file%.wpkg}.src.tar]:-}" ]; then
		fail "$pkg: no source archive ${file%.wpkg}.src.tar"
	fi
}
while IFS= read -r line; do
	case $line in
	'['*']') check_entry; pkg=${line#[}; pkg=${pkg%]}; file="" sum="" size="" deps="" delta="" ;;
	'delta = '*) delta=${line#delta = } ;;
	'file = '*) file=${line#file = } ;;
	'sha256 = '*) sum=${line#sha256 = } ;;
	'size = '*) size=${line#size = } ;;
	'depends = '*) deps=${line#depends = } ;;
	esac
done < "$tmp/index"
check_entry

msg "Dependencies"
while read -r pkg deps; do
	for d in $deps; do
		d=${d%%:*}
		[ -n "${in_index[$d]:-}" ] || fail "$pkg depends on $d, which is not in the index"
	done
done < "$tmp/deps"

if [ "$failures" -gt 0 ]; then
	printf '%d problems\n' "$failures"
	exit 1
fi
msg "The package repository is consistent"
