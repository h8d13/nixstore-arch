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
  │ iso/boot-test.sh                     │ headless QEMU smoke-boot of build/nixarch.iso, PASS on autologin  │
  │                                      │ (attaches build/nixstore.img when present)                        │
  └──────────────────────────────────────┴───────────────────────────────────────────────────────────────────┘
```

Boot flow: initramfs takes the generation named by `nixgen=` from the store
disk (label NIXSTORE) when it holds it, else from `nixstore.squashfs` on the
ISO; overlay-mounts it with a tmpfs upper and switch_roots into the merge.
Store visible at `/nixstore`; with a disk attached its root is at
`/nixstoredev` and `nixgen-commit <name>` (inside the box) imports the running
root as a new generation there (initializing a store on a blank disk) and
appends its GRUB entry on the disk. Rollback = pick an older GRUB entry.
Uncommitted writes live in RAM and vanish. Networking is baked in:
systemd-networkd DHCP on `en*`, DNS via systemd-resolved.

```
  examples/bootstrap.sh build/archstore          # fresh base (once)
  examples/iso/mkiso.sh build/archstore <base>   # ISO
  examples/iso/mkstoredisk.sh                    # blank persistence disk
  qemu-system-x86_64 -accel kvm -m 2G -boot d -cdrom build/nixarch.iso \
      -drive file=build/nixstore.img,format=raw,if=virtio \
      -nic user,model=virtio-net-pci
  # inside: break things freely; happy? nixgen-commit my-setup; reboot, it's in the menu
```

