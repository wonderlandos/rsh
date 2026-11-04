#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The bench texts, the same bytes on every machine (a fixed linear congruential generator, exact in a double,
# no random source): the shapes the QA round measured on (/tmp/qa100-rsh/w/big1m.txt).
#   big1m.txt     one line, 200,000 words from {foo bar baz qq ab cd x1}, about 686 KB
#   big100k.txt   one line, 20,000 words, about 68 KB
#   lines100k.txt 100,000 lines "line N word word word", about 2.3 MB
# Usage: sh bench/gen.sh DIR
set -eu
dir=$1
mkdir -p "$dir"
words() { # count -> one line of words
	awk -v n="$1" 'BEGIN {
		split("foo bar baz qq ab cd x1", w, " "); x = 12345
		for (i = 1; i <= n; i++) {
			x = (x * 69069 + 1) % 4294967296
			printf "%s%s", w[int(x / 65536) % 7 + 1], (i < n ? " " : "\n")
		}
	}'
}
words 200000 >"$dir/big1m.txt"
words 20000 >"$dir/big100k.txt"
awk 'BEGIN {
	split("foo bar baz qq ab cd x1", w, " "); x = 777
	for (i = 1; i <= 100000; i++) {
		line = "line " i
		for (k = 0; k < 3; k++) { x = (x * 69069 + 1) % 4294967296; line = line " " w[int(x / 65536) % 7 + 1] }
		print line
	}
}' >"$dir/lines100k.txt"
