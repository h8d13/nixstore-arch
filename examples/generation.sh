#!/bin/sh -e
# Create a new system generation: overlay-mount a base generation from the
# store, mutate it with a command in a chroot (userns, no root needed),
# import the merged result back as a new content-addressed store path.
# Dedup makes each generation cost only its diff.
#
# usage: generation.sh <store-root> <base-store-path> <new-name> [cmd]
#        cmd defaults to an interactive shell.
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/prefix

STORE_ROOT=$1 BASE=$2 NAME=$3
[ -n "$NAME" ] || { echo "usage: $0 <store-root> <base-store-path> <new-name> [cmd]" >&2; exit 1; }
shift 3
CMD=${*:-/usr/bin/bash}

[ -x "$REPO/build/import-dir" ] || {
	g++ -std=c++23 -O2 examples/import-dir.cc -o build/import-dir \
		$(PKG_CONFIG_PATH=$P/lib/pkgconfig pkg-config --cflags --libs nix-store nix-util)
}

TMP=$(mktemp -d "$REPO/build/gen.XXXXXX")
mkdir "$TMP/upper" "$TMP/work" "$TMP/mnt"
# overlayfs creates work/work with mode 000; userns root can still rm it
trap 'unshare -r rm -rf "$TMP"' EXIT

# Inner script runs as fake root in fresh mount+pid namespaces. Mounts die
# with the namespace; proc/dev are unmounted before import so the merged
# view contains only real files.
cat > "$TMP/inner.sh" <<EOF
set -e
mount -t overlay overlay \
	-o "lowerdir=$BASE,upperdir=$TMP/upper,workdir=$TMP/work" "$TMP/mnt"
mount --rbind /dev "$TMP/mnt/dev"
mount -t proc proc "$TMP/mnt/proc"
rm -f "$TMP/mnt/etc/resolv.conf"
cp /etc/resolv.conf "$TMP/mnt/etc/resolv.conf"

chroot "$TMP/mnt" /usr/bin/env -i \
	HOME=/root PATH=/usr/bin:/usr/sbin TERM=\${TERM:-dumb} \
	sh -c '$CMD'

umount -l "$TMP/mnt/dev"
umount "$TMP/mnt/proc"

# scrub what a generation must not capture: scratch dirs, and
# sockets/fifos (NAR cannot represent them)
rm -rf "$TMP/mnt/tmp"/* "$TMP/mnt/tmp"/.[!.]* "$TMP/mnt/run"/* 2>/dev/null || true
find "$TMP/mnt" \( -type s -o -type p \) -delete

LD_LIBRARY_PATH=$P/lib "$REPO/build/import-dir" "$STORE_ROOT" "$NAME" "$TMP/mnt"
EOF

unshare -rmpf --kill-child sh "$TMP/inner.sh"
