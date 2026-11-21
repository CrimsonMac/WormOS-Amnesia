#!/bin/bash
# Boot the WormOS disk image in QEMU with UEFI (or BIOS) firmware.
#
# Usage: scripts/run-qemu.sh [--gui] [--snapshot] [--bios]
#
# By default the VM runs on the serial console in this terminal
# (press Ctrl-A X to quit QEMU). --gui opens a graphical window instead.
# --snapshot discards all disk writes when QEMU exits. --bios boots with
# QEMU's default BIOS firmware (SeaBIOS) instead of UEFI.
# Both modes have a SPICE agent channel (QEMU's qemu-vdagent): the guest's
# clipboard agent shares the clipboard with QEMU's window and VNC clients.
# QEMU_HOSTFWD forwards host ports to the VM, e.g. tcp:127.0.0.1:2222-:22
# (several separated by spaces). QEMU_EXTRA adds options (split on spaces).
# IMAGE boots another image (raw, or qcow2 such as a release image).
# ISO adds that ISO as a CD drive and boots from it (with IMAGE as the
# disk to install onto). USB_ISO adds it as a USB stick instead (QEMU
# device id "stick", so the monitor can pull it out: device_del stick).
# QEMU_VDAGENT_MOUSE=on sends the pointer through the guest's agent, as
# SPICE clients do (the agent replays it as a virtual pointer).
# QEMU_NIC picks the network card model (default virtio-net-pci; e.g. e1000,
# e1000e or vmxnet3, the cards VMware offers).

. "$(dirname "$0")/env.sh"

IMAGE=${IMAGE:-$OUT/wormos.img}
[ -f "$IMAGE" ] || die "no image, run scripts/mkimage.sh first"
format=raw
case $IMAGE in *.qcow2) format=qcow2 ;; esac

OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS=$OUT/OVMF_VARS.fd
mkdir -p "$OUT"
[ -f "$OVMF_VARS" ] || cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS"

display=(-nographic)
extra=()
firmware=(-drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE"
	-drive if=pflash,format=raw,file="$OVMF_VARS")
for arg in "$@"; do
	case $arg in
	--gui)      display=(-display gtk -serial mon:stdio) ;;
	--snapshot) extra+=(-snapshot) ;;
	--bios)     firmware=() ;;
	*)          die "unknown option: $arg" ;;
	esac
done

netdev=user,id=net0
for fwd in ${QEMU_HOSTFWD:-}; do
	netdev+=,hostfwd=$fwd
done

cdrom=()
disk_boot=0
if [ -n "${ISO:-}" ]; then
	[ -f "$ISO" ] || die "no such ISO: $ISO"
	cdrom=(-drive file="$ISO",format=raw,if=none,id=cd0,media=cdrom,readonly=on
		-device ide-cd,drive=cd0,bus=ide.1,bootindex=0)
	disk_boot=1
fi

if [ -n "${USB_ISO:-}" ]; then
	[ -f "$USB_ISO" ] || die "no such ISO: $USB_ISO"
	cdrom=(-device qemu-xhci,id=xhci
		-drive file="$USB_ISO",format=raw,if=none,id=stick0,readonly=on
		-device usb-storage,bus=xhci.0,drive=stick0,id=stick,removable=on,bootindex=0)
	disk_boot=1
fi

accel=(-accel tcg)
[ -w /dev/kvm ] && accel=(-accel kvm -cpu host)

exec qemu-system-x86_64 \
	-machine q35 \
	"${accel[@]}" \
	-smp "${QEMU_CPUS:-4}" \
	-m "${QEMU_MEM:-1024}" \
	"${firmware[@]}" \
	-drive file="$IMAGE",format=$format,if=none,id=disk0 \
	-device virtio-blk-pci,drive=disk0,bootindex=$disk_boot \
	"${cdrom[@]}" \
	-netdev "$netdev" \
	-device "${QEMU_NIC:-virtio-net-pci}",netdev=net0 \
	-device virtio-rng-pci \
	-device virtio-serial-pci,id=vserial0 \
	-chardev qemu-vdagent,id=vdagent0,name=vdagent,clipboard=on,mouse=${QEMU_VDAGENT_MOUSE:-off} \
	-device virtserialport,bus=vserial0.0,chardev=vdagent0,name=com.redhat.spice.0 \
	"${display[@]}" "${extra[@]}" ${QEMU_EXTRA:-}
