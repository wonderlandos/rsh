#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Sh.rm_rf never follows a link that replaces a folder while the walk runs (audit 2 M9, rsh 1.0).
# With strace (skipped without it) every stat of the walk is held for 300 ms while a second process
# swaps t/d1 between a real folder and a link to keep/ every 100 ms; the walk is repeated 10 times.
# 0.9.0 (path based: lstat, then opendir, then unlink by path) lost keep/f in about one run of
# three; the walk from file descriptors (openat O_NOFOLLOW, unlinkat) cannot. The first check is
# deterministic and fails on 0.9.0 every time: the system calls themselves, no path-based rmdir or
# unlink, and O_NOFOLLOW on every folder opened below the start.
# Usage: sh tests/race.sh path/to/rsh
set -u
rsh=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
if ! command -v strace >/dev/null 2>&1 || ! strace -qq -o /dev/null true 2>/dev/null; then
	echo "race: strace missing or not allowed, skipped"
	exit 0
fi
w=$(mktemp -d)
trap 'rm -f "$w/go"; rm -rf "$w"' EXIT INT TERM
fail=0
cd "$w" || exit 99

# 1. the system calls
mkdir -p tree/a/b && : >tree/a/b/f && ln -s ../a tree/a/up
strace -f -qq -o "$w/calls" -e trace=openat,unlinkat,unlink,rmdir "$rsh" -c 'Sh.rm_rf("tree")' >/dev/null 2>&1
if [ -e tree ] || grep -Eq '(^|[0-9] )(rmdir|unlink)\(' "$w/calls" ||
	! grep -E 'openat\(.*"b".*O_NOFOLLOW' "$w/calls" | grep -q O_DIRECTORY ||
	! grep -q 'unlinkat(.*AT_REMOVEDIR' "$w/calls"; then
	echo "FAIL race: rm_rf did not walk with openat(O_NOFOLLOW) and unlinkat"
	grep -E 'openat\(|unlink|rmdir' "$w/calls" | tail -8
	fail=1
fi

# 2. the swap
mkdir keep && echo precious >keep/f && : >go
(
	while [ -e "$w/go" ]; do
		rmdir t/d1 2>/dev/null
		ln -s ../keep t/d1 2>/dev/null
		sleep 0.1
		rm -f t/d1 2>/dev/null
		mkdir t/d1 2>/dev/null
		sleep 0.1
	done
) &
cat >loop.rsh <<'X'
10.times do
  begin
    Dir.mkdir("t") rescue nil
    Dir.mkdir("t/d1") rescue nil
    Sh.rm_rf("t")
  rescue SystemCallError, StandardError
  end
  break unless File.exist?("keep/f")
end
X
strace -f -qq -o /dev/null -e trace=newfstatat -e inject=newfstatat:delay_exit=300ms "$rsh" loop.rsh >/dev/null 2>&1
rm -f go
wait
if [ ! -f keep/f ]; then
	echo "FAIL race: Sh.rm_rf followed a link that replaced a folder and removed keep/f"
	fail=1
fi
if [ $fail = 0 ]; then echo "race: 2 checks, 0 failed"; else exit 1; fi
