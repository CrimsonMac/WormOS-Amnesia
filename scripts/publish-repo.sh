#!/bin/bash
# Publish the core package repository: out/repo (index, index.sig, the
# packages and their deltas, made here by make-deltas.sh) and out/sources
# (from scripts/source-archives.sh) become the assets of the release "core"
# of the public packages repository, the URL in /etc/wh/repo. Assets no longer in either folder are removed, so the
# release always holds exactly one repository state.
#
# Needs the GitHub CLI (gh), logged in with write access to $PACKAGES_REPO.
#
# Usage: scripts/publish-repo.sh

. "$(dirname "$0")/env.sh"

PACKAGES_REPO=${PACKAGES_REPO:-CrimsonMac/wormos-packages}
TAG=core

command -v gh > /dev/null || die "gh is missing (the GitHub CLI)"
[ -f "$REPO/index" ] && [ -f "$REPO/index.sig" ] || die "no signed index in $REPO"
[ -f "$OUT/sources/SHA256SUMS" ] || die "no source archives (run scripts/source-archives.sh)"
grep -qF "github.com/$PACKAGES_REPO/releases/download/$TAG/" \
	"$WORMOS_ROOT/recipes/wh/recipe" || die "/etc/wh/repo does not point at $PACKAGES_REPO"

if ! gh release view "$TAG" -R "$PACKAGES_REPO" > /dev/null 2>&1; then
	msg "Creating the release $TAG"
	gh release create "$TAG" -R "$PACKAGES_REPO" --title "Core packages" \
		--notes "The WormOS core package repository, used by wh. Source archives (*.src.tar) hold each package's recipe and upstream sources."
fi

# A package already published under the same name (version and release)
# stays as published: released images hold an index with its SHA-256, and
# different bytes for it would make wh refuse it there ("does not match
# the package index") until their index is updated. A rebuild of an
# unchanged package, or a change without a new release, keeps the
# published file; the index is then written again over what is kept.
declare -A published
while read -r name sum; do
	published[$name]=${sum#sha256:}
done < <(gh api "repos/$PACKAGES_REPO/releases/tags/$TAG" --paginate \
	--jq '.assets[] | .name + " " + (.digest // "")')
kept=0
for f in "$REPO"/*.wpkg; do
	name=$(basename "$f")
	sum=${published[$name]:-}
	[ -n "$sum" ] && [ "$sum" != "$(sha256sum "$f" | cut -c1-64)" ] || continue
	for try in 1 2 3; do
		gh release download "$TAG" -R "$PACKAGES_REPO" -p "$name" -O "$f.published" \
			--clobber && break
		[ "$try" = 3 ] && die "cannot download the published $name"
		sleep 10
	done
	[ "$(sha256sum "$f.published" | cut -c1-64)" = "$sum" ] || die "the published $name does not match its digest"
	mv "$f.published" "$f"
	kept=$((kept + 1))
done
if [ "$kept" -gt 0 ]; then
	msg "Kept $kept published packages whose version did not change; writing the index again"
	"$WORMOS_ROOT/tools/wrepo" --index-only
fi

# Deltas against the packages published now (only the files that changed).
"$WORMOS_ROOT/scripts/make-deltas.sh"

mapfile -t local_files < <(ls "$REPO"/*.wpkg "$REPO"/*.wdelta "$REPO/index" "$REPO/index.sig" \
	"$OUT/sources"/* 2>/dev/null)
# Name and SHA-256 of every asset already published (GitHub records each
# asset's digest), so unchanged files are not uploaded again.
declare -A digest
while read -r name sum; do
	digest[$name]=${sum#sha256:}
done < <(gh api "repos/$PACKAGES_REPO/releases/tags/$TAG" --paginate \
	--jq '.assets[] | .name + " " + (.digest // "")')
remote=("${!digest[@]}")

# GitHub's upload endpoint sometimes times out: try each file a few times.
upload() {
	local try
	for try in 1 2 3; do
		gh release upload "$TAG" -R "$PACKAGES_REPO" --clobber "$@" && return 0
		msg "Upload failed (attempt $try), trying again"
		sleep 10
	done
	die "could not upload $*"
}

# Packages first, the index and its signature last: a client never sees an
# index naming packages that are not there yet.
n=0
for f in "${local_files[@]}"; do
	name=$(basename "$f")
	case $name in index|index.sig) continue ;; esac
	[ "${digest[$name]:-}" = "$(sha256sum "$f" | cut -c1-64)" ] && continue
	upload "$f"
	n=$((n + 1))
done
msg "Uploaded $n changed files of ${#local_files[@]}"
upload "$REPO/index" "$REPO/index.sig"

declare -A keep
for f in "${local_files[@]}"; do
	keep[$(basename "$f")]=1
done
for name in "${remote[@]}"; do
	[ -n "${keep[$name]:-}" ] && continue
	msg "Removing $name"
	gh release delete-asset "$TAG" "$name" -R "$PACKAGES_REPO" -y
done
msg "Published to https://github.com/$PACKAGES_REPO/releases/tag/$TAG"
