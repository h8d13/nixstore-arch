#!/usr/bin/env bash
# Dev VM for nixarch on the real UEFI path: OVMF pflash, persistent
# NVRAM, q35. Images are NOT built here: mkiso.sh and mkstoredisk.sh do
# that. Installing onto a disk is nixgen-setup's job, run from inside a
# booted box, which is what the fresh/keep/boot modes exercise.
# Derived from archinstoo's TVM.
#
# Host artifacts (in build/, git-ignored):
#   build/ovmf-vars.fd      persistent UEFI NVRAM (copied from OVMF_VARS)
#   build/vm-target.qcow2   blank disk to install onto (nixgen-setup)
#
# Usage (argless is the real workflow: ISO in the drive, one empty disk,
# install onto it with nixgen-setup):
#   ./uefi-vm.sh          | fresh - boot the ISO with a blank target as
#                                   the only disk (nixgen-setup /dev/vda)
#   ./uefi-vm.sh keep     | k     - same, reusing the existing target
#   ./uefi-vm.sh boot     | b     - boot the installed target, no ISO
#   ./uefi-vm.sh iso      | i     - boot the ISO with the NIXSTORE store
#                                   disk instead: the commit/update play-
#                                   ground, nothing gets installed
#   ./uefi-vm.sh clean    | c     - remove NVRAM and the install target
#   ./uefi-vm.sh help     | h     - this text
# Env overrides:
#   ISO, STORE          paths to the ISO / NIXSTORE store disk
#                       (STORE=none boots without a store disk)
#   RAM, CPUS           VM resources (default 2G, 2)
#   DISK_SIZE           blank target size (default 30G)
#   DISPLAY_BACKEND     qemu -display value (default 'gtk,zoom-to-fit=off')
#   VGA                 qemu -vga value (default 'std')
#   GL                  'on' swaps video for virtio-vga-gl + display gl=on
#                       (virgl 3D). Needs the full module chain:
#                       qemu-hw-display-virtio-{gpu,gpu-gl,vga,vga-gl}
#                       + qemu-ui-opengl
#   AUDIO_BACKEND       qemu -audiodev value ('none' default; 'pipewire',
#                       'pa', 'alsa' each need the matching qemu-audio-* pkg)
#   SERIAL              'on' runs headless with serial console + monitor
#                       muxed on this terminal (autologin getty lands
#                       here). Ctrl-A c toggles console/monitor, Ctrl-A x
#                       quits. GRUB menu stays on the (absent) VGA:
#                       default entry boots after the timeout


set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${ISO:-$REPO/build/nixarch.iso}"
STORE="${STORE:-$REPO/build/nixstore.img}"
TARGET="$REPO/build/vm-target.qcow2"
VARS="$REPO/build/ovmf-vars.fd"
DISK_SIZE="${DISK_SIZE:-30G}"
RAM="${RAM:-2G}"
CPUS="${CPUS:-2}"
DISPLAY_BACKEND="${DISPLAY_BACKEND:-gtk,zoom-to-fit=off}"
VGA="${VGA:-std}"
GL="${GL:-off}"
# nixarch ships no audio stack; sound is opt-in so the VM does not need
# a qemu-audio-* package to start
AUDIO_BACKEND="${AUDIO_BACKEND:-none}"
SERIAL="${SERIAL:-off}"
ARG="${1:-fresh}"

err() {
	echo "ERROR: $*" >&2
	exit 1
}

check_host_deps() {
	local missing=() cmd
	for cmd in qemu-system-x86_64 qemu-img; do
		command -v "$cmd" > /dev/null 2>&1 || missing+=("$cmd")
	done
	[ ${#missing[@]} -eq 0 ] \
		|| err "missing host commands: ${missing[*]} (install qemu-base)"
}

# OVMF pairs across distro layouts, first hit wins
probe_ovmf() {
	local pairs=(
		"/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd"
		"/usr/share/edk2-ovmf/x64/OVMF_CODE.fd:/usr/share/edk2-ovmf/x64/OVMF_VARS.fd"
		"/usr/share/edk2/ovmf/OVMF_CODE.fd:/usr/share/edk2/ovmf/OVMF_VARS.fd"
		"/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd"
		"/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd"
	)
	local pair
	for pair in "${pairs[@]}"; do
		if [ -f "${pair%%:*}" ] && [ -f "${pair##*:}" ]; then
			OVMF_CODE="${pair%%:*}"
			OVMF_VARS_ORIG="${pair##*:}"
			return 0
		fi
	done
	err "no OVMF firmware (pacman -S edk2-ovmf)"
}

require_iso() {
	[ -f "$ISO" ] || err "no $ISO: run arch/iso/mkiso.sh first"
}

ATTACH_ISO=false ATTACH_STORE=false ATTACH_TARGET=false

case "$ARG" in
-h | --help | help | h)
	sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; /^set -euo/d'
	exit 0
	;;
clean | c)
	rm -fv "$VARS" "$TARGET"
	exit 0
	;;
