#!/bin/sh -e
# The primary workflow, offline: boot the ISO with one blank disk, run
# nixgen-setup onto it, power off, then boot that disk alone under OVMF.
# update-test covers the same install as one leg of a much longer run
# (kernel archive, network); this one is the fast daily check.
#
# The assertion that matters is on the second boot: an installed system
# must find its store on the disk and stop there. The ISO hunt is a
# fallback for a generation the disk does not hold, and a disk boot that
# reaches it is either about to fail or about to mount the wrong thing.
# Read from the initrd journal, which survives switch-root, because the
# service logs to console=tty0 (last console= wins) and never to serial.
# Serial console is a unix socket driven by the embedded python
# (expect pattern / send line), same as update-test.
cd "$(dirname "$0")/../.."

FS=${FS:-ext4}
IMG=build/install-test.img
LOG=build/install-test.log
SOCK=build/install-test.sock
VARS=build/install-test-vars.fd
rm -f "$LOG" "$SOCK" "$IMG" "$VARS"

[ -f build/nixarch.iso ] || { echo "no build/nixarch.iso: run arch/iso/mkiso.sh" >&2; exit 1; }

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm"

OVMF_CODE= OVMF_VARS=
for pair in \
	/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd \
	/usr/share/edk2-ovmf/x64/OVMF_CODE.fd:/usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
	/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd; do
	c=${pair%%:*} v=${pair##*:}
	if [ -f "$c" ] && [ -f "$v" ]; then
		OVMF_CODE=$c OVMF_VARS=$v
		break
	fi
done
[ -n "$OVMF_CODE" ] || { echo "FAIL: no OVMF firmware (edk2-ovmf)" >&2; exit 1; }
cp "$OVMF_VARS" "$VARS"

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

truncate -s 8G "$IMG"

echo "--- boot 1: ISO + blank disk, nixgen-setup --fs $FS"
qemu-system-x86_64 $ACCEL -m 2G -boot d -cdrom build/nixarch.iso \
	-drive file="$IMG",format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
drive "NIXARCH BOOT OK" \
	"nixgen-setup /dev/vda inst-test --fs $FS" \
	"type the device path to continue" \
	"/dev/vda" \
	"installed: inst-test on /dev/vda" \
	"poweroff" > /dev/null || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID

echo "--- boot 2: that disk alone under OVMF"
rm -f "$SOCK"
qemu-system-x86_64 $ACCEL -machine q35 -m 2G \
	-drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
	-drive if=pflash,format=raw,file="$VARS" \
	-drive file="$IMG",format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
# STORE_SRC/HUNT come from the initrd journal: the store must have been
# taken from the disk, and the ISO hunt must never have been entered
drive "NIXARCH BOOT OK" \
	'grep -o "nixgen=[^ ]*" /proc/cmdline' \
	"-inst-test" \
	'findmnt -no SOURCE /nixstore | grep -q /dev/ && echo STORE_ON_"DISK"' \
	"STORE_ON_DISK" \
	'journalctl -b -u nixgen-store.service --no-pager | grep -c "waiting for device labeled" | sed "s/^/HUNT=/"' \
	"HUNT=0" \
	'journalctl -b -u nixgen-store.service --no-pager | grep -q "writable store on" && echo FROM_"STOREDEV"' \
	"FROM_STOREDEV" \
	"poweroff" > /dev/null || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID

echo "PASS: installed disk boots alone, store from the disk, no ISO fallback"
