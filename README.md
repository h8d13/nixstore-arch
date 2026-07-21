# archinix

## [Nixstore](src/)

Local-store-only extraction of the Nix store layer (libnixutil,
libnixstore) from [NixOS/nix](https://github.com/NixOS/nix)
2.36.0 (`40f375fa`), buildable on any Linux without Nix: `./build.sh`.

> [!NOTE]
> Remote stores (s3/http/ssh/daemon) and the .drv realisation machinery
> are cut: stores hold imported trees only; and the shell glue for retrieval/integration with userland.

Build depends on: `meson`, `ninja`, C++23 compiler:

Arch package names (`boost` is headers only; the compiled
`context`/`coroutine`/`iostreams` libs live in `boost-libs`):

```
pacman -S --needed meson ninja gcc pkgconf boost boost-libs openssl \
	libblake3 nlohmann-json sqlite
```

API reference:

C++ headers install to `include/nix/{util,store}/` ([internal API docs](https://hydra.nixos.org/job/nix/master/internal-api-docs/latest/download-by-type/doc/internal-api-docs)).

---

## [Arch Linux generations](arch/)

Immutable Arch generations on the store: updates build the next
generation offline, rollback is booting an older GRUB entry.

### From releases: [ISO](https://github.com/h8d13/archinix/releases)

**GRUB only**, since it has to read the store to load a generation's
kernel. Store filesystem is ext4 by default, with btrfs, xfs and f2fs
in the table ([`arch/nixgen/nixgen-fs`](arch/nixgen/nixgen-fs)):
`nixgen-setup /dev/disk --fs btrfs`, or a fourth argument to the image
builders.

`nixgen-setup /dev/disk` installs current running generation to a hard disk.
> [!NOTE]
> The ISO needs a disk for persistence. Otherwise vanishes on reboot.

Then, in the box: `nixgen-{commit,update,switch,remove,listid,diffid,setup}`;

`nixgen-help` is the full reference.

### From source:

```
./build.sh                                       # store libs into build/prefix
arch/bootstrap.sh build/archstore                # base generation (prints <base>)
arch/iso/mkiso.sh build/archstore <base>         # bootable ISO
arch/uefi-vm.sh                                  # QEMU (UEFI): ISO + one blank disk
#   in the box: nixgen-setup /dev/vda my-install [--fs btrfs]
arch/uefi-vm.sh boot                             # boot what you installed
```

Examples of post scripts: https://github.com/h8d13/nixarch.cfg
