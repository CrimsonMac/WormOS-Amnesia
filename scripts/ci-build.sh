#!/bin/bash
# Build WormOS's own programs with the build machine's compiler, warnings as
# errors, then run the unit tests (tests/unit). Needs no toolchain and no
# image: CI runs it on every push. The GUI programs need the GTK 4, gtk4-
# layer-shell and Wayland development files; without them they are skipped
# (CI installs them, so there nothing is skipped).
#
# With SANITIZE=1 the unit tests also run under AddressSanitizer and
# UndefinedBehaviorSanitizer (any report fails them).
#
# Usage: [SANITIZE=1] scripts/ci-build.sh [CC]

. "$(dirname "$0")/env.sh"
set +u

CC=${1:-${CC:-cc}}
# Truncation warnings guess that a short name could be 100 KB long: shown,
# not fatal.
CFLAGS="-O2 -D_FORTIFY_SOURCE=3 -Werror -Wno-error=format-truncation"
[ "$(basename "$CC")" = clang ] && CFLAGS="$CFLAGS -Wno-unknown-warning-option"
dest=$BUILD/ci
mkdir -p "$dest"
work=$(mktemp -d "$dest/build.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Monocypher for wh, from its recipe's pinned source (as tools/wrepo does).
eval "$(sed -n '/^\(name\|version\|source\|sha256\)=/p' "$WORMOS_ROOT/recipes/monocypher/recipe")"
fetch "${source[0]}" "${sha256[0]}"
mono=$work/monocypher
unpack "$SOURCES/$(basename "${source[0]}")" "$mono"

# gtk4-layer-shell, from its recipe's pinned source, where the build
# machine has GTK 4 but no package of it (Ubuntu 24.04).
if pkg-config --exists gtk4 && ! pkg-config --exists gtk4-layer-shell-0 && command -v meson > /dev/null; then
	msg "Building gtk4-layer-shell (no package of it here)"
	eval "$(sed -n '/^\(name\|version\|source\|sha256\)=/p' "$WORMOS_ROOT/recipes/gtk4-layer-shell/recipe")"
	fetch "${source[0]}" "${sha256[0]}"
	unpack "$SOURCES/${source[0]%%::*}" "$work/gtk4-layer-shell"
	(cd "$work/gtk4-layer-shell" &&
		meson setup build --prefix="$work/prefix" --libdir=lib -Dexamples=false -Ddocs=false \
			-Dtests=false -Dsmoke-tests=false -Dintrospection=false -Dvapi=false > /dev/null &&
		ninja -C build install > /dev/null) || die "gtk4-layer-shell did not build"
	export PKG_CONFIG_PATH=$work/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
fi

# CI_IN_PLACE=1 builds in the checkout itself (CodeQL needs the sources
# where it saw them compiled); otherwise in a copy, leaving the tree clean.
srcroot=$work
if [ "${CI_IN_PLACE:-0}" = 1 ]; then
	srcroot=$WORMOS_ROOT
else
	cp -r "$WORMOS_ROOT/src" "$work/src"
fi
failed=()
for dir in "$srcroot"/src/*/; do
	name=$(basename "$dir")
	[ -f "$dir/Makefile" ] || continue
	if grep -q 'PKG_CONFIG' "$dir/Makefile" && ! pkg-config --exists gtk4 wayland-client 2>/dev/null; then
		msg "Skipped $name (no GTK 4 or Wayland development files)"
		continue
	fi
	msg "Building $name"
	make -s -C "$dir" CC="$CC" CFLAGS="$CFLAGS" \
		MONO_CFLAGS="-I$mono/src -I$mono/src/optional" \
		MONO_LIBS="$mono/src/monocypher.c $mono/src/optional/monocypher-ed25519.c" ||
		failed+=("$name")
done

msg "Unit tests"
cp -r "$WORMOS_ROOT/tests" "$work/tests"
unit_cflags="-O2 -D_FORTIFY_SOURCE=3"
if [ "${SANITIZE:-0}" = 1 ]; then
	unit_cflags="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
	export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1
	msg "Unit tests run under AddressSanitizer and UndefinedBehaviorSanitizer"
fi
make -s -C "$work/tests/unit" CC="$CC" CFLAGS="$unit_cflags" \
	MONO_CFLAGS="-I$mono/src -I$mono/src/optional" \
	MONO_LIBS="$mono/src/monocypher.c $mono/src/optional/monocypher-ed25519.c" check ||
	failed+=(unit-tests)

[ ${#failed[@]} -eq 0 ] || die "failed: ${failed[*]}"
msg "All programs built, unit tests passed"
