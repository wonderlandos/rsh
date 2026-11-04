#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Doctrine check: the names of upstream projects stay out of rsh's own text (source, tests, manual
# page, documents). Build files (Makefile, build_config.rb, packaging.PKGBUILD) name the embedded
# interpreter's source on purpose, and so do its #include lines. With a built program the size limit
# (3 MB) is checked too.  Usage: sh doctrine.sh [path/to/rsh]
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
	if [ "$size" -ge 3145728 ]; then
		echo "FAIL size: $bin is $size bytes, limit 3145728"
		fail=1
	fi
fi
[ "$fail" = 0 ] && echo "rsh doctrine: ok"
exit "$fail"
