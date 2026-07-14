#!/bin/sh -e
# Run a command (default: interactive bash) inside a generation from the
# store. Same sandbox as generation.sh, but the overlay is throwaway:
# writes work and vanish on exit, the base store path stays immutable.
# usage: enter.sh <base-store-path> [cmd]
cd "$(dirname "$0")/.."
REPO=$PWD

BASE=$1
[ -d "$BASE" ] || { echo "usage: $0 <base-store-path> [cmd]" >&2; exit 1; }
shift
CMD=${*:-/usr/bin/bash}

TMP=$(mktemp -d "$REPO/build/enter.XXXXXX")
mkdir "$TMP/upper" "$TMP/work" "$TMP/mnt"
# overlayfs creates work/work with mode 000; userns root can still rm it
trap 'unshare -r rm -rf "$TMP"' EXIT

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
EOF

unshare -rmpf --kill-child sh "$TMP/inner.sh"
