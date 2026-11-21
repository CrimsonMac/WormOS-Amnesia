#!/bin/bash
# Build the WormOS ISO from the assembled root filesystem: a live system
# that runs from memory and installs WormOS onto a disk. It boots from a
# CD/DVD or a USB stick (a hybrid image: write it to the stick as is), with
# UEFI or BIOS firmware.
#
# Contents:
#   boot/vmlinuz, boot/initramfs.img   the kernel and wormrd
#   boot/grub/grub.cfg                 the menu (GRUB, from grub-mkrescue)
#   wormos/system.img.zst              the btrfs root filesystem, shrunk to
#                                      its contents and compressed: copied
#                                      into memory to run live, written to
#                                      the disk to install (then grown)
#   wormos/system.size                 its size uncompressed, in bytes
#   wormos/esp.img.zst                 the EFI system partition for the disk
#   wormos/head.img                    the disk's first MiB: MBR boot code,
#                                      GPT and GRUB for BIOS (the installer
#                                      writes its own partition table)
#   wormos/version                     the WormOS version
#
# ISO (default out/wormos.iso, out/wormos-barebones.iso or
# out/wormos-amnesia.iso) chooses the output file. The edition is the one
# the root filesystem was assembled as (WORMOS_VARIANT in build-rootfs.sh):
# the amnesia ISO only runs live (no installer) and boots with stricter
# kernel settings.

. "$(dirname "$0")/env.sh"

VARIANT_ID=standard
VARIANT=
[ -f "$ROOTFS/etc/worm/variant" ] && . "$ROOTFS/etc/worm/variant"
if [ "$VARIANT_ID" = standard ]; then
	ISO=${ISO:-$OUT/wormos.iso}
	TITLE="WormOS"
else
	ISO=${ISO:-$OUT/wormos-$VARIANT_ID.iso}
	TITLE="WormOS $VARIANT"
fi
VOLUME=WORMOS_LIVE
WORK=$BUILD/iso
PARTS=$WORK/parts
TREE=$WORK/tree

command -v grub-mkrescue > /dev/null || die "grub-mkrescue is missing (apt install grub-common)"
command -v xorriso > /dev/null || die "xorriso is missing (apt install xorriso)"

rm -rf "$WORK"
mkdir -p "$TREE/boot/grub" "$TREE/wormos"

# The partitions, with the root filesystem shrunk; the disk image itself
# is thrown away.
SHRINK=1 PARTS_DIR=$PARTS IMAGE=$WORK/disk.img DISK_MB=4096 \
	"$WORMOS_ROOT/scripts/mkimage.sh"
rm -f "$WORK/disk.img"

msg "Assembling the ISO tree"
cp "$ROOTFS/boot/vmlinuz" "$ROOTFS/boot/initramfs.img" "$TREE/boot/"
stat -c %s "$PARTS/system.img" > "$TREE/wormos/system.size"
# --rsyncable: unchanged parts of the system compress to the same bytes in
# the next release, so updates can download only what changed (WormOS
# Amnesia's updater does).
zstd -q -T0 -12 --rsyncable --rm "$PARTS/system.img" -o "$TREE/wormos/system.img.zst"
zstd -q -19 "$PARTS/esp.img" -o "$TREE/wormos/esp.img.zst"
cp "$PARTS/head.img" "$TREE/wormos/head.img"
sed -n 's/^VERSION_ID=//p' "$ROOTFS/etc/os-release" > "$TREE/wormos/version"

# The installed system's command line, with the live system instead of root=.
cmdline=$(sed 's/root=UUID=[^ ]* //' "$PARTS/cmdline")
if [ "$VARIANT_ID" = amnesia ]; then
	# Nothing to install, and less for an attacker: kernel memory stays
	# unreadable even to root (lockdown), freed memory is wiped at once.
	cmdline="$cmdline lockdown=confidentiality init_on_free=1 page_alloc.shuffle=1 randomize_kstack_offset=on vsyscall=none"
	entry="$TITLE"
else
	entry="Try or install $TITLE"
fi
cat > "$TREE/boot/grub/grub.cfg" <<EOF
serial --unit=0 --speed=115200
terminal_input console serial
terminal_output console serial

set timeout=5
set default=0

menuentry "$entry" {
	linux /boot/vmlinuz wormos.live $cmdline
	initrd /boot/initramfs.img
}

menuentry "$entry (no network)" {
	linux /boot/vmlinuz wormos.live wormos.safe $cmdline
	initrd /boot/initramfs.img
}

if [ "\$grub_platform" = efi ]; then
	menuentry "Firmware settings" {
		fwsetup
	}
fi
EOF

msg "Writing $(basename "$ISO")"
rm -f "$ISO"
grub-mkrescue -o "$ISO" "$TREE" -- -volid "$VOLUME" 2> "$WORK/mkrescue.log" ||
	{ cat "$WORK/mkrescue.log" >&2; die "grub-mkrescue failed"; }
rm -rf "$WORK"

msg "ISO ready: $ISO ($(du -h "$ISO" | cut -f1))"
