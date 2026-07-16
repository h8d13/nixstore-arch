#!/bin/sh -e
# Fresh base generation from the official Arch bootstrap tarball:
# download (if missing), extract, make pacman usable inside the userns
# sandbox (mirror list, CheckSpace confused by overlay df, DownloadUser
# off so the single-uid fallback sandbox works too), init the keyring,
# import the result as arch-base. Prints the store path to build on with
# generation.sh / iso/mkiso.sh.
# usage: bootstrap.sh <store-root>
cd "$(dirname "$0")/.."
REPO=$PWD

STORE=$1
[ -n "$STORE" ] || { echo "usage: $0 <store-root>" >&2; exit 1; }
mkdir -p "$STORE"
STORE=$(realpath "$STORE")

P=$REPO/build/prefix
[ -x "$REPO/build/import-dir" ] || {
	g++ -std=c++23 -O2 arch/import-dir.cc -o build/import-dir \
		$(PKG_CONFIG_PATH=$P/lib/pkgconfig pkg-config --cflags --libs nix-store nix-util)
}

TARBALL=$REPO/build/archlinux-bootstrap-x86_64.tar.zst
[ -f "$TARBALL" ] || curl -L -o "$TARBALL" \
	https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst

# multi-uid sandbox when /etc/subuid holds a range for us (chowns
# inside succeed); single-uid fallback, chowns fail soft
UNSHARE="unshare --map-auto --map-root-user"
$UNSHARE true || {
	echo "WARN: no subuid range, single-uid sandbox (chowns fail soft)" >&2
	UNSHARE="unshare --map-root-user"
}

TMP=$(mktemp -d "$REPO/build/bootstrap.XXXXXX")
mkdir "$TMP/root"
trap '$UNSHARE rm -rf "$TMP"' EXIT

cat > "$TMP/inner.sh" <<EOF
set -e
# single-uid namespace: chown to other uids is impossible and the whole
# model is root-owned files anyway
tar --strip-components=1 --no-same-owner -C "$TMP/root" -xf "$TARBALL"

sed -i 's/^CheckSpace/#CheckSpace/; s/^DownloadUser/#DownloadUser/' \
	"$TMP/root/etc/pacman.conf"
echo 'Server = https://geo.mirror.pkgbuild.com/\$repo/os/\$arch' \
	> "$TMP/root/etc/pacman.d/mirrorlist"

# minimal /dev + tmpfs run/tmp, same sandbox surface as generation.sh
mount -t tmpfs -o mode=0755,nosuid dev "$TMP/root/dev"
for d in full null random tty urandom zero; do
	touch "$TMP/root/dev/\$d"
	mount --bind "/dev/\$d" "$TMP/root/dev/\$d"
done
ln -s /proc/self/fd "$TMP/root/dev/fd"
ln -s /proc/self/fd/0 "$TMP/root/dev/stdin"
ln -s /proc/self/fd/1 "$TMP/root/dev/stdout"
ln -s /proc/self/fd/2 "$TMP/root/dev/stderr"
mount -t proc proc "$TMP/root/proc"
mount -t tmpfs -o mode=0755,nosuid,nodev run "$TMP/root/run"
mount -t tmpfs -o mode=1777,strictatime,nodev,nosuid tmp "$TMP/root/tmp"
rm -f "$TMP/root/etc/resolv.conf"
cp /etc/resolv.conf "$TMP/root/etc/resolv.conf"

chroot "$TMP/root" /usr/bin/env -i \
	HOME=/root PATH=/usr/bin:/usr/sbin TERM=dumb \
	sh -c 'pacman-key --init && pacman-key --populate archlinux'

umount -R "$TMP/root/dev"
umount "$TMP/root/proc" "$TMP/root/run" "$TMP/root/tmp"

# gpg-agent leaves sockets in etc/pacman.d/gnupg; NAR can't hold them
find "$TMP/root" \( -type s -o -type p \) -delete

"$REPO/arch/iso/nixgen-savemeta" "$TMP/root"
LD_LIBRARY_PATH=$REPO/build/prefix/lib "$REPO/build/import-dir" \
	"$STORE" arch-base "$TMP/root"
EOF
$UNSHARE -mpf --kill-child sh "$TMP/inner.sh"
