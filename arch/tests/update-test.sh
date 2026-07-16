#!/bin/sh -e
# End-to-end test of nixgen-update: boot the ISO with a fresh store
# disk, run an offline update in the box that installs the kernel from
# a dated Arch Linux Archive snapshot: a real version change (direction
# is irrelevant to the machinery: new kernel files land in the overlay,
# the ALPM hook regenerates the initramfs, the new generation boots its
# own kernel). Archive use lives here only; stock generations track
# live mirrors. Poweroff; then boot the new generation directly from
# the store disk (no ISO: proves self-hosting)
# and check the boot marker, restored permissions, and the new package,
# then run the rest of the in-box lifecycle: nixgen-remove must refuse
# the running generation, a committed generation must remove cleanly
# (store path GC'd, GRUB entry pruned), nixgen-switch must soft-reboot
# into a committed generation with the marker (not the stale cmdline)
# driving running-generation detection afterwards, and nixgen-setup
# must install onto a blank disk that then boots alone under OVMF
# (boot 3: real firmware, real ESP, the in-box install path).
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

# the in-box update installs the kernel from a dated archive snapshot:
# mirrorlist swapped for one transaction (-Syy: the archive db is older
# than the cached live one, plain -Sy 304s and resolves against live;
# no download timeout: the archive throttles), then restored. \$repo
# survives the layers to let pacman expand it
PIN=$(date -d '-30 days' +%Y/%m/%d)
UPCMD="mv /etc/pacman.d/mirrorlist /tmp/ml && echo \"Server = https://archive.archlinux.org/repos/$PIN/\\\$repo/os/\\\$arch\" > /etc/pacman.d/mirrorlist && pacman -Syy --noconfirm --disable-download-timeout linux tree && mv /tmp/ml /etc/pacman.d/mirrorlist && pacman -Syy --noconfirm"

arch/iso/mkstoredisk.sh
echo "--- boot 1: ISO + fresh disk, offline kernel version change in the box"
qemu-system-x86_64 $ACCEL -m 2G -boot d -cdrom build/nixarch.iso \
	-drive file=build/nixstore.img,format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
# expect a pattern *after* the generation name: matching on "updated: "
# can fire mid-line, before the name has arrived over the serial socket
OUT=$(drive "NIXARCH BOOT OK" \
	"nixgen-update test-up '$UPCMD'" \
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

# the ISO kernel must have been replaced by the snapshot one, and the
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
	&& { echo "FAIL: kernel unchanged (archive install broken)"; exit 1; }
cmp -s build/iso-initrd build/test-initrd \
	&& { echo "FAIL: initramfs not regenerated"; exit 1; }
echo "kernel version changed, initramfs regenerated"

echo "--- boot 2: new generation from store disk only (no ISO)"
rm -f "$SOCK" build/install-test.img
truncate -s 6G build/install-test.img
qemu-system-x86_64 $ACCEL -m 2G \
	-kernel build/test-vmlinuz -initrd build/test-initrd \
	-append "nixgen=$NEWGEN console=ttyS0,115200" \
	-drive file=build/nixstore.img,format=raw,if=virtio \
	-drive file=build/install-test.img,format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
drive "NIXARCH BOOT OK" \
	"stat -c %a /var/tmp" \
	"1777" \
	'pacman -Q 2>&1 >/dev/null | grep -q "duplicated database" || echo DB_CLEAN' \
	"DB_CLEAN" \
	'echo "shadow=$(stat -c %a /etc/shadow)"' \
	"shadow=600" \
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
	'ps -o args= -C agetty | grep tty1 | grep -q autologin && echo AUTO_TTY1' \
	"AUTO_TTY1" \
	'systemctl start getty@tty2 && sleep 1 && ps -o args= -C agetty | grep tty2 | grep -q autologin && echo AUTO_TTY2' \
	"AUTO_TTY2" \
	'touch /etc/diffmark && ln -s /tmp /var/linktest && chown -h 977:977 /var/linktest && touch /etc/acltest && setfacl -m u:977:r /etc/acltest && nixgen-commit test-sw' \
	"visible next boot" \
	"nixgen-switch test-sw" \
	"-test-sw (soft)" \
	'echo "$(stat -c %a /usr)-$(stat -c %a /var/tmp)"' \
	"755-1777" \
	'echo "$(stat -c %u /var/linktest):$(getfacl --numeric /etc/acltest | grep -c "user:977:r--"):$(getcap /usr/bin/newuidmap | grep -c setuid)"' \
	"977:1:1" \
	"nixgen-remove test-sw" \
	"refusing to remove the running generation" \
	"nixgen-listid" \
	"test-sw (running)" \
	"nixgen-diffid test-up test-sw" \
	"Only in b/etc: diffmark" \
	'ID=$(nixgen-listid | grep test-up | cut -c1-8); nixgen-diffid "$ID" test-sw' \
	"Only in b/etc: diffmark" \
	'ok=1; for t in /usr/local/bin/nixgen-*; do nixgen-help | grep -q "$(basename "$t")" || { echo "undocumented: $t"; ok=0; }; done; [ $ok = 1 ] && echo HELP_OK' \
	"HELP_OK" \
	"nixgen-setup /dev/vdb inst-test" \
	"type the device path to continue" \
	"/dev/vdb" \
	"installed: inst-test on /dev/vdb" \
	'echo root:secret | chpasswd && systemctl restart getty@tty1 && sleep 1 && ps -o args= -C agetty | grep tty1 | grep -qv autologin && echo PROMPT_TTY1' \
	"PROMPT_TTY1" \
	"poweroff" > /dev/null || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID
rm -f build/iso-vmlinuz build/iso-initrd build/test-vmlinuz build/test-initrd

echo "--- boot 3: installed disk alone under OVMF (nixgen-setup output)"
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
[ -n "$OVMF_CODE" ] || { echo "FAIL: no OVMF firmware (edk2-ovmf)"; exit 1; }
cp "$OVMF_VARS" build/test-ovmf-vars.fd
rm -f "$SOCK"
qemu-system-x86_64 $ACCEL -machine q35 -m 2G \
	-drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
	-drive if=pflash,format=raw,file=build/test-ovmf-vars.fd \
	-drive file=build/install-test.img,format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
drive "NIXARCH BOOT OK" \
	'grep -o "nixgen=[^ ]*" /proc/cmdline' \
	"-inst-test" \
	"poweroff" > /dev/null || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID
rm -f build/install-test.img build/test-ovmf-vars.fd
echo "installed disk booted under OVMF"

grep -aq "nixgen=$NEWGEN" "$LOG" || { echo "FAIL: marker missing"; exit 1; }
# the removed generation's entry is gone, the surviving one remains
ENTRIES=$(debugfs -R "cat /entries.cfg" build/nixstore.img 2>/dev/null)
echo "$ENTRIES" | grep -q "test-rm" \
	&& { echo "FAIL: pruned GRUB entry still on disk"; exit 1; }
echo "$ENTRIES" | grep -q "nixgen=$NEWGEN" \
	|| { echo "FAIL: surviving GRUB entry lost"; exit 1; }
# the switched-into generation was refused removal, its entry must live
echo "$ENTRIES" | grep -q -- "-test-sw" \
	|| { echo "FAIL: test-sw GRUB entry lost"; exit 1; }
echo "PASS: $NEWGEN booted from disk, kernel changed, perms restored," \
	"tree installed, remove + soft-switch + getty lifecycle clean," \
	"in-box install boots under OVMF"
