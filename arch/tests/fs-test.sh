#!/bin/sh -e
# Host-only: every filesystem in the nixgen-fs table can carry a store.
# Formats an image file, checks the label came out right (GRUB and the
# initramfs both find the store by label, so a mkfs arm with the wrong
# label flag boots nothing), and runs the table's own read-only check
# against it. Rootless, no VM, seconds.
cd "$(dirname "$0")/../.."
. arch/nixgen/nixgen-fs

T=$(mktemp -d build/fs-test.XXXXXX)
trap 'rm -rf "$T"' EXIT
rc=0

for fs in $NIXGEN_FS_ALL; do
	if ! command -v "mkfs.$fs" > /dev/null; then
		echo "SKIP $fs: mkfs.$fs missing (pacman -S $(fs_pkg "$fs"))"
		continue
	fi

	IMG=$T/$fs.img
	truncate -s 512M "$IMG"
	fs_mkfs "$fs" "$IMG" NIXSTORE
	GOT=$(blkid -s LABEL -o value "$IMG")
	[ "$GOT" = NIXSTORE ] || {
		echo "FAIL $fs: label is '$GOT', not NIXSTORE"
		rc=1
	}
	[ "$(fs_of_device "$IMG")" = "$fs" ] || {
		echo "FAIL $fs: blkid reports '$(fs_of_device "$IMG")'"
		rc=1
	}
	if fs_check "$fs" "$IMG" > "$T/check-$fs.log" 2>&1; then
		echo "OK   $fs: formatted, labeled, checks clean"
	else
		echo "FAIL $fs: fs_check on a fresh filesystem"
		cat "$T/check-$fs.log"
		rc=1
	fi
done

[ "$rc" = 0 ] && echo PASS || echo FAIL
exit "$rc"
