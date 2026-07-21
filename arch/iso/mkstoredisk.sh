#!/bin/sh -e
# Blank writable store disk for the ISO: empty ext4 image labeled
# NIXSTORE. Attach it and nixgen-commit inside the box initializes a
# store on it at first commit; committed generations then boot from it.
# Sparse file: real usage grows with commits.
# usage: mkstoredisk.sh [img] [size] [fs]   (fs default ext4)
cd "$(dirname "$0")/../.."
. arch/nixgen/nixgen-fs

IMG=${1:-build/nixstore.img} SIZE=${2:-8G} FS=${3:-ext4}
is_supported_fs "$FS" || exit 1
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"
fs_mkfs "$FS" "$IMG" NIXSTORE
ls -lhs "$IMG"
