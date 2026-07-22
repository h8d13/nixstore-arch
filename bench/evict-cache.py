#!/usr/bin/env python3
# Evict a tree's data pages from the page cache (posix_fadvise
# DONTNEED), no root needed. Approximates a cold commit: the box's
# real nixgen-commit runs hours after boot with the source root and
# store partially evicted, while every bench default is cache-hot.
# Partial on purpose: dentries and inodes stay cached (dropping those
# needs root), so true after-boot cold is worse than this measures.
# usage: evict-cache.py <dir>...
import os
import sys

n = b = 0
for d in sys.argv[1:]:
	for root, dirs, files in os.walk(d):
		for f in files:
			p = os.path.join(root, f)
			try:
				fd = os.open(p, os.O_RDONLY)
			except OSError:
				continue
			try:
				sz = os.fstat(fd).st_size
				os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
				n += 1
				b += sz
			finally:
				os.close(fd)
print(f"evicted {n} files, {b / 1e6:.0f} MB", file=sys.stderr)