iso | i)
	require_iso
	ATTACH_ISO=true ATTACH_STORE=true
	;;
fresh | f)
	require_iso
	rm -f "$TARGET"
	qemu-img create -f qcow2 "$TARGET" "$DISK_SIZE"
	ATTACH_ISO=true ATTACH_TARGET=true
	;;
keep | k)
	require_iso
	[ -f "$TARGET" ] || err "no $TARGET: run '$0 fresh' first"
	ATTACH_ISO=true ATTACH_TARGET=true
	;;
boot | b)
	[ -f "$TARGET" ] || err "no $TARGET: install onto it with '$0 fresh'"
	ATTACH_TARGET=true
	;;
*)
	err "unknown argument: $ARG (try -h)"
	;;
esac

check_host_deps
probe_ovmf
# NVRAM persists across runs (boot entries, timeouts); clean resets it
[ -f "$VARS" ] || cp "$OVMF_VARS_ORIG" "$VARS"

# virgl needs both ends: a GL-capable device AND gl=on on the display.
# -device help skips unloaded modules, so probe the device itself
if [ "$GL" = "on" ]; then
	if qemu-system-x86_64 -device virtio-vga-gl,help 2>&1 | grep -q "not found"; then
		err "virtio-vga-gl not loadable (install" \
			"qemu-hw-display-virtio-{gpu,gpu-gl,vga,vga-gl} and qemu-ui-opengl)"
	fi
	VIDEO_ARGS=(-device virtio-vga-gl -display "$DISPLAY_BACKEND,gl=on")
else
	VIDEO_ARGS=(-vga "$VGA" -display "$DISPLAY_BACKEND")
fi

ACCEL=()
[ -w /dev/kvm ] && ACCEL=(-enable-kvm -cpu host)

# stdio carries either the monitor (default) or the muxed serial
# console (SERIAL=on): both on stdio would collide
MON_ARGS=(-monitor stdio)
if [ "$SERIAL" = "on" ]; then
	VIDEO_ARGS=(-display none)
	MON_ARGS=(-serial mon:stdio)
fi

QEMU_ARGS=(
	-machine q35
	"${ACCEL[@]}"
	-m "$RAM"
	-smp "$CPUS"
	-device virtio-net-pci,netdev=net0
	-netdev user,id=net0
	"${VIDEO_ARGS[@]}"
	"${MON_ARGS[@]}"
	-drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
	-drive "if=pflash,format=raw,file=$VARS"
)

if [ "$AUDIO_BACKEND" != "none" ]; then
	QEMU_ARGS+=(
		-audiodev "$AUDIO_BACKEND,id=snd0"
		-device ich9-intel-hda
		-device hda-duplex,audiodev=snd0
	)
fi

# explicit bootindex: NVRAM entries from earlier runs otherwise shadow
# the DVD (OVMF walks stale entries into a PXE loop and never falls back
# to the cdrom)
if [ "$ATTACH_ISO" = true ]; then
	QEMU_ARGS+=(
		-drive "if=none,id=cd,file=$ISO,format=raw,media=cdrom"
		-device ide-cd,drive=cd,bootindex=0
	)
fi
# the NIXSTORE disk: what nixgen-commit writes to in iso mode. Never
# alongside an install target: the target carries its own NIXSTORE
# partition, and GRUB picks the store by label, so two of them is an
# ambiguity nobody wants to debug. STORE=none skips it entirely
if [ "$ATTACH_STORE" = true ] && [ "$STORE" != none ] && [ -f "$STORE" ]; then
	QEMU_ARGS+=(-drive "file=$STORE,format=raw,if=virtio")
fi
# blank install target for nixgen-setup; after installing, '$0 boot'
# starts it alone, which is what a real machine does
if [ "$ATTACH_TARGET" = true ]; then
	QEMU_ARGS+=(-drive "file=$TARGET,format=qcow2,if=virtio")
fi

# monitor on stdio can leave the terminal raw on abnormal exit
if [ -t 0 ]; then
	trap 'stty sane' EXIT
fi
qemu-system-x86_64 "${QEMU_ARGS[@]}"
