#!/bin/bash
# Build the WormOS ISO from the assembled root filesystem: a live system
# that runs from memory and installs WormOS onto a disk. It boots from a
# CD/DVD or a USB stick (a hybrid image: write it to the stick as is), with
# UEFI or BIOS firmware.
#
# Contents:
#   boot/vmlinuz, boot/initramfs.img   the kernel and wormrd
#   boot/grub/grub.cfg                 the menu (GRUB)
#   boot/grub/eltorito.img             GRUB for BIOS
#   boot/efi.img                       shim and GRUB for UEFI (secureboot.sh)
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
. "$(dirname "$0")/secureboot.sh"

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

command -v grub-mkimage > /dev/null || die "grub-mkimage is missing (apt install grub-common)"
command -v xorriso > /dev/null || die "xorriso is missing (apt install xorriso)"

rm -rf "$WORK"
mkdir -p "$TREE/boot/grub" "$TREE/wormos"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$WORMOS_ROOT" log -1 --format=%ct 2> /dev/null || echo 0)}

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

msg "Creating the boot loaders"
# Both GRUBs find the ISO by a file only it has and read the menu above.
cat > "$WORK/early.cfg" <<EOF
search --no-floppy --file --set=root /wormos/system.size
set prefix=(\$root)/boot/grub
configfile \$prefix/grub.cfg
EOF
# UEFI: shim, signed GRUB, MokManager and the certificate in a FAT image,
# which the ISO also carries as an EFI system partition for USB sticks.
sb_efi_dir "$WORK/efi" "$WORK/early.cfg"
touch -d "@$SOURCE_DATE_EPOCH" "$WORK"/efi/*
mkfs.fat --invariant -n WORMOS-ISO -C "$TREE/boot/efi.img" 4096 > /dev/null
mmd -i "$TREE/boot/efi.img" ::/EFI ::/EFI/BOOT
mcopy -m -i "$TREE/boot/efi.img" "$WORK"/efi/* ::/EFI/BOOT/
# BIOS: GRUB as a CD boot image, every module built in.
grub-mkimage -O i386-pc-eltorito -p /boot/grub -c "$WORK/early.cfg" \
	-o "$TREE/boot/grub/eltorito.img" \
	biosdisk iso9660 part_msdos part_gpt normal linux search search_fs_file \
	configfile echo test ls cat sleep reboot halt serial terminal \
	all_video vbe vga font gfxterm

msg "Writing $(basename "$ISO")"
rm -f "$ISO"
# Every file and the volume dated by SOURCE_DATE_EPOCH (xorriso reads it).
find "$TREE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
# The layout grub-mkrescue makes: a hybrid MBR for BIOS from USB, El Torito
# for CDs, and the EFI image as an appended partition.
xorriso -as mkisofs -r -V "$VOLUME" \
	-b boot/grub/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table \
	--grub2-boot-info --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
	--efi-boot boot/efi.img -efi-boot-part --efi-boot-image --protective-msdos-label \
	-o "$ISO" "$TREE" 2> "$WORK/xorriso.log" ||
	{ cat "$WORK/xorriso.log" >&2; die "xorriso failed"; }
rm -rf "$WORK"

msg "ISO ready: $ISO ($(du -h "$ISO" | cut -f1))"
