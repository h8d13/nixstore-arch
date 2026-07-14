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
cp -P "$I"/payload/libnix*.so* /usr/local/lib/
install -m755 "$I/nixgen-commit" /usr/local/bin/nixgen-commit

cat > /root/.bash_profile <<'EOF'
echo "=== NIXARCH BOOT OK ==="
grep -o 'nixgen=[^ ]*' /proc/cmdline
echo "generations on this ISO:"
ls /nixstore
fastfetch --logo none || echo "(fastfetch not in this generation)"
EOF

# kernel install triggers mkinitcpio -P via ALPM hook, which picks up
# /etc/mkinitcpio.conf above and bakes the nixgen hook into the image.
# trailing libs = runtime deps of import-dir/libnixstore
pacman -Sy --noconfirm --needed linux mkinitcpio squashfs-tools \
	libblake3 boost-libs libsodium onetbb sqlite icu libxml2 libseccomp brotli

# resolved-managed DNS. Last on purpose: pacman above still needed the
# host resolv.conf that the sandbox copies in
ln -sfn /run/systemd/resolve/resolv.conf /etc/resolv.conf
