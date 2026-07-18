# archinix

## [Nixstore](src/)

Nix store layer (libnixutil, libnixstore + C API) extracted from [NixOS/nix](https://github.com/NixOS/nix) 2.36.0 (`40f375fa`), buildable on any Linux without Nix: `./build.sh`.

Build depends on: `meson`, `ninja`, C++23 compiler:

Arch package names (`boost` is headers only; the compiled
`context`/`coroutine`/`iostreams`/`url` libs live in `boost-libs`):

```
pacman -S --needed meson ninja gcc pkgconf boost boost-libs openssl libsodium \
	libarchive brotli zstd libblake3 nlohmann-json sqlite curl libseccomp
```

API reference:

C++ headers install to `include/nix/{util,store}/` ([internal API docs](https://hydra.nixos.org/job/nix/master/internal-api-docs/latest/download-by-type/doc/internal-api-docs))

C API in `nix_api_store.h` ([external API docs](https://hydra.nixos.org/job/nix/master/external-api-docs/latest/download-by-type/doc/external-api-docs)).

---

## [Arch Linux generations](arch/)

Immutable Arch generations on the store: updates build the next
generation offline, rollback is booting an older GRUB entry.

### From releases: [ISO](https://github.com/h8d13/archinix/releases)

Currently GRUB/Ext4 only.

`nixgen-setup /dev/disk` installs current running generation to a hard disk.

In the box: `nixgen-{commit,update,switch,remove,listid,diffid,setup}`;

`nixgen-help` is the full reference.

### From source:

```
./build.sh                                       # store libs into build/prefix
arch/bootstrap.sh build/archstore                # base generation (prints <base>)
arch/iso/mkiso.sh build/archstore <base>         # bootable ISO
arch/uefi-vm.sh iso                              # try it in QEMU (UEFI)
arch/iso/flashdisk.sh build/archstore /dev/sdX   # flash to hardware
```

Examples of post scripts: https://github.com/h8d13/nixarch.cfg
