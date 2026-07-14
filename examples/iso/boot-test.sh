#!/bin/sh -e
# Smoke-boot build/nixarch.iso headless: default GRUB entry, serial
# console captured to build/serial.log, pass = autologin marker appears.
# qemu has no reason to exit on success, so timeout reaps it; the log
# decides pass/fail.
cd "$(dirname "$0")/../.."

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm"
DISK=
[ -f build/nixstore.img ] && \
	DISK="-drive file=build/nixstore.img,format=raw,if=virtio"

timeout 180 qemu-system-x86_64 $ACCEL -m 1536 $DISK \
	-cdrom build/nixarch.iso -boot d \
	-display none -no-reboot -serial file:build/serial.log || true

grep -a "NIXARCH BOOT OK" build/serial.log && echo "PASS" || {
	echo "FAIL: marker not in build/serial.log"; exit 1;
}
