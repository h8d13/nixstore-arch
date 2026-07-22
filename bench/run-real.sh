#!/bin/sh -e
# Real-world bench: a full rootfs generation through the production
# path, on whatever media hosts the work dir. Four phases:
#   1. capture on/off on the real tree (bench-import --tree)
#   2. fresh install: import-dir into an empty store (cold farm)
#   3. touch update: 1-file variant into the same store; ~every file
#      is a farm dup (config tweak + nixgen-commit)
#   4. package update: variant plus NEWMB of fresh content (default
#      400, think sway + mesa + firmware); dups relink AND every new
#      file needs a farm entry, while the media still digests the
#      import's own writeback. This is the phase that chokes on a
#      USB-backed box.
# Variants are cp -al hardlink copies: no data duplication, dumpPath
# reads through the links.
# Run with the work dir on the target media to measure that media:
#   bench/run-real.sh <tree> /mnt/usb/benchwork
# COLD=1 evicts the tree's and store's data pages (bench/evict-cache.py)
# before each timed import instead of prewarming: the box's real commit
# runs hours after boot, not seconds after building the tree. Cache
# temperature outweighs code: the warm/cold gap measured 2.1x on NVMe.
# usage: bench/run-real.sh [tree] [work-dir]     env: NEWMB, COLD
#        tree default: sole *-arch-base in build/archstore
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/prefix
export PKG_CONFIG_PATH="$P/lib/pkgconfig"

TREE=$1
[ -n "$TREE" ] || {
	for g in "$REPO"/build/archstore/nix/store/*-arch-base; do
		[ -d "$g" ] || continue
		[ -z "$TREE" ] || { echo "several arch-base trees, pass one explicitly" >&2; exit 1; }
		TREE=$g
	done
}
[ -d "$TREE" ] || { echo "no tree to bench (run arch/bootstrap.sh or pass one)" >&2; exit 1; }
TREE=$(realpath "$TREE")

WORKPARENT=${2:-$REPO/build}
mkdir -p "$WORKPARENT"
WORK=$(mktemp -d "$WORKPARENT/benchreal.XXXXXX")
# chmod DIRS only before cleanup: the variants are cp -al copies whose
# file inodes are shared with the source tree; chmod -R here would
# reach through the hardlinks and strip the real store's canonical
# modes (this happened). Dirs are never hardlinked, and rm only needs
# dir write permission.
trap 'find "$WORK" -type d -exec chmod u+w {} + ; rm -rf "$WORK"' EXIT

g++ -std=c++23 -O2 bench/bench-import.cc -o build/bench-import \
	$(pkg-config --cflags --libs nix-store nix-util)
[ -x build/import-dir ] || g++ -std=c++23 -O2 arch/import-dir.cc -o build/import-dir \
	$(pkg-config --cflags --libs nix-store nix-util)

echo "media: $(df --output=source,fstype "$WORKPARENT" | tail -1)"
echo "tree:  $TREE ($(du -sh "$TREE" | cut -f1), $(find "$TREE" | wc -l) entries)"
# warm the page cache once so capture off/on see the same conditions;
# on repeat-import phases the tree is hot either way, like on a box
# where the generation was just built in the overlay. COLD=1 inverts
# this: evict instead, before every timed import
chill() {
	[ -n "$COLD" ] || return 0
	sync
	python3 bench/evict-cache.py "$@" 2>&1
}
if [ -n "$COLD" ]; then
	chill "$TREE"
else
	echo "prewarm: $(tar -cf - -C "$TREE" . | wc -c) bytes read"
fi

echo
echo "--- capture off/on, fresh store each (cold farm)"
LD_LIBRARY_PATH=$P/lib build/bench-import "$WORK/ab" --tree "$TREE"

echo
echo "--- fresh install: import-dir into empty store"
chill "$TREE"
T0=$(date +%s.%N)
LD_LIBRARY_PATH=$P/lib build/import-dir "$WORK/store" gen-base "$TREE"
T1=$(date +%s.%N)
echo "wall: $(echo "$T1 $T0" | awk '{printf "%.1f s", $1 - $2}')"

echo
echo "--- update: 1-file variant into the same store (~all dups)"
# hardlink copy when tree and work dir share a filesystem; full copy
# otherwise (which is also what a box's overlay upper amounts to)
if ! cp -al "$TREE" "$WORK/variant" 2> "$WORK/cp-al.err"; then
	cat "$WORK/cp-al.err"
	echo "cross-device, falling back to full copy"
	rm -rf "$WORK/variant"
	cp -a "$TREE" "$WORK/variant"
fi
chmod u+w "$WORK/variant"
echo "bench-variant $(date +%s)" > "$WORK/variant/.bench-variant"
chill "$WORK/variant" "$WORK/store"
T0=$(date +%s.%N)
LD_LIBRARY_PATH=$P/lib build/import-dir "$WORK/store" gen-next "$WORK/variant"
T1=$(date +%s.%N)
echo "wall: $(echo "$T1 $T0" | awk '{printf "%.1f s", $1 - $2}')"

NEWMB=${NEWMB:-400}
echo
echo "--- package update: variant + ${NEWMB} MiB new content"
cp -al "$WORK/variant" "$WORK/variant2"
# writable dirs where we add entries; dir inodes are fresh copies,
# safe to chmod (file inodes are NOT: shared with the source tree)
chmod u+w "$WORK/variant2" "$WORK/variant2/usr" "$WORK/variant2/usr/lib"
rm "$WORK/variant2/.bench-variant"
# new packages: mostly small files plus a few large ones (firmware
# blobs, gpu drivers). Random bytes: incompressible, never dedups.
mkdir -p "$WORK/variant2/usr/lib/new-pkgs"
NSMALL=$((NEWMB * 3))	# 3 x 200 KiB small files per MiB ~ 60% small
i=0
while [ "$i" -lt "$NSMALL" ]; do
	head -c 204800 /dev/urandom > "$WORK/variant2/usr/lib/new-pkgs/lib$i.so"
	i=$((i + 1))
done
NBIG=$((NEWMB / 10))	# remaining ~40% as 4 MiB blobs
i=0
while [ "$i" -lt "$NBIG" ]; do
	head -c 4194304 /dev/urandom > "$WORK/variant2/usr/lib/new-pkgs/fw$i.bin"
	i=$((i + 1))
done
echo "new: $((NSMALL + NBIG)) files, $(du -sm "$WORK/variant2/usr/lib/new-pkgs" | cut -f1) MiB"
chill "$WORK/variant2" "$WORK/store"
T0=$(date +%s.%N)
LD_LIBRARY_PATH=$P/lib build/import-dir "$WORK/store" gen-pkgs "$WORK/variant2"
T1=$(date +%s.%N)
echo "wall: $(echo "$T1 $T0" | awk '{printf "%.1f s", $1 - $2}')"
