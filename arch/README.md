# arch/: Arch Linux generations on the nix store layer

Root filesystem = an immutable, content-addressed generation in a nix
store. GRUB picks the generation, the initramfs overlay-mounts it with a
tmpfs upper and switch_roots in. Rollback = boot an older entry.

Why: pacman live updates replace `/usr/lib/modules` under the running
kernel (module tree effectively unloaded until reboot). Here an update
builds the *next* generation offline; the running root is never touched.

It also aims to solve partial updates in a way, since the current gen;
only lives in RAM and `checkupdates` equivalent is ran before `nixgen-update`

![SchemaArchinix](./schema.png)

This was generally the idea (like having `git` through changes you make on system).

Yet for the current to vanish when nothing to `-commit`:

<img width="1524" height="797" alt="Screenshot_20260720_174338" src="https://github.com/user-attachments/assets/d43ebffa-8a86-436b-8169-8b5c5c0d40c5" />

Inside the box (installed by `setup-boot.sh`):
All commands: reference is `nixgen-help` (source:
[nixgen/nixgen-help](nixgen/nixgen-help), drift-checked by
update-test: every installed nixgen-* must appear in it).

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
| `iso/mkstoredisk.sh [img] [size] [fs]` | blank store disk (label NIXSTORE); attached, it persists committed generations |
| `uefi-vm.sh [fresh\|keep\|boot\|iso\|clean]` | interactive QEMU on the real UEFI path (OVMF pflash, persistent NVRAM). Argless = the install workflow: ISO + one blank disk, `nixgen-setup /dev/vda`, then `boot` runs the result. `iso` swaps the blank disk for the NIXSTORE store disk (commit/update playground) |
| `devup.sh` | in-place box update of `nixgen-*` scripts. Usually when changes are small enough |

## Tests

| script | role |
|---|---|
| `tests/boot-test.sh` | headless QEMU smoke-boot of the ISO, PASS on autologin |
| `tests/update-test.sh` | e2e: kernel upgrade in the box, boot the result from the store disk alone |
| `tests/meta-test.sh` | host-only: user created in the sandbox survives manifest + restmeta replay |
| `tests/fs-test.sh` | host-only: every filesystem in the table formats, labels and passes its own read-only check |
| `tests/diskless-test.sh` | ISO with no store disk: boots promptly (nothing waits for absent hardware) and refuses commit/update/remove |
| `tests/isohunt-test.sh` | ISO hunt with the by-label shortcut broken: finds the medium past a decoy disk, without probe-by-mounting noise |

## From nothing

Host deps: build deps from the root README, plus
`pacman -S --needed grub xorriso mtools e2fsprogs (qemu-base edk2-ovmf)`,
and the progs of whatever store filesystem you pick (`btrfs-progs`,
`xfsprogs`, `f2fs-tools`): a builtin kernel driver is not a mkfs.
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

Boot flow: the initramfs is systemd-init (`HOOKS=(base systemd keyboard
block nixgen)`). `nixgen-store.service` takes the generation named by
`nixgen=` from the store disk (label NIXSTORE) when it holds it, else
from `nixstore.squashfs` on the ISO, and stages the tmpfs upper; a
generator writes the `sysroot.mount` that overlays them, so the root
filesystem is a unit like any other; `nixgen-bind.service` exposes the
store at `/nixstore` and the store-disk root at `/nixstoredev` before
switch-root. A failure anywhere in that chain lands in
`emergency.target`: a shell with `journalctl` behind it, not a bare
busybox prompt. Networking is baked in (networkd DHCP on `en*`,
resolved DNS). Autologin only while root is passwordless (stock state,
what the headless tests ride on). `passwd root` restores login prompts,
but the password lives in the tmpfs upper like any write: commit it,
or the next boot is passwordless autologin again.

## Real hardware (UEFI only, Secure Boot off)

Write the ISO to a stick (`dd if=build/nixarch.iso of=/dev/sdX bs=4M
oflag=direct status=progress`), boot it on the target, shape the system,
then install what you actually run:

```
nixgen-setup /dev/nvme0n1 my-install --fs btrfs
```

GPT (ESP + NIXSTORE), a standalone GRUB whose whole job is sourcing
`entries.cfg` from the store partition, and a first generation committed
from the running root. It refuses to run until the device path is typed
back; everything on the target is lost. `--fs` picks the store
filesystem; run it with no arguments to list them.

Dry-run the whole thing first: `arch/uefi-vm.sh` (argless) boots the ISO
with a single blank disk at `/dev/vda`, exactly the shape of the real
thing, and `arch/uefi-vm.sh boot` starts the result alone afterwards.
The store disk is never attached on that path: an installed target has
its own NIXSTORE partition, and two of them makes GRUB's label search
ambiguous.

## Gotchas

- **Reruns of mkiso.sh reuse the nixarch generations** and only
  reassemble the ISO. `REBUILD=1` discards them first; required after
  changing setup-boot.sh or any `arch/iso/initcpio-*` file. After any ISO
  rebuild, restart QEMU: a live VM's GRUB menu points at pre-rebuild
  hashes.
- **Boot entries carry `rd.systemd.gpt_auto=0`.** Root comes from
  `nixgen=`, so systemd-gpt-auto-generator must not go hunting for a
  root partition and race the generated `sysroot.mount`. Entries written
  by commit/update/adopt inherit it from `/proc/cmdline`; hand-written
  ones need it too.
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
- **USB store disks enumerate late.** `udevadm settle` doesn't wait
  for undiscovered hardware, so disk-only boots on usb lost a ~5s
  race and fell into the ISO hunt (`wrong fs type` spam, no
  recovery). Disk-boot GRUB entries carry `nixsource=disk`, which makes
  `nixgen-store.service` `udevadm wait` (up to 30s) for the store disk
  instead: it returns the moment udev has the device, and covers
  hardware that has not been discovered yet.
  Virtio enumerates instantly: a VM PASS does not cover this path.
- **update-test.sh pins a dated Arch Archive snapshot** to prove a real
  kernel version change; archive use lives in the test only, stock
  generations track live mirrors.
