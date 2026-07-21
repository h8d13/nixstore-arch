#!/bin/sh -e
# Boot the ISO with NO store disk and prove two things at once:
#   - the box comes up fast. This path has no NIXSTORE device to find,
#     which is exactly where an initramfs is tempted to wait for absent
#     hardware; nixgen-store settles instead of waiting, and a stall
#     here means that regressed (see LIMIT below).
#   - commit/update/remove refuse (mountpoint guard) rather than
#     building a generation into RAM that a reboot would eat.
# Drives the box over the serial console, so it needs no store, no
# snapshot and no second boot.
cd "$(dirname "$0")/../.."

LOG=build/diskless-test.log
SOCK=build/diskless-test.sock
LIMIT=${LIMIT:-60}
rm -f "$LOG" "$SOCK"

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm"
qemu-system-x86_64 $ACCEL -m 2G -boot d -cdrom build/nixarch.iso \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

python3 - "$SOCK" "$LOG" "$LIMIT" <<'PY'
import socket, sys, time

sock_path, log_path, limit = sys.argv[1], sys.argv[2], float(sys.argv[3])
s = socket.socket(socket.AF_UNIX)
deadline = time.time() + 30
while True:
	try:
		s.connect(sock_path)
		break
	except OSError:
		if time.time() > deadline:
			sys.exit("connect timeout")
		time.sleep(0.5)
s.settimeout(1.0)
log = open(log_path, "ab")
buf = b""
start = time.time()

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
	buf = b""

wait_for("NIXARCH BOOT OK", 300)
booted = time.time() - start
print(f"booted in {booted:.0f}s (diskless)")
if booted > limit:
	sys.exit(f"FAIL: diskless boot took {booted:.0f}s, over the {limit:.0f}s"
		" budget: something is waiting for the absent store disk")
for cmd in ("nixgen-commit x", "nixgen-update x", "nixgen-remove x"):
	s.sendall(cmd.encode() + b"\n")
	wait_for("no writable store", 60)
	print("refused:", cmd)
s.sendall(b"poweroff\n")
PY

wait $QPID 2>/dev/null || true
echo "PASS: diskless boot is prompt and refuses commit/update/remove"
