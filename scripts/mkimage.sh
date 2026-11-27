#!/bin/bash
# Build a bootable disk image from the assembled root filesystem. It
# boots with UEFI and with BIOS firmware (GRUB for both, one grub.cfg).
#
# Layout (GPT):
#   1  EFI system partition (FAT32)  GRUB for UEFI, grub.cfg
#   2  WormOS root (btrfs)           subvolumes @, @var, @home, @snapshots,
#                                    @wormholes
#   3  BIOS boot partition           GRUB for BIOS (core.img), in the gap
#                                    before partition 1; its boot sector
#                                    is in the protective MBR
#
# Each partition is built as a separate file and then written into the
# disk image, so no loop devices are needed. Building the btrfs needs root
# (sudo) for one step: mkfs.btrfs reads file owners in a way fakeroot cannot
# intercept, so the owners and modes fakeroot recorded are applied for real
# to a temporary copy of the tree first.
#
# IMAGE (default out/wormos.img) and DISK_MB (default 8192) choose the
# output file and the disk size. For the ISO (scripts/mkiso.sh), PARTS_DIR
# keeps the pieces there as well: system.img (the btrfs filesystem, shrunk
# to its contents, with SHRINK=1), esp.img and head.img (the disk's first
# MiB: protective MBR with GRUB's boot code, GPT, GRUB's core.img).

. "$(dirname "$0")/env.sh"

STATE=$BUILD/fakeroot.state
[ -f "$STATE" ] && [ -d "$ROOTFS" ] || die "run scripts/build-rootfs.sh first"

IMAGE=${IMAGE:-$OUT/wormos.img}
DISK_MB=${DISK_MB:-8192}
ESP_MB=128
SECTOR=512
MB=$((1024 * 1024 / SECTOR))     # sectors per MiB

bios_start=40                    # after the GPT's partition entries
bios_size=$((MB - bios_start))
esp_start=$MB
esp_size=$((ESP_MB * MB))
root_start=$((esp_start + esp_size))
root_size=$(((DISK_MB - 1) * MB - root_start))   # leave 1 MiB for the backup GPT

new_uuid() { cat /proc/sys/kernel/random/uuid; }
FS_UUID=$(new_uuid)
ROOT_PARTUUID=$(new_uuid)
ESP_PARTUUID=$(new_uuid)
BIOS_PARTUUID=$(new_uuid)
# The ESP's FAT volume ID, which GRUB for BIOS searches for (as XXXX-XXXX).
ESP_ID=$(od -An -N4 -tx4 /dev/urandom | tr -d ' ' | tr a-f A-F)

TMP=$BUILD/image
sudo rm -rf "$TMP"
mkdir -p "$TMP" "$OUT"

# Hardening: no merged slab caches (harder heap exploits), no debugfs, a
# strict IOMMU (no DMA passthrough, also not before the kernel takes over).
HARDENING="slab_nomerge debugfs=off iommu.passthrough=0 iommu.strict=1 efi=disable_early_pci_dma"
# quiet: only warnings and errors from the kernel on the consoles (the
# first-boot setup and the login share tty1 with them); dmesg has the rest.
KERNEL_CMDLINE="root=UUID=$FS_UUID rootflags=subvol=@,compress=zstd ro $HARDENING quiet console=tty0 console=ttyS0,115200"

msg "Applying file ownership"
fakeroot -i "$STATE" -- find "$FSROOT" -printf '%U %G %m %y %P\0' > "$TMP/owners"
sudo rm -rf "$TMP/fsroot"
sudo cp -a "$FSROOT" "$TMP/fsroot"
sudo python3 -I "$WORMOS_ROOT/scripts/apply-ownership.py" "$TMP/owners" "$TMP/fsroot"

msg "Creating btrfs root filesystem"
truncate -s $((root_size * SECTOR)) "$TMP/root.img"
sudo mkfs.btrfs -q ${SHRINK:+--shrink} \
	-L WORMOS \
	-U "$FS_UUID" \
	--compress zstd \
	--rootdir "$TMP/fsroot" \
	--subvol rw:@ \
	--subvol rw:@var \
	--subvol rw:@home \
	--subvol rw:@snapshots \
	--subvol rw:@wormholes \
	"$TMP/root.img"
sudo rm -rf "$TMP/fsroot"
sudo chown "$(id -u):$(id -g)" "$TMP/root.img"

