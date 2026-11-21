# Common build environment. Sourced by all build scripts.

set -euo pipefail

# Run from make, variables such as VERSION=1.2 on its command line reach
# every makefile below through MAKEFLAGS and the environment, overriding
# their own (the kernel's VERSION, zstd's). Builds pass their own -j.
unset MAKEFLAGS MFLAGS MAKELEVEL MAKEOVERRIDES VERSION

WORMOS_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

TARGET=x86_64-wormos-linux-gnu
ARCH=x86_64
JOBS=${JOBS:-$(nproc)}

SOURCES=$WORMOS_ROOT/sources
BUILD=$WORMOS_ROOT/build
OUT=$WORMOS_ROOT/out
REPO=$OUT/repo              # signed package repository (.wpkg files and index)
TOOLS=$BUILD/tools          # cross toolchain prefix
SYSROOT=$BUILD/sysroot      # target headers and libraries used for linking
WORK=$BUILD/work            # unpacked sources and build trees
PKGDIR=$BUILD/pkgs          # staged package contents, one directory per package
FSROOT=$BUILD/fsroot        # top level of the btrfs image (one dir per subvolume)
ROOTFS=$FSROOT/@            # assembled root filesystem (the @ subvolume)

# ~/.local/bin: a newer meson than Debian's (pipx install meson, see docs/building.md).
export PATH=$TOOLS/bin:$HOME/.local/bin:/usr/sbin:/sbin:$PATH
export LC_ALL=C
# Files and directories built here end up in the system: never group- or
# world-writable unless a recipe says so.
umask 022

msg() { printf '\033[1;35m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# fetch URL SHA256
# Download a source archive into $SOURCES (if missing) and verify its checksum.
# URL may be given as NAME::URL to store the file under another name.
fetch() {
	local url=$1 sum=$2 file
	file=$SOURCES/$(srcname "$url")
	url=${url#*::}
	mkdir -p "$SOURCES"
	if [ ! -f "$file" ]; then
		msg "Downloading $(basename "$url")"
		curl -fL --retry 3 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	echo "$sum  $file" | sha256sum -c --quiet - ||
		die "checksum mismatch for $(basename "$file")"
}

# srcname URL: the file name a source is stored under (see fetch).
srcname() {
	case $1 in
	*::*) echo "${1%%::*}" ;;
	*) basename "$1" ;;
	esac
}

# unpack ARCHIVE DESTDIR
# Extract an archive into a fresh directory, stripping the top-level folder.
unpack() {
	rm -rf "$2"
	mkdir -p "$2"
	tar -xf "$1" -C "$2" --strip-components=1
}

# pkginfo NAME VERSION RELEASE DESCRIPTION [DEPENDS...]
# Record metadata for a package staged in $PKGDIR/NAME. Extra lines (caps,
# groups, excludes) can be appended to $BUILD/pkginfo/NAME.info afterwards.
pkginfo() {
	local name=$1 version=$2 release=$3 description=$4
	shift 4
	mkdir -p "$BUILD/pkginfo"
	{
		echo "name = $name"
		echo "version = $version"
		echo "release = $release"
		echo "description = $description"
		echo "depends = $*"
	} > "$BUILD/pkginfo/$name.info"
	(cd "$PKGDIR/$name" && find . -mindepth 1 | sed 's|^\./||' | sort) \
		> "$BUILD/pkginfo/$name.files"
}

# tree_hash DIR [PATH...]: one SHA-256 over the names, types, modes, link
# targets and contents of PATHs (default: everything) under DIR. Times
# and owners do not count.
tree_hash() {
	local dir=$1
	shift
	[ $# -gt 0 ] || set -- .
	(
		cd "$dir"
		find "$@" -printf '%y %m %p %l\n' | sort
		find "$@" -type f -print0 | sort -z | xargs -0 -r sha256sum
	) | sha256sum | cut -c1-64
}

# Build cache (wbuild, build-kernel.sh): $CACHE/NAME.key records what a
# package was built from, $CACHE/NAME.out a hash of what it produced.
# WBUILD_FORCE=1 rebuilds anyway.
CACHE=$BUILD/cache

# The scripts and toolchain every build runs under.
build_env_hash() {
	cat "$WORMOS_ROOT/tools/wbuild" "$WORMOS_ROOT/scripts/env.sh" \
		"$WORMOS_ROOT/scripts/build-toolchain.sh" "$WORMOS_ROOT/toolchain/sources" |
		sha256sum | cut -c1-64
}

# stamp NAME: mark a build step done; done_step NAME: test whether it is.
stamp()     { mkdir -p "$BUILD/stamps"; touch "$BUILD/stamps/$1"; }
done_step() { [ -f "$BUILD/stamps/$1" ]; }
