#!/bin/sh -e
# Fresh base generation from the official Arch bootstrap tarball:
# download (if missing), extract, make pacman usable inside the userns
# sandbox (mirror list, CheckSpace confused by overlay df, DownloadUser
# can't drop privileges in a single-uid namespace), init the keyring,
# import the result as arch-base. Prints the store path to build on with
# generation.sh / iso/mkiso.sh.
# usage: bootstrap.sh <store-root>
cd "$(dirname "$0")/.."
REPO=$PWD

STORE=$1
[ -n "$STORE" ] || { echo "usage: $0 <store-root>" >&2; exit 1; }
mkdir -p "$STORE"
STORE=$(realpath "$STORE")

TARBALL=$REPO/build/archlinux-bootstrap-x86_64.tar.zst
[ -f "$TARBALL" ] || curl -L -o "$TARBALL" \
	https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst

TMP=$(mktemp -d "$REPO/build/bootstrap.XXXXXX")
mkdir "$TMP/root"
trap 'unshare -r rm -rf "$TMP"' EXIT

cat > "$TMP/inner.sh" <<EOF
set -e
# single-uid namespace: chown to other uids is impossible and the whole
# model is root-owned files anyway
tar --strip-components=1 --no-same-owner -C "$TMP/root" -xf "$TARBALL"

sed -i 's/^CheckSpace/#CheckSpace/; s/^DownloadUser/#DownloadUser/' \
	"$TMP/root/etc/pacman.conf"
echo 'Server = https://geo.mirror.pkgbuild.com/\$repo/os/\$arch' \
	> "$TMP/root/etc/pacman.d/mirrorlist"

mount --rbind /dev "$TMP/root/dev"
mount -t proc proc "$TMP/root/proc"
rm -f "$TMP/root/etc/resolv.conf"
cp /etc/resolv.conf "$TMP/root/etc/resolv.conf"

chroot "$TMP/root" /usr/bin/env -i \
	HOME=/root PATH=/usr/bin:/usr/sbin TERM=dumb \
	sh -c 'pacman-key --init && pacman-key --populate archlinux'

umount -l "$TMP/root/dev"
umount "$TMP/root/proc"

# gpg-agent leaves sockets in etc/pacman.d/gnupg; NAR can't hold them
rm -rf "$TMP/root/tmp"/* "$TMP/root/tmp"/.[!.]* "$TMP/root/run"/* 2>/dev/null || true
find "$TMP/root" \( -type s -o -type p \) -delete

LD_LIBRARY_PATH=$REPO/build/prefix/lib "$REPO/build/import-dir" \
	"$STORE" arch-base "$TMP/root"
EOF
unshare -rmpf --kill-child sh "$TMP/inner.sh"
