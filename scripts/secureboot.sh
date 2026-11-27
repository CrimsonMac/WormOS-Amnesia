# shellcheck shell=bash
# Secure Boot pieces, sourced by the image, ISO and kernel scripts (after
# env.sh).
#
# WormOS boots under Secure Boot the way most distributions do: the firmware
# starts Debian's shim (signed by Microsoft's UEFI CA, which PCs trust out of
# the box), shim starts GRUB, and GRUB has shim check the kernel. GRUB and
# the kernel are signed with WormOS's own key; the first start with Secure
# Boot on opens shim's MokManager, where the user enrolls WormOS's
# certificate from the EFI system partition once (see docs/guide.md).
#
# The signing key is $WORMOS_SB_KEY (default ~/.config/wormos/secureboot.key,
# never in the repository) with the certificate keys/secureboot.crt. Without
# that key (anyone else's build) a development key is made in build/host,
# and its certificate goes onto the EFI system partition instead.

SB_DIR=$BUILD/host/secureboot

# Debian's signed shim and MokManager, pinned.
SHIM_DEB="https://deb.debian.org/debian/pool/main/s/shim-signed/shim-signed_1.51~1+deb13u1+16.1-2~deb13u1_amd64.deb"
SHIM_SHA=3c802fa303c0e6bf126adee74028d1042e3120360d159c2c05181dc9b2f61005
HELPERS_DEB="https://deb.debian.org/debian/pool/main/s/shim-helpers-amd64-signed/shim-helpers-amd64-signed_1+16.1+2~deb13u1_amd64.deb"
HELPERS_SHA=7e950d75d5a40ecc0a99f727452ce35780b47a04d9a2cc3b1ff7c2bb316c033e

# The key and certificate to sign with: SB_KEY, SB_CRT.
sb_key() {
	local real=${WORMOS_SB_KEY:-$HOME/.config/wormos/secureboot.key}

	command -v sbsign > /dev/null || die "sbsign is missing (apt install sbsigntool)"
	mkdir -p "$SB_DIR"
	if [ -f "$real" ]; then
		SB_KEY=$real
		SB_CRT=$WORMOS_ROOT/keys/secureboot.crt
		openssl x509 -in "$SB_CRT" -noout -pubkey |
			cmp -s - <(openssl pkey -in "$SB_KEY" -pubout) ||
			die "$SB_KEY does not belong to keys/secureboot.crt"
	else
		SB_KEY=$SB_DIR/dev.key
		SB_CRT=$SB_DIR/dev.crt
		if [ ! -f "$SB_KEY" ]; then
			msg "No Secure Boot key: making a development key"
			(umask 077; openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 36500 \
				-subj "/CN=WormOS Secure Boot (development)/" \
				-keyout "$SB_KEY" -out "$SB_CRT" 2> /dev/null) ||
				die "cannot make a development key"
		fi
	fi
}

# sb_sign IN OUT: an EFI program signed with the key. A signature records
# when it was made, so the same input reuses its earlier signed copy
# (kept by the input's SHA-256 and the certificate's): rebuilds stay the
# same bytes.
sb_sign() {
	local cached

	[ -n "${SB_KEY:-}" ] || sb_key
	cached=$SB_DIR/signed/$(cat "$1" "$SB_CRT" | sha256sum | cut -c1-64).efi
	if [ ! -f "$cached" ]; then
		mkdir -p "$SB_DIR/signed"
		sbsign --key "$SB_KEY" --cert "$SB_CRT" --output "$cached.new" "$1" 2> /dev/null ||
			die "cannot sign $(basename "$1")"
		mv "$cached.new" "$cached"
	fi
	cp "$cached" "$2"
}

# shim, MokManager and the certificate in DER form, in $SB_DIR.
sb_shim() {
	local tmp

	[ -n "${SB_KEY:-}" ] || sb_key
	fetch "$SHIM_DEB" "$SHIM_SHA"
	fetch "$HELPERS_DEB" "$HELPERS_SHA"
	tmp=$(mktemp -d)
	dpkg-deb -x "$SOURCES/$(basename "$SHIM_DEB")" "$tmp"
	dpkg-deb -x "$SOURCES/$(basename "$HELPERS_DEB")" "$tmp"
	cp "$tmp/usr/lib/shim/shimx64.efi.signed" "$SB_DIR/shimx64.efi"
	cp "$tmp/usr/lib/shim/mmx64.efi.signed" "$SB_DIR/mmx64.efi"
	rm -rf "$tmp"
	openssl x509 -in "$SB_CRT" -outform DER -out "$SB_DIR/wormos.cer"
}

# GRUB's SBAT data: Debian's (the modules are Debian's build) and a line of
# our own. shim refuses GRUB builds older than its revocation list.
sb_sbat() {
	local signed=/usr/lib/grub/x86_64-efi-signed/grubx64.efi.signed ver

	[ -f "$signed" ] || die "Debian's signed GRUB is missing (apt install grub-efi-amd64-signed)"
	ver=$(dpkg-query -W -f '${Version}' grub-efi-amd64-bin)
	objcopy -O binary --only-section=.sbat "$signed" /dev/stdout | tr -d '\0' > "$SB_DIR/sbat.csv"
	grep -q ",$ver," "$SB_DIR/sbat.csv" ||
		die "grub-efi-amd64-signed and grub-efi-amd64-bin differ in version"
	echo "grub.wormos,1,WormOS,grub2,$ver,https://github.com/CrimsonMac/WormOS" >> "$SB_DIR/sbat.csv"
}

# GRUB's modules for UEFI. Under Secure Boot GRUB loads no modules from
# disk, so everything it needs is built in.
SB_GRUB_MODULES="part_gpt part_msdos fat iso9660 btrfs zstd normal linux search
	search_fs_uuid search_fs_file configfile echo test ls cat sleep reboot halt
	serial terminal all_video efi_gop font gfxterm efifwsetup
	cryptodisk luks2 gcry_rijndael gcry_sha256 gcry_sha512 pbkdf2"

# sb_efi_dir DIR [CONFIG]: the EFI boot folder: shim as BOOTX64.EFI, GRUB
# (prefix /EFI/BOOT, CONFIG built in if given) signed as grubx64.efi,
# MokManager and the certificate to enroll.
sb_efi_dir() {
	local dir=$1 config=${2:-}

	sb_shim
	sb_sbat
	# shellcheck disable=SC2086 # the module list is words
	grub-mkimage -O x86_64-efi -p /EFI/BOOT ${config:+-c "$config"} --sbat "$SB_DIR/sbat.csv" \
		-o "$SB_DIR/grubx64.efi" $SB_GRUB_MODULES
	mkdir -p "$dir"
	sb_sign "$SB_DIR/grubx64.efi" "$dir/grubx64.efi"
	cp "$SB_DIR/shimx64.efi" "$dir/BOOTX64.EFI"
	cp "$SB_DIR/mmx64.efi" "$dir/mmx64.efi"
	cp "$SB_DIR/wormos.cer" "$dir/wormos.cer"
}
