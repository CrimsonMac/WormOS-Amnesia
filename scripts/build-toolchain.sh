#!/bin/bash
# Build the x86_64-wormos-linux-gnu cross toolchain.
#
# Stages:
#   1. binutils
#   2. gcc (bootstrap: C only, static libgcc, no libc)
#   3. linux API headers
#   4. glibc
#   5. gcc (final: C and C++, shared libgcc, linked against our glibc)
#
# The resulting compiler lives in build/tools, target libraries in build/sysroot.

. "$(dirname "$0")/env.sh"

while read -r url sum; do
	case $url in ''|'#'*) continue ;; esac
	fetch "$url" "$sum"
done < "$WORMOS_ROOT/toolchain/sources"

BINUTILS=binutils-2.47
GCC=gcc-15.3.0
GLIBC=glibc-2.44
GMP=gmp-6.3.0
MPFR=mpfr-4.2.2
MPC=mpc-1.3.1
LINUX=linux-6.18.57

mkdir -p "$TOOLS" "$SYSROOT/usr"/{bin,lib,sbin,include} "$WORK"

# Merged /usr layout: everything lives under /usr, top-level dirs are links.
for d in bin lib sbin; do ln -sfn usr/$d "$SYSROOT/$d"; done
ln -sfn usr/lib "$SYSROOT/lib64"
ln -sfn lib "$SYSROOT/usr/lib64"

common_gcc_flags=(
	--target="$TARGET"
	--prefix="$TOOLS"
	--with-sysroot="$SYSROOT"
	--with-glibc-version=2.44
	--enable-default-pie
	--enable-default-ssp
	--disable-multilib
	--disable-nls
)

prepare_gcc_tree() {
	unpack "$SOURCES/$GCC.tar.xz" "$WORK/gcc"
	unpack "$SOURCES/$GMP.tar.xz" "$WORK/gcc/gmp"
	unpack "$SOURCES/$MPFR.tar.xz" "$WORK/gcc/mpfr"
	unpack "$SOURCES/$MPC.tar.gz" "$WORK/gcc/mpc"
	# Install 64-bit libraries into lib rather than lib64.
	sed -e '/m64=/s/lib64/lib/' -i "$WORK/gcc/gcc/config/i386/t-linux64"
}

if ! done_step binutils; then
	msg "Building $BINUTILS"
	unpack "$SOURCES/$BINUTILS.tar.xz" "$WORK/binutils"
	mkdir -p "$WORK/binutils/build" && cd "$WORK/binutils/build" || exit 1
	../configure \
		--target="$TARGET" \
		--prefix="$TOOLS" \
		--with-sysroot="$SYSROOT" \
		--disable-nls \
		--disable-werror \
		--disable-gprofng \
		--enable-new-dtags \
		--enable-default-hash-style=gnu
	make -j"$JOBS"
	make install
	stamp binutils
fi

if ! done_step gcc-bootstrap; then
	msg "Building $GCC (bootstrap)"
	prepare_gcc_tree
	mkdir -p "$WORK/gcc/build-bootstrap" && cd "$WORK/gcc/build-bootstrap" || exit 1
	../configure "${common_gcc_flags[@]}" \
		--with-newlib \
		--without-headers \
		--disable-shared \
		--disable-threads \
		--disable-libatomic \
		--disable-libgomp \
		--disable-libquadmath \
		--disable-libssp \
		--disable-libvtv \
		--disable-libstdcxx \
		--disable-libsanitizer \
		--enable-languages=c,c++
	make -j"$JOBS"
	make install
	# Without libc headers gcc installs a partial limits.h; replace it.
	cd "$WORK/gcc" || exit 1
	cat gcc/limitx.h gcc/glimits.h gcc/limity.h > \
		"$(dirname "$("$TARGET-gcc" -print-libgcc-file-name)")/include/limits.h"
	stamp gcc-bootstrap
fi

if ! done_step linux-headers; then
	msg "Installing $LINUX API headers"
	unpack "$SOURCES/$LINUX.tar.xz" "$WORK/linux"
	cd "$WORK/linux" || exit 1
	make mrproper
	make ARCH=$ARCH headers
	rm -rf "$PKGDIR/linux-headers"
	mkdir -p "$PKGDIR/linux-headers/usr"
	cp -r usr/include "$PKGDIR/linux-headers/usr/"
	find "$PKGDIR/linux-headers/usr/include" -type f ! -name '*.h' -delete
	cp -r "$PKGDIR/linux-headers/usr/include" "$SYSROOT/usr/"
	stamp linux-headers
