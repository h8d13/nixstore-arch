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

# multi-uid sandbox when /etc/subuid holds a range for us (chowns
# inside succeed); single-uid fallback, chowns fail soft
UNSHARE="unshare --map-auto --map-root-user"
$UNSHARE true || {
	echo "WARN: no subuid range, single-uid sandbox (chowns fail soft)" >&2
	UNSHARE="unshare --map-root-user"
}

TMP=$(mktemp -d "$REPO/build/enter.XXXXXX")
mkdir "$TMP/upper" "$TMP/work" "$TMP/mnt"
# overlayfs creates work/work with mode 000; userns root can still rm it
trap '$UNSHARE rm -rf "$TMP"' EXIT

cat > "$TMP/inner.sh" <<EOF
set -e
# userxattr: dir removals over the lower need it in a userns (see
# generation.sh)
mount -t overlay overlay \
	-o "lowerdir=$BASE,upperdir=$TMP/upper,workdir=$TMP/work,userxattr" "$TMP/mnt"
# replay captured modes over the canonical 0555 lower (pacman rejects a
# no-write-bits cachedir even as root); copy-ups land in the throwaway
# upper. See nixgen-savemeta
if [ -f "$BASE/etc/nixgen/perms" ]; then
	"$REPO/arch/nixgen/nixgen-restmeta" "$TMP/mnt"
fi
# minimal /dev + tmpfs run/tmp, same sandbox surface as generation.sh
mount -t tmpfs -o mode=0755,nosuid dev "$TMP/mnt/dev"
for d in full null random tty urandom zero; do
	touch "$TMP/mnt/dev/\$d"
	mount --bind "/dev/\$d" "$TMP/mnt/dev/\$d"
done
ln -s /proc/self/fd "$TMP/mnt/dev/fd"
ln -s /proc/self/fd/0 "$TMP/mnt/dev/stdin"
ln -s /proc/self/fd/1 "$TMP/mnt/dev/stdout"
ln -s /proc/self/fd/2 "$TMP/mnt/dev/stderr"
mount -t proc proc "$TMP/mnt/proc"
mount -t tmpfs -o mode=0755,nosuid,nodev run "$TMP/mnt/run"
mount -t tmpfs -o mode=1777,strictatime,nodev,nosuid tmp "$TMP/mnt/tmp"
rm -f "$TMP/mnt/etc/resolv.conf"
cp /etc/resolv.conf "$TMP/mnt/etc/resolv.conf"

chroot "$TMP/mnt" /usr/bin/env -i \
	HOME=/root PATH=/usr/bin:/usr/sbin TERM=\${TERM:-dumb} \
	sh -c '$CMD'
EOF

$UNSHARE -mpf --kill-child sh "$TMP/inner.sh"
