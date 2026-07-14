#!/bin/sh -e
# Blank writable store disk for the ISO: empty ext4 image labeled
# NIXSTORE. Attach it and nixgen-commit inside the box initializes a
# store on it at first commit; committed generations then boot from it.
# Sparse file: real usage grows with commits.
# usage: mkstoredisk.sh [img] [size]
cd "$(dirname "$0")/../.."

IMG=${1:-build/nixstore.img} SIZE=${2:-8G}
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"
mkfs.ext4 -q -L NIXSTORE "$IMG"
ls -lhs "$IMG"
