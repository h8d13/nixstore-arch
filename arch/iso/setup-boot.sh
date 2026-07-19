#!/bin/sh -e
# Runs inside the overlay chroot (userns fake root). Turns a plain Arch
# generation into a bootable one: nixgen initcpio hook, kernel, autologin,
# squashfs-tools (used later to squash the store from inside the store).
# Injected files are copied to /run/inject by mkiso.sh; /run is scrubbed
# before import so none of this scaffolding ends up in the generation.
I=/run/inject

install -Dm644 "$I/initcpio-install-nixgen" /etc/initcpio/install/nixgen
install -Dm644 "$I/initcpio-hook-nixgen" /etc/initcpio/hooks/nixgen
# conf.d drop-in, not /etc/mkinitcpio.conf: pacman owns that file and
# pre-placing it yields a .pacnew warning at kernel install
install -Dm644 "$I/mkinitcpio.conf" /etc/mkinitcpio.conf.d/nixgen.conf

echo nixarch > /etc/hostname
# login banner: stock /etc/issue renders \S{PRETTY_NAME} from
# os-release. Override the pretty name via the /etc precedence file
# (stock is a symlink into /usr/lib; rm first or > would follow it);
# NAME/ID stay arch for tooling
rm /etc/os-release
sed 's/^PRETTY_NAME=.*/PRETTY_NAME="nixarch"/' /usr/lib/os-release > /etc/os-release
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

# conditional autologin (nixgen-getty): prompt-free only while root is
# passwordless; passwd root restores normal login on the next getty.
# Template-wide drop-in: logind autovts (tty2+) get it too, root's
# stock shadow entry "*" can never pass a password prompt
install -m755 "$I/nixgen-getty" /usr/local/bin/nixgen-getty
install -d /etc/systemd/system/getty@.service.d \
	/etc/systemd/system/serial-getty@.service.d
cat > /etc/systemd/system/getty@.service.d/autologin.conf <<EOF
[Service]
ExecStart=
ExecStart=-/usr/local/bin/nixgen-getty --noclear %I \$TERM
EOF
cat > /etc/systemd/system/serial-getty@.service.d/autologin.conf <<EOF
[Service]
ExecStart=
ExecStart=-/usr/local/bin/nixgen-getty --keep-baud 115200,57600,38400,9600 %I \$TERM
EOF

# in-box generation tooling: import-dir + its nixstore libs (payload
# staged by mkiso.sh) and the commit wrapper
install -d /usr/local/bin /usr/local/lib
install -m755 "$I/payload/import-dir" /usr/local/bin/import-dir
install -m755 "$I/payload/rm-path" /usr/local/bin/rm-path
install -m755 "$I/payload/export-path" /usr/local/bin/export-path
install -m755 "$I/payload/import-path" /usr/local/bin/import-path
cp -P "$I"/payload/libnix*.so* /usr/local/lib/
install -m755 "$I/nixgen-commit" /usr/local/bin/nixgen-commit
install -m755 "$I/nixgen-remove" /usr/local/bin/nixgen-remove
install -m755 "$I/nixgen-update" /usr/local/bin/nixgen-update
install -m755 "$I/nixgen-switch" /usr/local/bin/nixgen-switch
install -m755 "$I/nixgen-listid" /usr/local/bin/nixgen-listid
install -m755 "$I/nixgen-diffid" /usr/local/bin/nixgen-diffid
install -m755 "$I/nixgen-setup" /usr/local/bin/nixgen-setup
install -m755 "$I/nixgen-help" /usr/local/bin/nixgen-help
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
# DefaultDependencies=no skips the implicit shutdown conflict; without
# it the unit's active state survives a soft-reboot and it never
# re-runs in the switched root
Conflicts=shutdown.target
Before=shutdown.target

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
# soft switches (nixgen-switch) leave the boot-time cmdline stale
if [ -s /run/nixgen-active ]; then
	echo "nixgen=$(cat /run/nixgen-active) (soft)"
else
	grep -o 'nixgen=[^ ]*' /proc/cmdline
fi
EOF

# kernel install triggers mkinitcpio -P via ALPM hook, which picks up
# the conf.d drop-in above and bakes the nixgen hook into the image.
# -Syu, not -Sy: the base is a monthly bootstrap tarball, installing
# against a fresh db without upgrading it would ship gen1 as a partial
# upgrade. nixgen-setup's extra deps (grub, dosfstools) install on
# demand there, not into every generation; trailing libs = runtime
# deps of import-dir/libnixstore
pacman -Syu --noconfirm --needed linux mkinitcpio squashfs-tools diffutils \
	libblake3 boost-libs libsodium onetbb sqlite icu libxml2 libseccomp brotli

# resolved-managed DNS. Last on purpose: pacman above still needed the
# host resolv.conf that the sandbox copies in
ln -sfn /run/systemd/resolve/resolv.conf /etc/resolv.conf
