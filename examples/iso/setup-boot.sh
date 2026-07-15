#!/bin/sh -e
# Runs inside the overlay chroot (userns fake root). Turns a plain Arch
# generation into a bootable one: nixgen initcpio hook, kernel, autologin,
# squashfs-tools (used later to squash the store from inside the store).
# Injected files are copied to /run/inject by mkiso.sh; /run is scrubbed
# before import so none of this scaffolding ends up in the generation.
I=/run/inject

install -Dm644 "$I/initcpio-install-nixgen" /etc/initcpio/install/nixgen
install -Dm644 "$I/initcpio-hook-nixgen" /etc/initcpio/hooks/nixgen
install -Dm644 "$I/mkinitcpio.conf" /etc/mkinitcpio.conf

echo nixarch > /etc/hostname
# bake the machine identity into the generation: without it every boot
# is a systemd "first boot" (tmpfs upper) and re-applies preset policy.
# Generations of one machine sharing an id is the correct semantic.
systemd-machine-id-setup
# networking out of the box: DHCP on any en* (QEMU user NAT, real ethernet)
cat > /etc/systemd/network/dhcp.network <<EOF
[Match]
Name=en*

[Network]
DHCP=yes
EOF
systemctl enable systemd-networkd systemd-resolved
# firstboot would prompt for locale/timezone on the console and hang a
# headless boot
ln -sf /dev/null /etc/systemd/system/systemd-firstboot.service

install -d /etc/systemd/system/getty@tty1.service.d \
	/etc/systemd/system/serial-getty@.service.d
cat > /etc/systemd/system/getty@tty1.service.d/autologin.conf <<EOF
[Service]
ExecStart=
ExecStart=-/usr/bin/agetty --autologin root --noclear %I \$TERM
EOF
cat > /etc/systemd/system/serial-getty@.service.d/autologin.conf <<EOF
[Service]
ExecStart=
ExecStart=-/usr/bin/agetty --autologin root --keep-baud 115200,57600,38400,9600 %I \$TERM
EOF

# in-box generation tooling: import-dir + its nixstore libs (payload
# staged by mkiso.sh) and the commit wrapper
install -d /usr/local/bin /usr/local/lib
install -m755 "$I/payload/import-dir" /usr/local/bin/import-dir
install -m755 "$I/payload/rm-path" /usr/local/bin/rm-path
cp -P "$I"/payload/libnix*.so* /usr/local/lib/
install -m755 "$I/nixgen-commit" /usr/local/bin/nixgen-commit
install -m755 "$I/nixgen-remove" /usr/local/bin/nixgen-remove
install -m755 "$I/nixgen-update" /usr/local/bin/nixgen-update
install -m755 "$I/nixgen-savemeta" /usr/local/bin/nixgen-savemeta
install -m755 "$I/nixgen-restmeta" /usr/local/bin/nixgen-restmeta

# store import canonicalises permissions (dirs 0555, no setuid/sticky/
# ownership/caps); replay the captured manifest before anything else
# runs. See nixgen-savemeta/-restmeta
cat > /etc/systemd/system/nixgen-perms.service <<EOF
[Unit]
Description=Restore store-canonicalised permissions
DefaultDependencies=no
After=systemd-remount-fs.service
Before=sysinit.target systemd-tmpfiles-setup.service

[Service]
Type=oneshot
ExecStart=/usr/local/bin/nixgen-restmeta
RemainAfterExit=yes

[Install]
WantedBy=sysinit.target
EOF
systemctl enable nixgen-perms

cat > /root/.bash_profile <<'EOF'
echo "=== NIXARCH BOOT OK ==="
grep -o 'nixgen=[^ ]*' /proc/cmdline
EOF

# tools + runtime deps of import-dir/libnixstore first, so the ALPM
# mkinitcpio hook exists before the kernel lands
pacman -Sy --noconfirm --needed mkinitcpio squashfs-tools \
	libblake3 boost-libs libsodium onetbb sqlite icu libxml2 libseccomp brotli

# kernel pinned ~30 days back, so nixgen-update against live repos
# performs a real version-to-version kernel upgrade (exercised by
# iso/update-test.sh). The Arch Linux Archive mirrors the live repo
# layout, so pin = swap the mirrorlist for this one transaction (a
# custom [section] would 404: pacman fetches <section>.db, ALA only
# serves core.db). Install triggers mkinitcpio -P via ALPM hook, which
# picks up /etc/mkinitcpio.conf above and bakes the nixgen hook in
PIN=$(date -d '-30 days' +%Y/%m/%d)
mv /etc/pacman.d/mirrorlist /etc/pacman.d/mirrorlist.live
echo "Server = https://archive.archlinux.org/repos/$PIN/\$repo/os/\$arch" \
	> /etc/pacman.d/mirrorlist
# -Syy: the archive db is *older* than the cached live one; plain -Sy
# gets a 304 and silently resolves against the live db (ALA's rewrite
# then serves any package file, masking the broken pin)
pacman -Syy --noconfirm linux
# back to live mirrors: shipped generations must update from them
mv /etc/pacman.d/mirrorlist.live /etc/pacman.d/mirrorlist
pacman -Syy

# resolved-managed DNS. Last on purpose: pacman above still needed the
# host resolv.conf that the sandbox copies in
ln -sfn /run/systemd/resolve/resolv.conf /etc/resolv.conf
