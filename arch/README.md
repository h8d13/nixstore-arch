# arch/: Arch Linux generations on the nix store layer

Root filesystem = an immutable, content-addressed generation in a nix
store. GRUB picks the generation, the initramfs overlay-mounts it with a
tmpfs upper and switch_roots in. Rollback = boot an older entry.

Why: pacman live updates replace `/usr/lib/modules` under the running
kernel (module tree effectively unloaded until reboot). Here an update
builds the *next* generation offline; the running root is never touched.

## Tools

| script | role |
|---|---|
| `bootstrap.sh <store-root>` | Arch bootstrap tarball → keyring → import as `arch-base` (prints the base path) |
| `import-dir <store-root> <name> <dir>` | commit any tree as a generation + dedup |
| `generation.sh <store-root> <base> <name> [cmd]` | mutate base in an overlay sandbox, import result as new generation |
| `enter.sh <base> [cmd]` | same sandbox, throwaway: writes evaporate |
| `rm-path <store-root> <basename>...` | delete generations, disk + db via GC; refuses referenced paths. Never `rm -rf` inside a store |
| `iso/mkiso.sh <store-root> <base>` | bootable ISO: squashed store + GRUB entry per generation |
| `iso/mkstoredisk.sh [img] [size]` | blank ext4 disk (label NIXSTORE); attached, it persists committed generations |
| `iso/mkbootdisk.sh <store-root> [img] [MiB]` | standalone bootable disk image (UEFI, no ISO): GRUB ESP + seeded store partition |
| `iso/boot-test.sh` | headless QEMU smoke-boot of the ISO, PASS on autologin |
| `iso/update-test.sh` | e2e: kernel upgrade in the box, boot the result from the store disk alone |

Inside the box (installed by setup-boot.sh):

- `nixgen-commit <name>`: import the running root onto the store disk
  (initializes a blank one) + GRUB entry. Reboot, it's in the menu.
- `nixgen-update <name> [cmd]`: build the next generation offline
  (default `pacman -Syu`) with its upper on the store disk; the live
  root and its module tree stay intact.
- `nixgen-remove <basename>...`: delete committed generations (GC
  codepath, refuses the running one) + prune their GRUB entries.

## From nothing

Host deps: build deps from the root README, plus
`pacman -S --needed grub xorriso mtools e2fsprogs qemu-base`.
No root needed anywhere; no squashfs-tools (mksquashfs runs from
inside the generation).

```
./build.sh                                     # libs into build/prefix
arch/bootstrap.sh build/archstore              # once; prints <base>
arch/iso/mkiso.sh build/archstore <base>       # ISO with 2 generations
arch/iso/mkstoredisk.sh                        # blank persistence disk
qemu-system-x86_64 -accel kvm -m 2G -boot d -cdrom build/nixarch.iso \
    -drive file=build/nixstore.img,format=raw,if=virtio \
    -nic user,model=virtio-net-pci
# inside: break things freely; happy? nixgen-commit my-setup
```

Boot flow: the initramfs takes the generation named by `nixgen=` from
the store disk (label NIXSTORE) when it holds it, else from
`nixstore.squashfs` on the ISO; store visible at `/nixstore`, store-disk
root at `/nixstoredev`. Networking is baked in (networkd DHCP on `en*`,
resolved DNS). Autologin only while root is passwordless (stock state,
what the headless tests ride on); `passwd root` restores login prompts,
commit to keep that.

## Real hardware (UEFI only, Secure Boot off)

Size the image to the target disk, flash, done. Sparse: real bytes =
store size, `conv=sparse` skips the rest, so a full-disk image flashes
in minutes.

```
arch/iso/mkbootdisk.sh build/archstore build/nixarch-disk.img \
    $(( $(lsblk -b -dn -o SIZE /dev/sdX) / 1048576 ))
sudo dd if=build/nixarch-disk.img of=/dev/sdX bs=4M conv=sparse \
    oflag=direct status=progress && sync
```

`sdX` = the whole target disk, not a partition; everything on it is
lost. The size arithmetic is bash; from fish, run the mkbootdisk line
via `sh -c '...'`.

## Gotchas

- **Reruns of mkiso.sh reuse the nixarch generations** and only
  reassemble the ISO. `REBUILD=1` discards them first; required after
  changing setup-boot.sh or the initcpio hook. After any ISO rebuild,
  restart QEMU: a live VM's GRUB menu points at pre-rebuild hashes.
- **Uncommitted writes live in RAM and vanish**: overlay upper is a
  tmpfs (75% of RAM), no swap. A big enough pacman transaction (~1 GiB
  of downloads+extract) dies with `Write failed` in a 2G box. Big
  installs: `nixgen-update` (upper on the store disk), more `-m`, or
  commit + reboot between chunks (the upper resets).
- **Import canonicalises permissions** (dirs 0555, files 0444/0555,
  root-owned, no xattrs: NAR keeps only the executable bit).
  `nixgen-savemeta` captures what that strips into
  `etc/nixgen/{perms,caps}`; `nixgen-restmeta` replays it at boot
  (nixgen-perms.service) and inside every build sandbox. A base
  imported without the manifest breaks the chain: pacman warns on
  every dir and rejects its 0555 cachedir (downloads fall back to
  /tmp = more RAM). generation.sh warns when the base lacks it;
  re-bootstrap to fix. Plain 644-vs-444 file modes stay canonical on
  purpose (root bypasses them; restoring would copy-up every file).
- **Diskless BIOS boots pay ~10s** of GRUB probing for the absent
  NIXSTORE label. Known cost, attached-disk boots don't pay it.
- **`conv=sparse` leaves old bytes** physically present in the skipped
  regions of the target disk. Harmless (ext4 never reads unallocated
  blocks); drop it for a full overwrite.
- **update-test.sh pins a dated Arch Archive snapshot** to prove a real
  kernel version change; archive use lives in the test only, stock
  generations track live mirrors.
