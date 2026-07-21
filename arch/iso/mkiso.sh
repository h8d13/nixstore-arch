#!/bin/sh -e
# Build a bootable ISO whose root filesystem is a nixstore generation.
# GRUB menu = generation picker; rollback is choosing an older entry.
#
# Steps: mutate <base> into a bootable generation (kernel + nixgen
# initcpio hook), squash the whole store, wrap in GRUB ISO.
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
for t in rm-path import-dir export-path import-path; do
	# -nt: also recompile when the source is newer than the binary
	[ "$REPO/build/$t" -nt "arch/$t.cc" ] || g++ -std=c++23 -O2 \
		"arch/$t.cc" -o "build/$t" \
		$(PKG_CONFIG_PATH=$P/lib/pkgconfig pkg-config --cflags --libs nix-store nix-util)
done

TMP=$(mktemp -d "$REPO/build/iso.XXXXXX")
trap 'unshare -r rm -rf "$TMP"' EXIT

# Existing nixarch generations are reused so grub/squash tweaks rebuild
# the ISO without re-running pacman. REBUILD=1 discards them first (any
# change to setup-boot.sh or the initcpio hook invalidates them).
if [ -n "$REBUILD" ]; then
	for g in "$SDIR"/*-nixarch-1; do
		[ -d "$g" ] || continue
		LD_LIBRARY_PATH=$P/lib "$REPO/build/rm-path" \
			"$STORE" "$(basename "$g")"
	done
fi
# name reuse means several *-nixarch-1 paths can coexist (mtimes are
# canonicalised, no "newest" to pick): refuse to guess
GEN1=
for g in "$SDIR"/*-nixarch-1; do
	[ -d "$g" ] || continue
	[ -z "$GEN1" ] || {
		echo "multiple nixarch-1 generations in $SDIR, REBUILD=1 to clear:" >&2
		ls -d "$SDIR"/*-nixarch-1 >&2
		exit 1
	}
	GEN1=$g
done

# --- gen 1: base + kernel + nixgen hook -------------------------------
# generation.sh sandbox with the box tooling (arch/nixgen) and iso
# scaffolding (initcpio hooks, config, import-dir payload) injected at
# /run/inject
[ -n "$GEN1" ] || {
	mkdir "$TMP/inject"
	cp "$REPO"/arch/nixgen/nixgen-* "$TMP/inject/"
	cp "$REPO/arch/iso/setup-boot.sh" "$REPO/arch/iso/mkinitcpio.conf" \
		"$REPO"/arch/iso/initcpio-* "$TMP/inject/"
	mkdir "$TMP/inject/payload"
	cp "$REPO/build/import-dir" "$REPO/build/rm-path" \
		"$REPO/build/export-path" "$REPO/build/import-path" \
		"$P/lib"/libnixstore.so* \
		"$P/lib"/libnixutil.so* "$TMP/inject/payload/"
	# sh -e: run via interpreter ignores the shebang's -e; without it a
	# failed setup step imports a broken generation instead of aborting
	INJECT=$TMP/inject GENOUT=$TMP/gen1path arch/generation.sh \
		"$STORE" "$BASE" nixarch-1 "sh -e /run/inject/setup-boot.sh"
	GEN1=$(cat "$TMP/gen1path")
}
echo "gen1: $GEN1"

# --- squash the store ---------------------------------------------------
# mksquashfs comes from gen1 itself (host may not have it). unshare -r so
# store files read as root-owned; gen1's own loader+libs run the binary.
ISO=$TMP/iso
mkdir -p "$ISO/boot/grub"
unshare -r "$GEN1/usr/lib/ld-linux-x86-64.so.2" --library-path "$GEN1/usr/lib" \
	"$GEN1/usr/bin/mksquashfs" "$SDIR" "$ISO/nixstore.squashfs" \
	-comp zstd -noappend -wildcards -no-hardlinks \
	-e '.links' 'tmp-*' '*/var/cache/pacman/pkg/*'
	# -no-hardlinks: store dedup hardlinks would survive into the squash
	# and alias unrelated paths through one inode under the boot overlay
	# (writing wtmp rewrote /etc/subuid); squashfs dedups by content, so
	# splitting them costs nothing

cp "$GEN1/boot/vmlinuz-linux" "$ISO/boot/vmlinuz-linux"
cp "$GEN1/boot/initramfs-linux.img" "$ISO/boot/initramfs-linux.img"

G1=$(basename "$GEN1")
cat > "$ISO/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=5

# rd.systemd.gpt_auto=0: the initrd's root comes from nixgen=, so
# systemd-gpt-auto-generator must not go looking for a root partition of
# its own and race the generated sysroot.mount
menuentry "nixarch ISO (read-only): $G1" {
	linux /boot/vmlinuz-linux nixgen=$G1 nixlabel=$LABEL rd.systemd.gpt_auto=0 console=ttyS0,115200 console=tty0
	initrd /boot/initramfs-linux.img
}

# generations committed from inside the box (nixgen-commit/-update) live
# on the NIXSTORE disk; pull their entries in when it's attached, under
# a submenu so the ISO entry stays unmistakable at the top level. On
# installed disks the same entries.cfg is top-level on purpose: there it
# is the primary boot path. Known cost: BIOS grub probing for an absent
# label takes ~10s, paid on every diskless boot
search --no-floppy --set=nixdev --label NIXSTORE
if [ -n "\$nixdev" ]; then
	submenu "generations on NIXSTORE disk ->" {
		search --no-floppy --set=nixdev --label NIXSTORE
		source (\$nixdev)/entries.cfg
	}
fi
EOF

grub-mkrescue -o "$REPO/build/nixarch.iso" "$ISO" -volid "$LABEL"
ls -lh "$REPO/build/nixarch.iso"
echo "boot test: arch/tests/boot-test.sh"