msg "Creating the boot loaders and the EFI system partition"
grub-mkimage -O x86_64-efi -p /EFI/BOOT -o "$TMP/BOOTX64.EFI" \
	part_gpt fat btrfs zstd normal linux search search_fs_uuid \
	configfile echo test ls cat sleep reboot halt \
	serial terminal all_video efi_gop font gfxterm \
	cryptodisk luks2 gcry_rijndael gcry_sha256 gcry_sha512 pbkdf2

# GRUB for BIOS finds the ESP by its volume ID and reads the same grub.cfg.
cat > "$TMP/bios.cfg" <<EOF
search --no-floppy --fs-uuid --set=root ${ESP_ID:0:4}-${ESP_ID:4:4}
set prefix=(\$root)/EFI/BOOT
configfile \$prefix/grub.cfg
EOF
grub-mkimage -O i386-pc -c "$TMP/bios.cfg" -p /EFI/BOOT -o "$TMP/core.img" \
	biosdisk part_gpt fat btrfs zstd normal linux search search_fs_uuid \
	configfile echo test ls cat sleep reboot halt \
	serial terminal all_video vbe vga font gfxterm \
	cryptodisk luks2 gcry_rijndael gcry_sha256 gcry_sha512 pbkdf2

cat > "$TMP/grub.cfg" <<EOF
serial --unit=0 --speed=115200
terminal_input console serial
terminal_output console serial

set timeout=3
set default=0

# An encrypted install (the installer's LUKS2 with PBKDF2, which GRUB can
# open): the system's filesystem appears once the disk is unlocked.
if ! search --no-floppy --fs-uuid --set=root $FS_UUID; then
	echo "WormOS: the disk is encrypted."
	cryptomount -a
	search --no-floppy --fs-uuid --set=root $FS_UUID
fi
set wormos_cmdline="$KERNEL_CMDLINE"
export wormos_cmdline

menuentry "WormOS" {
	linux /@/boot/vmlinuz \$wormos_cmdline
	initrd /@/boot/initramfs.img
}

# Safe mode and entries for recent snapshots, maintained by snap. Before
# the first snapshot, safe mode boots the current system.
if [ -f /@snapshots/grub.cfg ]; then
	source /@snapshots/grub.cfg
else
	menuentry "Safe mode (no network)" {
		linux /@/boot/vmlinuz \$wormos_cmdline wormos.safe
		initrd /@/boot/initramfs.img
	}
fi
EOF

mkfs.fat -F 32 -n WORMOS-EFI -i "$ESP_ID" -C "$TMP/esp.img" $((ESP_MB * 1024)) > /dev/null
mmd -i "$TMP/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$TMP/esp.img" "$TMP/BOOTX64.EFI" "$TMP/grub.cfg" ::/EFI/BOOT/

msg "Writing disk image"
rm -f "$IMAGE"
truncate -s ${DISK_MB}M "$IMAGE"
sfdisk -q "$IMAGE" <<EOF
label: gpt
unit: sectors
first-lba: 34
start=$esp_start, size=$esp_size, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=$ESP_PARTUUID, name="EFI"
start=$root_start, size=$root_size, type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709, uuid=$ROOT_PARTUUID, name="WormOS"
start=$bios_start, size=$bios_size, type=21686148-6449-6E6F-744E-656564454649, uuid=$BIOS_PARTUUID, name="BIOS boot"
EOF
# Only the data of the (sparse) partition images is copied.
"$WORMOS_ROOT/scripts/sparse-copy.py" "$TMP/esp.img" "$IMAGE" $((esp_start * SECTOR))
"$WORMOS_ROOT/scripts/sparse-copy.py" "$TMP/root.img" "$IMAGE" $((root_start * SECTOR))
"$WORMOS_ROOT/scripts/bios-boot.py" "$IMAGE" /usr/lib/grub/i386-pc/boot.img \
	"$TMP/core.img" $bios_start $bios_size > /dev/null
if [ -n "${PARTS_DIR:-}" ]; then
	mkdir -p "$PARTS_DIR"
	cp --sparse=always "$TMP/root.img" "$PARTS_DIR/system.img"
	cp --sparse=always "$TMP/esp.img" "$PARTS_DIR/esp.img"
	head -c $((MB * SECTOR)) "$IMAGE" > "$PARTS_DIR/head.img"
	echo "$FS_UUID" > "$PARTS_DIR/fs-uuid"
	echo "$KERNEL_CMDLINE" > "$PARTS_DIR/cmdline"
fi
rm -rf "$TMP"

msg "Image ready: $IMAGE ($(du -h "$IMAGE" | cut -f1) used of ${DISK_MB} MiB)"
