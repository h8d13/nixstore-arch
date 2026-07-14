#!/bin/sh -e
# Build a bootable ISO whose root filesystem is a nixstore generation.
# GRUB menu = generation picker; rollback is choosing an older entry.
#
# Steps: mutate <base> into a bootable generation (kernel + nixgen
# initcpio hook), derive a second generation on top of it (demo that
# entries share the store), squash the whole store, wrap in GRUB ISO.
#
# usage: mkiso.sh <store-root> <base-store-path>
cd "$(dirname "$0")/../.."
REPO=$PWD

STORE=$1 BASE=$2
[ -d "$BASE" ] || { echo "usage: $0 <store-root> <base-store-path>" >&2; exit 1; }
STORE=$(realpath "$STORE") BASE=$(realpath "$BASE")
SDIR=$STORE/nix/store
LABEL=NIXISO

P=$REPO/build/prefix
[ -x "$REPO/build/rm-path" ] || {
	g++ -std=c++23 -O2 examples/rm-path.cc -o build/rm-path \
		$(PKG_CONFIG_PATH=$P/lib/pkgconfig pkg-config --cflags --libs nix-store nix-util)
}

TMP=$(mktemp -d "$REPO/build/iso.XXXXXX")
mkdir "$TMP/upper" "$TMP/work" "$TMP/mnt"
trap 'unshare -r rm -rf "$TMP"' EXIT

# Existing nixarch generations are reused so grub/squash tweaks rebuild
# the ISO without re-running pacman. REBUILD=1 discards them first (any
# change to setup-boot.sh or the initcpio hook invalidates them).
if [ -n "$REBUILD" ]; then
	for g in "$SDIR"/*-nixarch-1 "$SDIR"/*-nixarch-2; do
		[ -d "$g" ] || continue
		LD_LIBRARY_PATH=$P/lib "$REPO/build/rm-path" \
			"$STORE" "$(basename "$g")"
	done
fi
GEN1=$(ls -td "$SDIR"/*-nixarch-1 | head -1)
GEN2=$(ls -td "$SDIR"/*-nixarch-2 | head -1)

# --- gen 1: base + kernel + nixgen hook -------------------------------
# Same sandbox as generation.sh, plus injected files copied to /run/inject
# (copied, not bind-mounted: the pre-import scrub of /run must never be
# able to reach back into the repo).
[ -n "$GEN1" ] || {
cat > "$TMP/inner.sh" <<EOF
set -e
mount -t overlay overlay \
	-o "lowerdir=$BASE,upperdir=$TMP/upper,workdir=$TMP/work" "$TMP/mnt"
mount --rbind /dev "$TMP/mnt/dev"
mount -t proc proc "$TMP/mnt/proc"
rm -f "$TMP/mnt/etc/resolv.conf"
cp /etc/resolv.conf "$TMP/mnt/etc/resolv.conf"
cp -r "$REPO/examples/iso" "$TMP/mnt/run/inject"
mkdir "$TMP/mnt/run/inject/payload"
cp "$REPO/build/import-dir" "$REPO/build/prefix/lib"/libnixstore.so* \
	"$REPO/build/prefix/lib"/libnixutil.so* "$TMP/mnt/run/inject/payload/"

chroot "$TMP/mnt" /usr/bin/env -i \
	HOME=/root PATH=/usr/bin:/usr/sbin TERM=\${TERM:-dumb} \
	sh /run/inject/setup-boot.sh

umount -l "$TMP/mnt/dev"
umount "$TMP/mnt/proc"
rm -rf "$TMP/mnt/tmp"/* "$TMP/mnt/tmp"/.[!.]* "$TMP/mnt/run"/* 2>/dev/null || true
find "$TMP/mnt" \( -type s -o -type p \) -delete

LD_LIBRARY_PATH=$REPO/build/prefix/lib "$REPO/build/import-dir" \
	"$STORE" nixarch-1 "$TMP/mnt"
EOF
unshare -rmpf --kill-child sh "$TMP/inner.sh"
GEN1=$(ls -td "$SDIR"/*-nixarch-1 | head -1)
}
echo "gen1: $GEN1"

# --- gen 2: gen1 + one extra package (the rollback demo) ---------------
[ -n "$GEN2" ] || {
examples/generation.sh "$STORE" "$GEN1" nixarch-2 \
	"pacman -S --noconfirm --needed fastfetch"
GEN2=$(ls -td "$SDIR"/*-nixarch-2 | head -1)
}
echo "gen2: $GEN2"

# --- squash the store ---------------------------------------------------
# mksquashfs comes from gen1 itself (host may not have it). unshare -r so
# store files read as root-owned; gen1's own loader+libs run the binary.
ISO=$TMP/iso
mkdir -p "$ISO/boot/grub"
unshare -r "$GEN1/usr/lib/ld-linux-x86-64.so.2" --library-path "$GEN1/usr/lib" \
	"$GEN1/usr/bin/mksquashfs" "$SDIR" "$ISO/nixstore.squashfs" \
	-comp zstd -noappend -wildcards \
	-e '.links' 'tmp-*' '*/var/cache/pacman/pkg/*'

# gen1 and gen2 share the same kernel/initramfs files
cp "$GEN1/boot/vmlinuz-linux" "$ISO/boot/vmlinuz-linux"
cp "$GEN1/boot/initramfs-linux.img" "$ISO/boot/initramfs-linux.img"

G1=$(basename "$GEN1") G2=$(basename "$GEN2")
cat > "$ISO/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=5

# generations committed from inside the box (nixgen-commit) live on the
# NIXSTORE disk; pull their menu entries in when it's attached
search --no-floppy --set=nixdev --label NIXSTORE
if [ -n "\$nixdev" ]; then
	source (\$nixdev)/entries.cfg
fi

menuentry "nixarch: $G2" {
	linux /boot/vmlinuz-linux nixgen=$G2 nixlabel=$LABEL console=ttyS0,115200 console=tty0
	initrd /boot/initramfs-linux.img
}
menuentry "nixarch: $G1 (rollback)" {
	linux /boot/vmlinuz-linux nixgen=$G1 nixlabel=$LABEL console=ttyS0,115200 console=tty0
	initrd /boot/initramfs-linux.img
}
EOF

grub-mkrescue -o "$REPO/build/nixarch.iso" "$ISO" -volid "$LABEL"
ls -lh "$REPO/build/nixarch.iso"
echo "boot test: examples/iso/boot-test.sh"
