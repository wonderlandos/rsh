#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Doctrine check: the names of upstream projects stay out of rsh's own text (source, tests, manual
# page, documents). Build files (Makefile, build_config.rb, packaging.PKGBUILD) name the embedded
# interpreter's source on purpose, and so do its #include lines. With a built program the size limit
# is checked too (3.4 MiB, see below).  Usage: sh doctrine.sh [path/to/rsh]
set -u
here=$(cd "$(dirname "$0")" && pwd)
top=$here/..
fail=0
hits=$(cd "$top" && grep -rniwE 'mruby|matz|cruby|yukihiro' src tests docs examples rsh.1 README.md CHANGELOG.md .github 2>/dev/null |
	grep -v '^tests/doctrine.sh:' | grep -v '#include <mruby' || true)
if [ -n "$hits" ]; then
	echo "FAIL doctrine: an upstream name in rsh's own text (say \"the interpreter\"):"
	echo "$hits" | head -20
	fail=1
fi
bin=${1:-}
if [ -n "$bin" ] && [ -f "$bin" ]; then
	size=$(wc -c <"$bin")
	# The limit is for the release toolchain (Arch gcc, static, stripped, -Os), which makes a larger
	# program than Ubuntu's: rsh 0.9.1 is 3,011,400 bytes with Ubuntu gcc and 3,284,720 with Arch's
	# (0.9.0 on the AWS box: 2,895,976). 3.4 MiB (3,565,158 bytes) leaves room for a release or two
	# on either; a jump past it is a real growth to look at (docs/rsh.md, "Size gate").
	limit=3565158
	if [ "$size" -ge "$limit" ]; then
		echo "FAIL size: $bin is $size bytes, limit $limit"
		fail=1
	fi
fi
[ "$fail" = 0 ] && echo "rsh doctrine: ok"
exit "$fail"
