#!/bin/sh -e
# Create a new system generation: overlay-mount a base generation from the
# store, mutate it with a command in a chroot (userns, no root needed),
# import the merged result back as a new content-addressed store path.
# Dedup makes each generation cost only its diff.
#
# usage: generation.sh <store-root> <base-store-path> <new-name> [cmd]
#        cmd defaults to an interactive shell.
#        env INJECT=<dir>: copied to /run/inject inside the sandbox
#        (visible to cmd, scrubbed with the rest of /run before import).
#        env GENOUT=<file>: the imported store path is written there
#        (stdout belongs to cmd; globbing the store by name is ambiguous
#        on reuse and mtimes are canonicalised, so no ls -t either).
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/prefix

STORE_ROOT=$1 BASE=$2 NAME=$3
[ -n "$NAME" ] || { echo "usage: $0 <store-root> <base-store-path> <new-name> [cmd]" >&2; exit 1; }
shift 3
CMD=${*:-/usr/bin/bash}

# -nt: also recompile when the source is newer than the binary
[ "$REPO/build/import-dir" -nt arch/import-dir.cc ] || {
	g++ -std=c++23 -O2 arch/import-dir.cc -o build/import-dir \
		$(PKG_CONFIG_PATH=$P/lib/pkgconfig pkg-config --cflags --libs nix-store nix-util)
}

# multi-uid sandbox when /etc/subuid holds a range for us: chowns from
# pacman hooks (sysusers et al) succeed and land in the perms manifest.
# Single-uid fallback keeps working, chowns fail soft
UNSHARE="unshare --map-auto --map-root-user"
$UNSHARE true || {
	echo "WARN: no subuid range, single-uid sandbox (chowns fail soft)" >&2
	UNSHARE="unshare --map-root-user"
}

TMP=$(mktemp -d "$REPO/build/gen.XXXXXX")
mkdir "$TMP/upper" "$TMP/work" "$TMP/mnt"
# overlayfs creates work/work with mode 000; userns root can still rm it
trap '$UNSHARE rm -rf "$TMP"' EXIT

# Inner script runs as fake root in fresh mount+pid namespaces. Mounts die
# with the namespace; proc/dev are unmounted before import so the merged
# view contains only real files.
# Not arch-chroot: its root mode mounts devtmpfs/sysfs (EPERM in a userns);
# its -N mode re-execs under unshare --map-auto unconditionally, which
# cannot nest inside this sandbox (uid_map write EPERM) and standalone
# offers no hook to mount the overlay before the chroot. Its
# unshare_setup makes the same substitutions done manually here.
cat > "$TMP/inner.sh" <<EOF
set -e
# userxattr: whiteout-only deletes work without it, but removing a
# dir that exists in the lower needs an opaque marker xattr, and
# trusted.* is off-limits in a userns (pacman upgrades hit this
# deleting old db entry dirs: EIO, stale duplicate entries)
mount -t overlay overlay \
	-o "lowerdir=$BASE,upperdir=$TMP/upper,workdir=$TMP/work,userxattr" "$TMP/mnt"
# give the merged view the modes the base captured before import
# (store lower is canonical 0555); see nixgen-savemeta
if [ -f "$BASE/etc/nixgen/perms" ]; then
	"$REPO/arch/nixgen/nixgen-restmeta" "$TMP/mnt"
else
	echo "WARN: $BASE has no etc/nixgen/perms; canonical 0555 modes" \
		"stay, pacman will warn (re-bootstrap the base)" >&2
fi
# minimal /dev (arch-chroot unshare_setup parity): six pseudo-device
# binds instead of an rbind of host /dev, so tmpfiles/udev hooks find
# no real nodes to chown (fchownat EPERM noise, failed pacman hook).
# /run and /tmp are namespace tmpfs: scaffolding and scratch never
# reach the upper
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
if [ -n "\$INJECT" ]; then
	cp -r "\$INJECT" "$TMP/mnt/run/inject"
fi

chroot "$TMP/mnt" /usr/bin/env -i \
	HOME=/root PATH=/usr/bin:/usr/sbin TERM=\${TERM:-dumb} \
	sh -c '$CMD'

# lazy: a hook-spawned daemon (gpg-agent after a keyring update) can
# hold /dev/null busy past the chroot command; the detach is immediate
# for the import below, the mounts die with the namespace
umount -Rl "$TMP/mnt/dev"
umount -l "$TMP/mnt/proc" "$TMP/mnt/run" "$TMP/mnt/tmp"

# resolv.conf: the host copy above was build scaffolding. A command that
# replaced it (e.g. symlink to systemd-resolved) keeps its version;
# otherwise the generation inherits whatever its base had.
if [ ! -L "$TMP/mnt/etc/resolv.conf" ] \
		&& cmp -s /etc/resolv.conf "$TMP/mnt/etc/resolv.conf"; then
	rm -f "$TMP/mnt/etc/resolv.conf"
	if [ -e "$BASE/etc/resolv.conf" ] || [ -L "$BASE/etc/resolv.conf" ]; then
		cp -P "$BASE/etc/resolv.conf" "$TMP/mnt/etc/resolv.conf"
	fi
fi

# scrub what a generation must not capture: sockets/fifos (NAR cannot
# represent them; gpg-agent drops them in /etc/pacman.d/gnupg).
# /tmp and /run were namespace tmpfs, nothing of them is in the upper
find "$TMP/mnt" \( -type s -o -type p \) -delete

"$REPO/arch/nixgen/nixgen-savemeta" "$TMP/mnt"
LD_LIBRARY_PATH=$P/lib "$REPO/build/import-dir" "$STORE_ROOT" "$NAME" "$TMP/mnt" \
	> "$TMP/imported"
EOF

$UNSHARE -mpf --kill-child sh "$TMP/inner.sh"

[ -z "$GENOUT" ] || cp "$TMP/imported" "$GENOUT"
