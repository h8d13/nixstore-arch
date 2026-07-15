```
  ┌──────────────────────────────────────┬───────────────────────────────────────────────────────────────────┐
  │                script                │                               role                                │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ import-dir <store-root> <name> <dir> │ commit any tree as a content-addressed generation + dedup         │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ generation.sh <store-root> <base>    │ mutate base in overlay sandbox → import result as new generation  │
  │ <name> [cmd]                         │                                                                   │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ enter.sh <base> [cmd]                │ same sandbox, throwaway: inspect/test/experiment in any           │
  │                                      │ generation, writes evaporate                                      │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ rm-path <store-root> <basename>...   │ delete generations consistently (disk + db via GC; refuses paths  │
  │                                      │ still referenced). Never rm -rf inside a store                    │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ bootstrap.sh <store-root>            │ fresh start: Arch bootstrap tarball → keyring init → import as    │
  │                                      │ arch-base (the base everything else builds on)                    │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ iso/mkiso.sh <store-root> <base>     │ bootable ISO: squashed store + GRUB entry per generation,         │
  │                                      │ nixgen initcpio hook overlays chosen generation as / (tmpfs up)   │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ iso/mkstoredisk.sh [img] [size]      │ blank ext4 disk (label NIXSTORE); attach it and nixgen-commit     │
  │                                      │ inside the box persists generations onto it                       │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ iso/mkbootdisk.sh <store-root>       │ standalone bootable disk image (UEFI, no ISO): GRUB on ESP +      │
  │ [img] [size-MiB]                     │ seeded store partition; size to the target disk (sparse), dd it   │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ iso/boot-test.sh                     │ headless QEMU smoke-boot of build/nixarch.iso, PASS on autologin  │
  │                                      │ (attaches build/nixstore.img when present)                        │
  ├──────────────────────────────────────┼───────────────────────────────────────────────────────────────────┤
  │ iso/update-test.sh                   │ nixgen-update e2e: kernel upgrade in the box, then boot the result│
  │                                      │ from the store disk alone (no ISO), PASS on marker + package      │
  └──────────────────────────────────────┴───────────────────────────────────────────────────────────────────┘
```

Boot flow: initramfs takes the generation named by `nixgen=` from the store
disk (label NIXSTORE) when it holds it, else from `nixstore.squashfs` on the
ISO; overlay-mounts it with a tmpfs upper and switch_roots into the merge.
Store visible at `/nixstore`; with a disk attached its root is at
`/nixstoredev` and `nixgen-commit <name>` (inside the box) imports the running
root as a new generation there (initializing a store on a blank disk) and
appends its GRUB entry on the disk. Rollback = pick an older GRUB entry.
`nixgen-update <name> [cmd]` (also inside the box) goes the other way:
builds the *next* generation offline (overlay on the booted generation,
cmd = `pacman -Syu` by default in a chroot, import + GRUB entry) while
the live root stays untouched, so the running kernel keeps its full
module tree and the pacman live-update mismatch cannot happen.
`nixgen-remove <store-path-basename>...` deletes committed generations
from the store disk (disk + db together via the GC codepath, refusing
the running generation and anything still referenced) and prunes their
GRUB entries from entries.cfg.

Store import canonicalises permissions (dirs 0555, files 0444/0555,
root-owned, no xattrs: NAR keeps only the executable bit). What that
strips is captured per generation by `nixgen-savemeta` into
`/etc/nixgen/{perms,caps}` (directory modes, setuid/sticky bits,
ownership, capabilities) and replayed by `nixgen-restmeta`: at boot
via `nixgen-perms.service` (metadata-only copy-up into the tmpfs
upper) and inside every build sandbox right after the overlay mount,
so pacman sees the modes it expects and stays warning-free. Plain
644-vs-444 file modes stay canonical on purpose (root bypasses them;
restoring would copy-up every file). The ISO kernel is pinned ~30 days
back (Arch Linux Archive) so `iso/update-test.sh` exercises a real
version-to-version kernel upgrade.
Uncommitted writes live in RAM and vanish. Networking is baked in:
systemd-networkd DHCP on `en*`, DNS via systemd-resolved.

From zero: `./build.sh` at the repo root first (libs into `build/prefix`;
the C++ tools then compile on demand). Host needs `g++`, `curl`, `grub` +
`xorriso` + `mtools` (grub-mkrescue), `e2fsprogs`, `qemu` for testing. No
root, no squashfs-tools (mksquashfs runs from inside the generation).

```
pacman -S --needed grub xorriso mtools e2fsprogs qemu-base
```

```
  examples/bootstrap.sh build/archstore          # fresh base (once)
  examples/iso/mkiso.sh build/archstore <base>   # ISO (<base> = path bootstrap printed)
  examples/iso/mkstoredisk.sh                    # blank persistence disk
  qemu-system-x86_64 -accel kvm -m 2G -boot d -cdrom build/nixarch.iso \
      -drive file=build/nixstore.img,format=raw,if=virtio \
      -nic user,model=virtio-net-pci
  # inside: break things freely; happy? nixgen-commit my-setup; reboot, it's in the menu
```

Reruns of mkiso.sh reuse the nixarch generations and only reassemble the
ISO; `REBUILD=1 examples/iso/mkiso.sh ...` discards and rebuilds them
(needed after changing setup-boot.sh or the initcpio hook). After any ISO
rebuild, restart QEMU: a live VM's GRUB menu points at pre-rebuild hashes.

Real hardware (UEFI): build the image at the target disk's size, then flash.
The image is sparse and `conv=sparse` skips the empty space, so a full-disk
image flashes in minutes regardless of disk size.

```
  examples/iso/mkbootdisk.sh build/archstore build/nixarch-disk.img \
      $(( $(lsblk -b -dn -o SIZE /dev/sdX) / 1048576 ))
  sudo dd if=build/nixarch-disk.img of=/dev/sdX bs=4M conv=sparse \
      oflag=direct status=progress && sync
```

`sdX` = the target disk; everything on it is lost. Old bytes in the skipped
(sparse) regions stay physically present. Harmless (ext4 never reads
unallocated blocks), but drop `conv=sparse` for a full overwrite.

