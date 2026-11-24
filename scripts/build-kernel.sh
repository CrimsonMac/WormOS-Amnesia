#!/bin/bash
# Build the WormOS kernel and stage it as the "linux" package.

. "$(dirname "$0")/env.sh"

LINUX=linux-6.18.64
KOBJ=$BUILD/kernel

[ -x "$TOOLS/bin/$TARGET-gcc" ] || die "toolchain missing, run scripts/build-toolchain.sh first"

shopt -s nullglob
patches=("$WORMOS_ROOT"/kernel/patches/*.patch)

# Built again only when the sources, the configuration or the build scripts
# changed (WBUILD_FORCE=1 builds anyway).
key=$({
	echo "kernel cache 1 $LINUX"
	echo "env $(build_env_hash)"
	cat "$WORMOS_ROOT/scripts/build-kernel.sh" "$WORMOS_ROOT/kernel/wormos.config" \
		"${patches[@]}"
} | sha256sum | cut -c1-64)
if [ "${WBUILD_FORCE:-}" != 1 ] && [ -d "$PKGDIR/linux" ] &&
   [ -f "$BUILD/pkginfo/linux.info" ] && [ -f "$CACHE/linux.key" ] &&
   [ "$key" = "$(cat "$CACHE/linux.key")" ]; then
	msg "Up to date: $LINUX"
	exit 0
fi
mkdir -p "$CACHE"
rm -f "$CACHE/linux.key"

# A fresh tree, so that the patches in kernel/patches apply exactly once.
unpack "$SOURCES/$LINUX.tar.xz" "$WORK/linux"
for p in "${patches[@]}"; do
	msg "Applying $(basename "$p")"
	patch -d "$WORK/linux" -p1 < "$p"
done

# Keep the build machine's user and host name out of the kernel.
export KBUILD_BUILD_USER=wormos KBUILD_BUILD_HOST=wormos

kmake() {
	make -C "$WORK/linux" O="$KOBJ" ARCH=$ARCH CROSS_COMPILE="$TARGET-" "$@"
}

msg "Configuring $LINUX"
# Out-of-tree builds need a clean source tree (the headers step builds in it).
make -C "$WORK/linux" ARCH=$ARCH mrproper
mkdir -p "$KOBJ"
kmake x86_64_defconfig
"$WORK/linux/scripts/kconfig/merge_config.sh" -m -O "$KOBJ" \
	"$KOBJ/.config" "$WORMOS_ROOT/kernel/wormos.config"
kmake olddefconfig

# Report any requested option that did not survive dependency resolution.
missing=0
while IFS= read -r line; do
	case $line in
	CONFIG_*)
		grep -qxF "$line" "$KOBJ/.config" && continue ;;
	"# CONFIG_"*" is not set")
		opt=${line#\# }; opt=${opt%% *}
		! grep -q "^$opt=" "$KOBJ/.config" && continue ;;
	*)
		continue ;;
	esac
	echo "warning: not applied: $line"
	missing=1
done < "$WORMOS_ROOT/kernel/wormos.config"

msg "Building $LINUX"
kmake -j"$JOBS" bzImage modules

kver=$(kmake -s kernelrelease)
rm -rf "$PKGDIR/linux"
mkdir -p "$PKGDIR/linux/boot"
cp "$KOBJ/arch/x86/boot/bzImage" "$PKGDIR/linux/boot/vmlinuz-$kver"
cp "$KOBJ/.config" "$PKGDIR/linux/boot/config-$kver"
ln -sfn "vmlinuz-$kver" "$PKGDIR/linux/boot/vmlinuz"
kmake INSTALL_MOD_PATH="$PKGDIR/linux/usr" INSTALL_MOD_STRIP=1 modules_install
rm -f "$PKGDIR/linux/usr/lib/modules/$kver/build"
pkginfo linux "${LINUX#linux-}" 8 "The Linux kernel and modules"
tree_hash "$PKGDIR/linux" > "$CACHE/linux.out"
echo "$key" > "$CACHE/linux.key"

msg "Kernel $kver ready"
[ $missing -eq 0 ] || msg "Some config options were not applied (see warnings above)"
