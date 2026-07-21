#!/bin/sh -e
# The ISO hunt, with the by-label shortcut deliberately broken
# (nixlabel=BOGUS): the initramfs has to walk the candidate list and
# recognise the boot medium by content. The store disk sits at vda, ie.
# before the ISO in that list, so a hunt that probes by mounting instead
# of gating on blkid spams the console with wrong-fs-type and can pick
# the wrong device. Boots kernel+initramfs directly, so no GRUB entry
# has to carry the bogus label.
# PASS = booted inside the budget, no mount errors on the console. The
# budget is the point of the timing: this boots the kernel directly (no
# GRUB menu), so a bogus label must cost nothing. Waiting on the missing
# label instead of going straight to the hunt put 10s here.
cd "$(dirname "$0")/../.."

LOG=build/isohunt-test.log
NOISE=${NOISE:-0}
LIMIT=${LIMIT:-15}
rm -f build/isohunt-vmlinuz build/isohunt-initrd "$LOG"

# store mtimes are canonicalised, so there is no newest to pick
G1=
for g in build/archstore/nix/store/*-nixarch-1; do
	[ -d "$g" ] || continue
	[ -z "$G1" ] || { echo "multiple nixarch-1 generations, REBUILD=1 mkiso" >&2; exit 1; }
	G1=$(basename "$g")
done
[ -n "$G1" ] || { echo "no nixarch generation: run arch/iso/mkiso.sh" >&2; exit 1; }
[ -f build/nixstore.img ] || { echo "no build/nixstore.img: run arch/iso/mkstoredisk.sh" >&2; exit 1; }

xorriso -osirrox on -indev build/nixarch.iso \
	-extract /boot/vmlinuz-linux build/isohunt-vmlinuz \
	-extract /boot/initramfs-linux.img build/isohunt-initrd

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm"
qemu-system-x86_64 $ACCEL -m 1536 \
	-kernel build/isohunt-vmlinuz -initrd build/isohunt-initrd \
	-append "nixgen=$G1 nixlabel=BOGUS rd.systemd.gpt_auto=0 console=tty0 console=ttyS0,115200" \
	-drive file=build/nixstore.img,format=raw,if=virtio \
	-drive file=build/nixarch.iso,format=raw,if=virtio \
	-display none -no-reboot -serial "file:$LOG" &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

WAITED=0
while [ "$WAITED" -lt 180 ]; do
	grep -aq "NIXARCH BOOT OK" "$LOG" 2>/dev/null && break
	kill -0 "$QPID" 2>/dev/null || break
	sleep 1
	WAITED=$((WAITED + 1))
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

grep -aq "NIXARCH BOOT OK" "$LOG" || {
	echo "FAIL: no boot marker, the hunt did not find the medium"
	exit 1
}
[ "$WAITED" -le "$LIMIT" ] || {
	echo "FAIL: booted in ${WAITED}s, over the ${LIMIT}s budget: a missing" \
		"label should cost nothing, something is waiting for it"
	exit 1
}
FOUND=$(grep -ac "wrong fs type\|bad superblock\|mount:.*failed" "$LOG" || true)
[ "$FOUND" -le "$NOISE" ] || {
	echo "FAIL: $FOUND mount-error lines on the console (budget $NOISE)," \
		"the hunt is probing by failing again:"
	grep -a "wrong fs type\|bad superblock\|mount:.*failed" "$LOG"
	exit 1
}
echo "PASS: hunt found the medium in ${WAITED}s past a decoy disk, $FOUND mount errors"