fi

if ! done_step glibc; then
	msg "Building $GLIBC"
	unpack "$SOURCES/$GLIBC.tar.xz" "$WORK/glibc"
	mkdir -p "$WORK/glibc/build" && cd "$WORK/glibc/build" || exit 1
	echo "rootsbindir=/usr/sbin" > configparms
	../configure \
		--prefix=/usr \
		--host="$TARGET" \
		--build="$(../scripts/config.guess)" \
		--enable-kernel=5.10 \
		--with-headers="$SYSROOT/usr/include" \
		--disable-nscd \
		--disable-werror \
		--enable-stack-protector=strong \
		--enable-bind-now \
		--enable-fortify-source \
		libc_cv_slibdir=/usr/lib
	make -j"$JOBS"
	rm -rf "$PKGDIR/glibc"
	make DESTDIR="$PKGDIR/glibc" install
	# Merged /usr: ld.so is expected at /lib64, which links to /usr/lib.
	sed '/RTLDLIST=/s@/usr@@g' -i "$PKGDIR/glibc/usr/bin/ldd"
	cp -a "$PKGDIR/glibc/." "$SYSROOT/"
	stamp glibc
fi

if ! done_step gcc-final; then
	msg "Building $GCC (final)"
	[ -d "$WORK/gcc" ] || prepare_gcc_tree
	rm -rf "$WORK/gcc/build-final"
	mkdir -p "$WORK/gcc/build-final" && cd "$WORK/gcc/build-final" || exit 1
	../configure "${common_gcc_flags[@]}" \
		--enable-shared \
		--enable-threads=posix \
		--enable-__cxa_atexit \
		--enable-languages=c,c++ \
		--disable-libsanitizer \
		--disable-bootstrap
	make -j"$JOBS"
	make install

	# Runtime libraries the target system needs.
	rm -rf "$PKGDIR/gcc-libs"
	mkdir -p "$PKGDIR/gcc-libs/usr/lib"
	cp -a "$TOOLS/$TARGET/lib/"libgcc_s.so* \
	      "$TOOLS/$TARGET/lib/"libstdc++.so* \
	      "$TOOLS/$TARGET/lib/"libatomic.so* \
	      "$PKGDIR/gcc-libs/usr/lib/"
	rm -f "$PKGDIR/gcc-libs/usr/lib/"*.py
	stamp gcc-final
fi

# The linker resolves the dependencies of shared libraries (such as Mesa's
# on libstdc++, from C programs) in the sysroot only.
cp -a "$PKGDIR/gcc-libs/usr/lib/." "$SYSROOT/usr/lib/"

# The C.UTF-8 locale (programs such as foot need a UTF-8 locale), compiled
# by the target's localedef through its own loader.
if [ ! -d "$PKGDIR/glibc/usr/lib/locale/C.utf8" ]; then
	mkdir -p "$PKGDIR/glibc/usr/lib/locale"
	I18NPATH=$PKGDIR/glibc/usr/share/i18n \
		"$PKGDIR/glibc/usr/lib/ld-linux-x86-64.so.2" --library-path "$PKGDIR/glibc/usr/lib" \
		"$PKGDIR/glibc/usr/bin/localedef" --no-archive -c -i C -f UTF-8 \
		"$PKGDIR/glibc/usr/lib/locale/C.utf8"
fi

# Package metadata for the libraries the target system uses. Development
# files stay in the sysroot and are left out of the packages.
pkginfo linux-headers 6.18.57 1 "Linux kernel headers"
pkginfo glibc 2.44 2 "The GNU C library"
printf 'exclude = %s\n' usr/include '*.a' '*.o' usr/share/info usr/share/man \
	usr/share/i18n >> "$BUILD/pkginfo/glibc.info"
pkginfo gcc-libs 15.3.0 1 "GCC runtime libraries" glibc

msg "Toolchain ready: $("$TARGET-gcc" --version | head -1)"
