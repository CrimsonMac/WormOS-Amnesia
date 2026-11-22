#!/bin/bash
# Make deltas for the packages in out/repo that changed since the published
# repository: for each, tools/wdelta keeps only the files that differ from
# the published version, and the index gains a line
#   delta = OLD-VERSION FILE SIZE SHA256
# for it and is signed again. wh (1.11 and later) downloads the delta
# instead of the full package when the published version is the one
# installed. Deltas that would save less than a third are left out.
# publish-repo.sh runs this before uploading.
#
# Usage: scripts/make-deltas.sh
#   (DELTA_REPO, DELTA_BASE_URL and DELTA_WORK change the folder, the
#   repository the deltas start from and the work folder, for tests)

. "$(dirname "$0")/env.sh"

SECRET=${WORMOS_SIGNING_KEY:-$HOME/.config/wormos/core.key}
PUBLIC=$WORMOS_ROOT/keys/core.pub
WSIGN=$BUILD/host/wsign
PACKAGES_REPO=${PACKAGES_REPO:-CrimsonMac/wormos-packages}
url=$(grep -o "https://github.com/$PACKAGES_REPO/releases/download/core/" \
	"$WORMOS_ROOT/recipes/wh/recipe" | head -1)
[ -n "$url" ] || die "/etc/wh/repo does not point at $PACKAGES_REPO"
# Tests make deltas for another folder against another repository.
REPO=${DELTA_REPO:-$REPO}
url=${DELTA_BASE_URL:-$url}
base=${DELTA_WORK:-$BUILD/deltas}
[ -x "$WSIGN" ] || die "no $WSIGN (run tools/wrepo first)"
[ -f "$REPO/index" ] || die "no index in $REPO"

mkdir -p "$base"
rm -f "$REPO"/*.wdelta
curl -fsSL --retry 3 -o "$base/index" "${url}index"
curl -fsSL --retry 3 -o "$base/index.sig" "${url}index.sig"
"$WSIGN" verify "$PUBLIC" "$base/index" > /dev/null ||
	die "the published index does not verify"

# NAME VERSION FILE SHA256 of every package in an index.
entries() {
	awk '/^\[/ { if (n) print n, v, f, s; n = substr($0, 2, length($0) - 2); v = f = s = "" }
	     /^version = / { v = $3 } /^file = / { f = $3 } /^sha256 = / { s = $3 }
	     END { if (n) print n, v, f, s }' "$1"
}
declare -A old_version old_file old_sum
while read -r name version file sum; do
	old_version[$name]=$version old_file[$name]=$file old_sum[$name]=$sum
done < <(entries "$base/index")

: > "$base/lines"
made=0 saved=0
while read -r name version file sum; do
	ov=${old_version[$name]:-}
	[ -n "$ov" ] && [ "$ov" != "$version" ] || continue
	old=$base/${old_file[$name]}
	if [ ! -f "$old" ] || [ "$(sha256sum "$old" | cut -c1-64)" != "${old_sum[$name]}" ]; then
		curl -fsSL --retry 3 -o "$old.part" "$url${old_file[$name]}" || die "cannot download ${old_file[$name]}"
		[ "$(sha256sum "$old.part" | cut -c1-64)" = "${old_sum[$name]}" ] ||
			die "${old_file[$name]} does not match the published index"
		mv "$old.part" "$old"
	fi
	delta=${file%.wpkg}.from-$ov.wdelta
	"$WORMOS_ROOT/tools/wdelta" "$old" "$REPO/$file" "$REPO/$delta" > /dev/null
	full=$(stat -c %s "$REPO/$file")
	size=$(stat -c %s "$REPO/$delta")
	if [ $((size * 3)) -gt $((full * 2)) ]; then
		rm -f "$REPO/$delta"
		continue
	fi
	printf '%s %s %s %s %s\n' "$name" "$ov" "$delta" "$size" \
		"$(sha256sum "$REPO/$delta" | cut -c1-64)" >> "$base/lines"
	made=$((made + 1)) saved=$((saved + full - size))
done < <(entries "$REPO/index")

# The index with a delta line in each package that has one, signed again.
awk 'NR == FNR { d[$1] = $2 " " $3 " " $4 " " $5; next }
     /^\[/ { name = substr($0, 2, length($0) - 2) }
     { print }
     /^size = / && (name in d) { print "delta = " d[name] }' \
	"$base/lines" <(grep -v '^delta = ' "$REPO/index") > "$REPO/index.part"
mv "$REPO/index.part" "$REPO/index"
rm -f "$REPO/index.sig"
"$WSIGN" sign "$SECRET" "$REPO/index"
"$WSIGN" verify "$PUBLIC" "$REPO/index" > /dev/null || die "the new index does not verify"
# Published packages no longer needed as a base.
for f in "$base"/*.wpkg; do
	[ -e "$f" ] || continue
	grep -qxF "file = $(basename "$f")" "$base/index" || rm -f "$f"
done
msg "$made deltas, $((saved / 1048576)) MB less to download than the full packages"
