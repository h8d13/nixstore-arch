#!/bin/sh -e
# End-to-end test of nixgen-update: boot the ISO with a fresh store
# disk, run a real offline system upgrade in the box (the ISO kernel is
# pinned ~30 days back, so -Syu bumps the kernel version and the ALPM
# hook regenerates the initramfs), poweroff; then boot the new
# generation directly from the store disk (no ISO: proves self-hosting)
# and check the boot marker, restored permissions, and the new package,
# then run the rest of the in-box lifecycle: nixgen-remove must refuse
# the running generation, and a committed generation must remove
# cleanly (store path GC'd, GRUB entry pruned).
# Serial console is a unix socket driven by the embedded python
# (expect pattern / send line); debugfs reads the ext4 img without root.
cd "$(dirname "$0")/../.."

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm"
LOG=build/update-test.log
SOCK=build/update-test.sock
rm -f "$LOG" "$SOCK"

drive() { python3 - "$SOCK" "$LOG" "$@" <<'PY'
import socket, sys, time

sock_path, log_path = sys.argv[1], sys.argv[2]
script = sys.argv[3:]           # expect [send expect]... [send]

s = socket.socket(socket.AF_UNIX)
deadline = time.time() + 30
while True:
	try:
		s.connect(sock_path)
		break
	except OSError:
		if time.time() > deadline:
			sys.exit("connect timeout on " + sock_path)
		time.sleep(0.5)
s.settimeout(1.0)
log = open(log_path, "ab")
buf = b""

def wait_for(pat, timeout):
	global buf
	end = time.time() + timeout
	while pat.encode() not in buf:
		if time.time() > end:
			sys.exit("timeout waiting for: " + pat)
		try:
			d = s.recv(4096)
		except TimeoutError:
			continue
		if not d:
			sys.exit("eof waiting for: " + pat)
		buf += d
		log.write(d)
		log.flush()
	for line in buf.decode(errors="replace").splitlines():
		if pat in line:
			print(line)
	buf = b""

wait_for(script[0], 300)
i = 1
while i < len(script):
	s.sendall(script[i].encode() + b"\n")
	if i + 1 >= len(script):
		break           # trailing send (e.g. poweroff), no expect
	wait_for(script[i + 1], 900)
	i += 2
PY
}

examples/iso/mkstoredisk.sh
echo "--- boot 1: ISO + fresh disk, offline kernel upgrade in the box"
qemu-system-x86_64 $ACCEL -m 2G -boot d -cdrom build/nixarch.iso \
	-drive file=build/nixstore.img,format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
# expect a pattern *after* the generation name: matching on "updated: "
# can fire mid-line, before the name has arrived over the serial socket
OUT=$(drive "NIXARCH BOOT OK" \
	"nixgen-update test-up 'pacman -Syu --noconfirm tree linux'" \
	"reboot to switch" \
	"poweroff") || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID
NEWGEN=$(echo "$OUT" | sed -n 's/.*updated: \([^ ]*\).*/\1/p')
[ -n "$NEWGEN" ] || { echo "FAIL: no generation name captured"; exit 1; }
echo "new generation: $NEWGEN"

# restored dir modes in the update sandbox = no canonical-555 complaints
if grep -aq "directory permissions differ" "$LOG"; then
	echo "FAIL: pacman permission warnings in update sandbox"
	exit 1
fi

debugfs -R "cat /entries.cfg" build/nixstore.img 2>/dev/null \
	| grep -q "nixgen=$NEWGEN" || { echo "FAIL: no GRUB entry on disk"; exit 1; }
echo "GRUB entry present on store disk"

# the pinned ISO kernel must have been replaced by a newer one, and the
# ALPM hook must have rebuilt the initramfs (nixgen hook included)
rm -f build/iso-vmlinuz build/iso-initrd build/test-vmlinuz build/test-initrd
xorriso -osirrox on -indev build/nixarch.iso \
	-extract /boot/vmlinuz-linux build/iso-vmlinuz \
	-extract /boot/initramfs-linux.img build/iso-initrd
debugfs -R "dump /nix/store/$NEWGEN/boot/vmlinuz-linux build/test-vmlinuz" \
	build/nixstore.img 2>/dev/null
debugfs -R "dump /nix/store/$NEWGEN/boot/initramfs-linux.img build/test-initrd" \
	build/nixstore.img 2>/dev/null
[ -s build/test-vmlinuz ] && [ -s build/test-initrd ] \
	|| { echo "FAIL: kernel/initramfs not extracted from img"; exit 1; }
cmp -s build/iso-vmlinuz build/test-vmlinuz \
	&& { echo "FAIL: kernel unchanged (pin or upgrade broken)"; exit 1; }
cmp -s build/iso-initrd build/test-initrd \
	&& { echo "FAIL: initramfs not regenerated"; exit 1; }
echo "kernel upgraded, initramfs regenerated"

echo "--- boot 2: new generation from store disk only (no ISO)"
rm -f "$SOCK"
qemu-system-x86_64 $ACCEL -m 2G \
	-kernel build/test-vmlinuz -initrd build/test-initrd \
	-append "nixgen=$NEWGEN console=ttyS0,115200" \
	-drive file=build/nixstore.img,format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
drive "NIXARCH BOOT OK" \
	"stat -c %a /var/tmp" \
	"1777" \
	"command -v tree" \
	"/usr/bin/tree" \
	"nixgen-remove $NEWGEN" \
	"refusing to remove the running generation" \
	"nixgen-commit test-rm" \
	"visible next boot" \
	'R=$(basename "$(ls -d /nixstoredev/nix/store/*-test-rm)"); nixgen-remove test-rm' \
	"GRUB entry pruned" \
	'[ -e "/nixstoredev/nix/store/$R" ] || echo STORE_PATH_GONE' \
	"STORE_PATH_GONE" \
	"poweroff" > /dev/null || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID
rm -f build/iso-vmlinuz build/iso-initrd build/test-vmlinuz build/test-initrd

grep -aq "nixgen=$NEWGEN" "$LOG" || { echo "FAIL: marker missing"; exit 1; }
# the removed generation's entry is gone, the surviving one remains
ENTRIES=$(debugfs -R "cat /entries.cfg" build/nixstore.img 2>/dev/null)
echo "$ENTRIES" | grep -q "test-rm" \
	&& { echo "FAIL: pruned GRUB entry still on disk"; exit 1; }
echo "$ENTRIES" | grep -q "nixgen=$NEWGEN" \
	|| { echo "FAIL: surviving GRUB entry lost"; exit 1; }
echo "PASS: $NEWGEN booted from disk, kernel upgraded, perms restored," \
	"tree installed, remove lifecycle clean"
