# arch/: Arch Linux generations on the nix store layer

Root filesystem = an immutable, content-addressed generation in a nix
store. GRUB picks the generation, the initramfs overlay-mounts it with a
tmpfs upper and switch_roots in. Rollback = boot an older entry.

Why: pacman live updates replace `/usr/lib/modules` under the running
kernel (module tree effectively unloaded until reboot). Here an update
builds the *next* generation offline; the running root is never touched.

It also aims to solve partial updates in a way, since the current gen;
only lives in RAM and `checkupdates` equivalent is ran before `nixgen-update`.

![SchemaArchinix](./schema.png)

This was generally the idea (like having `git` through changes you make on system).

Yet for the current to vanish when nothing to `-commit`:

<img width="1524" height="797" alt="Screenshot_20260720_174338" src="https://github.com/user-attachments/assets/d43ebffa-8a86-436b-8169-8b5c5c0d40c5" />

> My goal was for trying new stuff inside a box to be "forgiving".
> But also be more deliberate about what you do carry into a system.

Inside the box (installed by `setup-boot.sh`):
All commands: reference is `nixgen-help` (source:
[nixgen/nixgen-help](nixgen-help), drift-checked by
update-test: every installed [nixgen-*](./nixgen/) must appear in it).

## Resources

- https://wiki.archlinux.org/title/File_permissions_and_attributes

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

### Boot flow

initramfs is systemd-init (`HOOKS=(base systemd keyboard
block nixgen)`). `nixgen-store.service` takes the generation named by
`nixgen=` from the store disk (label NIXSTORE) when it holds it, else
from `nixstore.squashfs` on the ISO, and stages the tmpfs upper; a
generator writes the `sysroot.mount` that overlays them, so the root
filesystem is a unit like any other; `nixgen-bind.service` exposes the
store at `/nixstore` and the store-disk root at `/nixstoredev` before
switch-root.

Networking is baked in (networkd DHCP on `en*`, resolved DNS).

Autologin only while root is passwordless (stock state,
what the headless tests ride on). `passwd root` restores login prompts,
but the password lives in the tmpfs upper like any write: commit it,
or the next boot is passwordless autologin again.

## Real hardware (UEFI only)

### From releases: [ISO](https://github.com/h8d13/archinix/releases)

Write the ISO to a stick (`dd if=build/nixarch.iso of=/dev/sdX bs=4M
oflag=direct status=progress`), boot it on the target, shape the system,
then install what you are running:

```
nixgen-setup /dev/nvme0n1 my-install --fs btrfs
```

GPT (ESP + NIXSTORE), a standalone GRUB whose whole job is sourcing
`entries.cfg` from the store partition, and a first generation committed
from the running root. It refuses to run until the device path is typed
back; everything on the target is lost. `--fs` picks the store
filesystem; run it with no arguments to list them.

## VM-testing

Dry-run the whole thing first: `arch/uefi-vm.sh` (argless) boots the ISO
with a single blank disk at `/dev/vda`, exactly the shape of the real
thing, and `arch/uefi-vm.sh boot` starts the result alone afterwards.

The store disk is never attached on that path: an installed target has
its own NIXSTORE partition, and two of them makes GRUB's label search
ambiguous.
