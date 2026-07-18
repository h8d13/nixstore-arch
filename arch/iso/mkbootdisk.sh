#!/bin/sh -e
# Standalone bootable disk image: the real install, no ISO involved.
# GPT with two partitions: ESP carrying a self-contained UEFI GRUB whose
# only job is to source entries.cfg from the store partition, and the
# NIXSTORE ext4 partition seeded with the store + boot entries for the
# nixarch generations. Committed generations land on the same partition,
# so the system is fully self-hosting after first boot.
#
# Rootless: partitions are built as files and dd'd into place. Flash the
# result to a USB stick (see the printed instructions) or boot it in
# QEMU with OVMF. UEFI only.
#
# Size the image to the target disk: it's sparse (real bytes = store
# size) and dd conv=sparse writes only real blocks, so a full-disk image
# costs nothing. Target size in MiB: lsblk -b -dn -o SIZE /dev/sdX,
# divided by 1048576, rounded down.
#
# usage: mkbootdisk.sh <store-root> [img] [size-MiB]
cd "$(dirname "$0")/../.."
REPO=$PWD

STORE=$1 IMG=${2:-$REPO/build/nixarch-disk.img} SIZE=${3:-4096}
[ -d "$STORE/nix/store" ] || { echo "usage: $0 <store-root> [img] [size-MiB]" >&2; exit 1; }
STORE=$(realpath "$STORE")
SDIR=$STORE/nix/store

# store mtimes are canonicalised (all 1), so there is no "newest" to
# pick with ls -t: require exactly one match
GEN1=
for g in "$SDIR"/*-nixarch-1; do
	[ -d "$g" ] || continue
	[ -z "$GEN1" ] || {
		echo "multiple nixarch-1 generations in $SDIR, remove stale ones first:" >&2
		ls -d "$SDIR"/*-nixarch-1 >&2
		exit 1
	}
	GEN1=$g
done
[ -n "$GEN1" ] || {
	echo "no nixarch generation in $SDIR: run iso/mkiso.sh first" >&2
	exit 1
}
G1=$(basename "$GEN1")

TMP=$(mktemp -d "$REPO/build/disk.XXXXXX")
trap 'unshare -r rm -rf "$TMP"' EXIT

# --- ESP: standalone GRUB, static config, never touched again ----------
cat > "$TMP/embed.cfg" <<EOF
set default=0
set timeout=5
insmod part_gpt
search --no-floppy --set=nixdev --label NIXSTORE
if [ -n "\$nixdev" ]; then
	source (\$nixdev)/entries.cfg
else
	echo "NIXSTORE partition not found"
fi
EOF
grub-mkstandalone -O x86_64-efi --locales= --themes= \
	-o "$TMP/BOOTX64.EFI" "boot/grub/grub.cfg=$TMP/embed.cfg"

truncate -s 64M "$TMP/esp.img"
# mformat, not mkfs.vfat: mtools is already a host dep for mmd/mcopy,
# dosfstools would be one more package for the same FAT32
mformat -i "$TMP/esp.img" -F -v NIXBOOT ::
mmd -i "$TMP/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$TMP/esp.img" "$TMP/BOOTX64.EFI" ::/EFI/BOOT/

# --- store partition: full store root + initial boot entries ------------
# hardlink copy: cheap, and mkfs -d only reads it
mkdir "$TMP/staging"
cp -al "$STORE/nix" "$TMP/staging/nix"
cat > "$TMP/staging/entries.cfg" <<EOF
menuentry "nixarch: $G1" {
	search --no-floppy --set=nixdev --label NIXSTORE
	linux (\$nixdev)/nix/store/$G1/boot/vmlinuz-linux nixgen=$G1 nixsource=disk
	initrd (\$nixdev)/nix/store/$G1/boot/initramfs-linux.img
}
EOF

STORE_MB=$((SIZE - 66))
truncate -s "${STORE_MB}M" "$TMP/store.img"
unshare -r mkfs.ext4 -q -L NIXSTORE -d "$TMP/staging" "$TMP/store.img"

# --- assemble GPT ------------------------------------------------------
rm -f "$IMG"
truncate -s "${SIZE}M" "$IMG"
sfdisk -q "$IMG" <<EOF
label: gpt
start=1MiB, size=64MiB, type=uefi
start=65MiB, type=linux
EOF
dd if="$TMP/esp.img" of="$IMG" bs=1M seek=1 conv=notrunc,sparse status=none
dd if="$TMP/store.img" of="$IMG" bs=1M seek=65 conv=notrunc,sparse status=none

ls -lhs "$IMG"
echo "test:  qemu-system-x86_64 -accel kvm -m 2G -bios /usr/share/ovmf/x64/OVMF.4m.fd \\"
echo "           -drive file=$IMG,format=raw,if=virtio"
echo "flash: dd if=$IMG of=/dev/sdX bs=4M conv=sparse oflag=direct status=progress && sync"
echo "       # sdX = the target, double-check!"
