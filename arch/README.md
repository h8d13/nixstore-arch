# arch/: Arch Linux generations on the nix store layer

Root filesystem = an immutable, content-addressed generation in a nix
store. GRUB picks the generation, the initramfs overlay-mounts it with a
tmpfs upper and switch_roots in. Rollback = boot an older entry.

Why: pacman live updates replace `/usr/lib/modules` under the running
kernel (module tree effectively unloaded until reboot). Here an update
builds the *next* generation offline; the running root is never touched.

It also aims to solve partial updates in a way, since the current gen;
only lives in RAM and `checkupdates` equiv  is ran before `nixgen-update`

![SchemaArchinix](./schema.png)

## Tools

| script | role |
|---|---|
| `bootstrap.sh <store-root>` | Arch bootstrap tarball → keyring → import as `arch-base` (prints the base path) |
| `import-dir <store-root> <name> <dir>` | commit any tree as a generation + dedup |
| `generation.sh <store-root> <base> <name> [cmd]` | mutate base in an overlay sandbox, import result as new generation |
| `enter.sh <base> [cmd]` | same sandbox, throwaway: writes evaporate |
| `rm-path <store-root> <basename>...` | delete generations, disk + db via GC; refuses referenced paths, prunes orphaned `.links`. Never `rm -rf` inside a store |
| `export-path <store-root> <basename>... > bundle` | stream generations in `nix-store --export` wire format; re-hashes against the db so local corruption cannot spread |
| `import-path <store-root> < bundle` | receive a bundle on another machine + dedup; recomputes the CA store path from the received bytes, refuses mismatches |
| `iso/mkiso.sh <store-root> <base>` | bootable ISO: squashed store + GRUB entry per generation |
| `iso/mkstoredisk.sh [img] [size]` | blank ext4 disk (label NIXSTORE); attached, it persists committed generations |
| `iso/mkbootdisk.sh <store-root> [img] [MiB]` | standalone bootable disk image (UEFI, no ISO): GRUB ESP + seeded store partition |
| `iso/flashdisk.sh <store-root> <device>` | one-shot flash: sizes image to the disk, builds, writes, fscks both partitions |
| `uefi-vm.sh [disk\|iso\|clean]` | interactive QEMU on the real UEFI path (OVMF pflash, persistent NVRAM): flashable disk image or ISO |
| `updev.sh` | in-place box update of `nixgen-*` scripts. Usually when changes are small enough |

## Tests

| script | role |
|---|---|
| `tests/boot-test.sh` | headless QEMU smoke-boot of the ISO, PASS on autologin |
| `tests/update-test.sh` | e2e: kernel upgrade in the box, boot the result from the store disk alone |
| `tests/meta-test.sh` | host-only: user created in the sandbox survives manifest + restmeta replay |


Inside the box (installed by `setup-boot.sh`):
All commands: reference is `nixgen-help` (source:
[nixgen/nixgen-help](nixgen/nixgen-help), drift-checked by
update-test: every installed nixgen-* must appear in it).

## From nothing

Host deps: build deps from the root README, plus
`pacman -S --needed grub xorriso mtools e2fsprogs (qemu-base edk2-ovmf)`.
No root needed anywhere; no squashfs-tools (mksquashfs runs from
inside the generation). QEMU optional.

```
./build.sh                                     # libs into build/prefix
arch/bootstrap.sh build/archstore              # once; prints <base>
arch/iso/mkiso.sh build/archstore <base>       # bootable ISO
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
what the headless tests ride on). `passwd root` restores login prompts,
but the password lives in the tmpfs upper like any write: commit it,
or the next boot is passwordless autologin again.

## Real hardware (UEFI only, Secure Boot off)

```
arch/iso/flashdisk.sh build/archstore /dev/sdX
```

Or install from a running box: boot any nixarch medium on the target
machine, shape the system, then `nixgen-setup /dev/nvmeXn1`: you
install what you actually run, not a pre-built image.

One command: sizes the image to the disk, builds it, writes it, fscks
both partitions. It refuses to run until the device path is typed back;
everything on the disk is lost. ~6 minutes, most of it the silent
mkbootdisk assembly (mkfs + hole-scanning dd).

What it does under the hood, for doing it manually: GPT+ESP (first
66MiB) written in full: a sparse write would leave the previous flash's
FAT metadata under the holes. Store partition written `conv=sparse`:
its metadata is all real bytes in the image, and `fsck.ext4 -fn`
afterwards proves the result. Never plain `conv=sparse` for the whole
disk on a previously-used target.

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
  `nixgen-savemeta` captures what that strips (modes, ownership incl.
  symlinks, capabilities, POSIX ACLs) into
  `etc/nixgen/{perms,caps,acls}`; `nixgen-restmeta` replays it at boot
  (nixgen-perms.service) and inside every build sandbox. A base
  imported without the manifest breaks the chain: pacman warns on
  every dir and rejects its 0555 cachedir (downloads fall back to
  /tmp = more RAM). generation.sh warns when the base lacks it;
  re-bootstrap to fix. Plain 644-vs-444 file modes stay canonical on
  purpose (root bypasses them; restoring would copy-up every file),
  except /etc/skel: useradd copies its modes to new users, so it is
  captured whole (tests/meta-test.sh pins this).
- **Diskless BIOS boots pay ~10s** of GRUB probing for the absent
  NIXSTORE label. Known cost, attached-disk boots don't pay it.
- **Sparse flashing trusts skipped regions to read zero.** On a
  previously-used disk they don't: the ESP (mostly zeros) inherits the
  old flash's FAT metadata. flashdisk.sh writes the first 66MiB in
  full and verifies both filesystems; stale bytes in ext4 *data*
  blocks stay harmless (never read before written).
- **USB store disks enumerate late.** `udevadm settle` doesn't wait
  for undiscovered hardware, so disk-only boots on usb lost a ~5s
  race and fell into the ISO hunt (`wrong fs type` spam, no
  recovery). Disk-boot GRUB entries carry `nixsource=disk`, which
  makes the initramfs wait (up to 30s) for the store disk instead.
  Virtio enumerates instantly: a VM PASS does not cover this path.
- **update-test.sh pins a dated Arch Archive snapshot** to prove a real
  kernel version change; archive use lives in the test only, stock
  generations track live mirrors.
