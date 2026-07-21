#!/bin/sh -e
# In-place nixgen tool refresh on a running box: git pull, run this.
# Lands in the overlay upper (RAM): nixgen-commit to keep.
cd "$(dirname "$0")"
# nixgen-fs is a sourced table, not a command: it belongs in lib, and a
# stale copy in bin would show up as an undocumented command
rm -f /usr/local/bin/nixgen-fs
install -Dm644 nixgen/nixgen-fs /usr/local/lib/nixgen-fs
for t in nixgen/nixgen-*; do
	[ "$t" = nixgen/nixgen-fs ] || install -m755 "$t" /usr/local/bin/
done
