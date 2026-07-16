#!/bin/sh -e
# Interactive QEMU launcher for the real UEFI path: OVMF pflash with
# persistent NVRAM (build/ovmf-vars.fd), q35 machine. Disk mode boots
# build/nixarch-disk.img exactly like the flashed stick: no ISO, no
# BIOS hybrid tricks. The headless CI-style checks stay in
# boot-test.sh/update-test.sh; this one opens a window.
#
# usage: uefi-vm.sh [disk|iso|clean]
#   disk (default)  boot build/nixarch-disk.img (mkbootdisk output)
#   iso             boot build/nixarch.iso, attach build/nixstore.img
#                   store disk when present
#   clean           remove the persistent NVRAM vars
# env: RAM (2G), CPUS (2), DISPLAY_BACKEND (gtk), IMG/ISO overrides
cd "$(dirname "$0")/../.."

RAM=${RAM:-2G} CPUS=${CPUS:-2}
DISPLAY_BACKEND=${DISPLAY_BACKEND:-gtk}
IMG=${IMG:-build/nixarch-disk.img}
ISO=${ISO:-build/nixarch.iso}
VARS=build/ovmf-vars.fd
MODE=${1:-disk}

# OVMF pairs across distro layouts, first hit wins
CODE= VARS_ORIG=
for pair in \
	/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd \
	/usr/share/edk2-ovmf/x64/OVMF_CODE.fd:/usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
	/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd \
	/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd; do
	c=${pair%%:*} v=${pair##*:}
	if [ -f "$c" ] && [ -f "$v" ]; then
		CODE=$c VARS_ORIG=$v
		break
	fi
done
[ -n "$CODE" ] || { echo "no OVMF firmware (pacman -S edk2-ovmf)" >&2; exit 1; }

case $MODE in
disk)
	[ -f "$IMG" ] || { echo "no $IMG: run arch/iso/mkbootdisk.sh first" >&2; exit 1; }
	DRIVES="-drive file=$IMG,format=raw,if=virtio"
	;;
iso)
	[ -f "$ISO" ] || { echo "no $ISO: run arch/iso/mkiso.sh first" >&2; exit 1; }
	DRIVES="-cdrom $ISO"
	if [ -f build/nixstore.img ]; then
		DRIVES="$DRIVES -drive file=build/nixstore.img,format=raw,if=virtio"
	fi
	;;
clean)
	rm -fv "$VARS"
	exit 0
	;;
*)
	echo "usage: $0 [disk|iso|clean]" >&2
	exit 1
	;;
esac

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"
# NVRAM persists across runs (boot entries, timeouts); clean resets it
[ -f "$VARS" ] || cp "$VARS_ORIG" "$VARS"

# monitor on stdio can leave the terminal raw on abnormal exit
if [ -t 0 ]; then
	trap 'stty sane' EXIT
fi
qemu-system-x86_64 $ACCEL -machine q35 -m "$RAM" -smp "$CPUS" \
	-drive "if=pflash,format=raw,readonly=on,file=$CODE" \
	-drive "if=pflash,format=raw,file=$VARS" \
	-nic user,model=virtio-net-pci \
	-display "$DISPLAY_BACKEND" \
	-monitor stdio \
	$DRIVES
