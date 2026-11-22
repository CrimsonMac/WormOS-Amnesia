#!/bin/bash
# Write the source archives that go with the public package repository
# into out/sources:
#
#   NAME-VERSION-RELEASE.src.tar   for every package in out/repo built from
#                                  upstream sources: the recipe directory
#                                  (recipe, patches, extra files) and the
#                                  upstream archives it builds from
#   wormos-build-tools.tar         the build scripts all recipes run under
#
# Packages built from this repository's own code (localsrc, or no sources)
# get none, nor does wormos-amnesia: mostly WormOS's own files, with GTK's
# theme sources (published with wormos-gtk-theme) only for its theme. linux-firmware gets its recipe only: the firmware files are
# shipped as they are, with their licences, and the full upstream archive
# is over 600 MB.
#
# Usage: scripts/source-archives.sh

. "$(dirname "$0")/env.sh"

DEST=$OUT/sources
STAGE=$BUILD/source-archives

ls "$REPO"/*.wpkg > /dev/null 2>&1 || die "no packages in $REPO (run make repo)"
rm -rf "$DEST" "$STAGE"
mkdir -p "$DEST" "$STAGE"

# recipe_sources NAME: the source file names of recipes/NAME, one per line;
# nothing for packages built from local code.
recipe_sources() {
	(
		source=() localsrc=
		. "$WORMOS_ROOT/recipes/$1/recipe"
		[ -z "$localsrc" ] || exit 0
		for url in "${source[@]}"; do
			srcname "$url"
		done
	)
}

count=0
for wpkg in "$REPO"/*.wpkg; do
	base=$(basename "$wpkg" .wpkg)       # NAME-VERSION-RELEASE
	name=${base%-*}
	name=${name%-*}
	files=()
	case $name in
	wormos-amnesia)
		continue ;;
	glibc)
		files=("$SOURCES/glibc-2.44.tar.xz") ;;
	gcc-libs)
		files=("$SOURCES/gcc-15.3.0.tar.xz") ;;
	linux)
		# The kernel may be newer than the toolchain's headers.
		files=("$SOURCES/$(sed -n 's/^LINUX=//p' "$WORMOS_ROOT/scripts/build-kernel.sh").tar.xz"
			"$WORMOS_ROOT/kernel/wormos.config" "$WORMOS_ROOT/kernel/patches") ;;
	linux-headers)
		files=("$SOURCES/$(sed -n 's/^LINUX=//p' "$WORMOS_ROOT/scripts/build-toolchain.sh").tar.xz") ;;
	*)
		[ -f "$WORMOS_ROOT/recipes/$name/recipe" ] ||
			die "no recipe for $name"
		mapfile -t srcs < <(recipe_sources "$name")
		[ ${#srcs[@]} -gt 0 ] || continue
		if [ "$name" != linux-firmware ]; then
			for src in "${srcs[@]}"; do
				files+=("$SOURCES/$src")
			done
		fi
		files+=("$WORMOS_ROOT/recipes/$name") ;;
	esac

	dir=$STAGE/$base
	mkdir -p "$dir"
	for f in "${files[@]}"; do
		[ -e "$f" ] || die "$name: $f is missing (run make repo first)"
		cp -a "$f" "$dir/"
	done
	tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
		-cf "$DEST/$base.src.tar" -C "$STAGE" "$base"
	rm -rf "$dir"
	count=$((count + 1))
done

# The scripts that drive every build (cross toolchain, kernel, recipes,
# packages and the signed index).
mkdir -p "$STAGE/wormos-build-tools/scripts" "$STAGE/wormos-build-tools/tools" \
	"$STAGE/wormos-build-tools/toolchain" "$STAGE/wormos-build-tools/kernel"
cp "$WORMOS_ROOT"/scripts/{env.sh,build-toolchain.sh,build-kernel.sh} \
	"$STAGE/wormos-build-tools/scripts/"
cp "$WORMOS_ROOT"/tools/{wbuild,wpack,wrepo} "$STAGE/wormos-build-tools/tools/"
cp "$WORMOS_ROOT/toolchain/sources" "$STAGE/wormos-build-tools/toolchain/"
cp -r "$WORMOS_ROOT/kernel/wormos.config" "$WORMOS_ROOT/kernel/patches" \
	"$STAGE/wormos-build-tools/kernel/"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
	-cf "$DEST/wormos-build-tools.tar" -C "$STAGE" wormos-build-tools
rm -rf "$STAGE"

(cd "$DEST" && sha256sum -- * > SHA256SUMS)
msg "$count source archives in $DEST ($(du -sh "$DEST" | cut -f1))"
