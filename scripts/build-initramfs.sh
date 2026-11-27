#!/bin/bash
# Build the initramfs into the assembled root filesystem (/boot/initramfs.img)
# with the system's own builder, /usr/lib/wormrd/mkinitramfs (wormrd's
# package), run through the root's dynamic loader: the same program wh runs
# as a hook on installed systems after changes to what the initramfs holds
# (wormrd, BusyBox, snap, zstd, cryptsetup, the libraries).

. "$(dirname "$0")/env.sh"

TOOL=$ROOTFS/usr/lib/wormrd/mkinitramfs
[ -x "$TOOL" ] || die "wormrd's mkinitramfs is not installed in the root filesystem"
"$ROOTFS/usr/lib/ld-linux-x86-64.so.2" --library-path "$ROOTFS/usr/lib" "$TOOL" "$ROOTFS" ||
	die "the initramfs could not be built"
msg "Initramfs ready ($(du -h "$ROOTFS/boot/initramfs.img" | cut -f1))"
