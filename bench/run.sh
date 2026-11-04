#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# rsh speed and memory on the shapes that were slow in 0.1.3 (QA round, findings H-1, M-10, M-5):
# Regexp loops on long texts, a pattern per line over 100,000 lines, a million small matches, and
# builtins in a loop. Each row is one `rsh -c` run: wall time, peak RSS, and its limit.
#   sh bench/run.sh path/to/rsh           print the table
#   sh bench/run.sh --gate path/to/rsh    the same, exit 1 when a row is over its limit, prints the
#                                         wrong answer, or runs past BENCH_TIMEOUT (default 20 s)
#   BENCH_SLACK=3 sh bench/run.sh --gate path/to/rsh   time limits x3 on a slow machine (see below)
# The texts come from bench/gen.sh (fixed bytes, checked by sha256 below). The limits are the
# 0.1.4 targets with room for a slower machine; 0.1.3 was 10 to 300 times over most of them.
set -u
gate=0
if [ "${1:-}" = "--gate" ]; then
	gate=1
	shift
fi
rsh=${1:?usage: sh bench/run.sh [--gate] path/to/rsh}
case $rsh in /*) ;; *) rsh=$PWD/$rsh ;; esac
here=$(cd "$(dirname "$0")" && pwd)
limit_time=${BENCH_TIMEOUT:-20}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
sh "$here/gen.sh" "$tmp" || exit 2
(cd "$tmp" && sha256sum -c --quiet) <<'SUMS' || { echo "bench: the generated texts differ (awk?)"; exit 2; }
a26fdad94a375346834bc6b5c03e2e49e8171d03cf21133743e562ea618298b5  big100k.txt
4fe5262655e077da1e03906aa4ca5151c467b91cc858c3f244ef83f3a541aafe  big1m.txt
0485fa5d668c35027d47a3a3be8662795603f624c8ffb38a989c25b20a1dfd4d  lines100k.txt
SUMS
# Time limits are for a quiet machine. BENCH_SLACK=N multiplies them (memory limits stay); on a
# machine whose 1-minute load average is above its CPU count the slack is at least 5, and says so.
slack=${BENCH_SLACK:-1}
if [ -r /proc/loadavg ] && command -v nproc >/dev/null; then
	if awk -v l="$(cut -d' ' -f1 /proc/loadavg)" -v c="$(nproc)" 'BEGIN { exit !(l > c) }'; then
		echo "bench: the machine is busy (load average over $(nproc) CPUs): time limits x5 unless BENCH_SLACK is larger"
		awk -v s="$slack" 'BEGIN { exit !(s < 5) }' && slack=5
	fi
fi
have_time=0
[ -x /usr/bin/time ] && have_time=1
bad=0
printf '%-26s %9s %9s %8s %8s  %s\n' row seconds limit_s rss_mb limit_mb result
row() { # name limit_seconds limit_mb expected code
	name=$1 lim=$(awk -v l="$2" -v s="$slack" 'BEGIN { printf "%g", l * s }') limmb=$3 want=$4 code=$5
	a=$(date +%s%N)
	if [ "$have_time" = 1 ]; then
		(cd "$tmp" && timeout "$limit_time" /usr/bin/time -f '%M' -o "$tmp/rss" "$rsh" -c "$code" >"$tmp/out" 2>"$tmp/err")
	else
		(cd "$tmp" && timeout "$limit_time" "$rsh" -c "$code" >"$tmp/out" 2>"$tmp/err")
	fi
	rc=$?
	b=$(date +%s%N)
	secs=$(awk -v d=$((b - a)) 'BEGIN { printf "%.3f", d / 1e9 }')
	mb=-
	[ "$have_time" = 1 ] && [ -s "$tmp/rss" ] && mb=$(awk '/^[0-9]+$/ { printf "%.1f", $1 / 1024 }' "$tmp/rss")
	got=$(tr '\n' ' ' <"$tmp/out" | sed 's/ $//')
	verdict=ok
	if [ "$rc" = 124 ]; then
		verdict="TIMEOUT (over ${limit_time} s)"
	elif [ "$rc" != 0 ] || [ "$got" != "$want" ]; then
		verdict="WRONG (rc $rc, got '$got', want '$want'; $(head -c 200 "$tmp/err"))"
	elif awk -v s="$secs" -v l="$lim" 'BEGIN { exit !(s > l) }'; then
		verdict=SLOW
	elif [ "$limmb" != - ] && [ "$mb" != - ] && awk -v m="$mb" -v l="$limmb" 'BEGIN { exit !(m > l) }'; then
		verdict=MEMORY
	fi
	[ "$verdict" = ok ] || bad=$((bad + 1))
	printf '%-26s %9s %9s %8s %8s  %s\n' "$name" "$secs" "$lim" "$mb" "$limmb" "$verdict"
}
# H-1: one UTF-8 check per subject; the loops are linear in the text.
row 'scan(/\w+/) 686 KB' 1.0 - 200000 's = File.read("big1m.txt"); puts s.scan(/\w+/).size'
row 'scan(/\w+/) 68 KB' 0.3 - 20000 's = File.read("big100k.txt"); puts s.scan(/\w+/).size'
row 'scan(/(\w)\w/) { } 686 KB' 2.0 - 200000 's = File.read("big1m.txt"); n = 0; s.scan(/(\w)\w/) { |m| n += 1 }; puts n'
row 'split(/ /) 686 KB' 1.0 - 200000 's = File.read("big1m.txt"); puts s.split(/ /).size'
row 'gsub(/foo/, "F") 686 KB' 0.5 - 628799 's = File.read("big1m.txt"); puts s.gsub(/foo/, "F").size'
row 'gsub(/(f)oo/, "\\1") 686 KB' 0.5 - 628799 's = File.read("big1m.txt"); puts s.gsub(/(f)oo/, "\\1").size'
row 'gsub(/foo/) { } 686 KB' 1.0 - 628799 's = File.read("big1m.txt"); puts s.gsub(/foo/) { "F" }.size'
row 'invalid UTF-8 gsub 68 KB' 1.0 - 20000 's = File.read("big100k.txt").gsub(/ /, "\xFF"); puts s.scan(/\w+/).size'
row 'scan 686 KB + 1 bad byte' 1.0 - 200000 's = File.read("big1m.txt") + "\xFF"; puts s.scan(/\w+/).size'
# M-10: a pattern per line; memory stays flat.
row 'readlines =~ 100k lines' 1.0 30 37044 'n = 0; File.readlines("lines100k.txt").each { |l| n += 1 if l =~ /foo/ }; puts n'
row 'readlines match? 100k' 1.0 30 37044 'n = 0; File.readlines("lines100k.txt").each { |l| n += 1 if l.match?(/foo/) }; puts n'
row 'each_line sub(/re/) 100k' 1.0 30 100000 'n = 0; File.foreach("lines100k.txt") { |l| n += 1 if l.sub(/line/, "x") }; puts n'
row '1M x match?' 3.0 30 1000000 'i = 0; n = 0; r = /foo/; while i < 1000000; n += 1 if r.match?("a foo b"); i += 1; end; puts n'
row '1M x =~ with $1' 6.0 30 1000000 'i = 0; n = 0; while i < 1000000; n += 1 if "a foo b" =~ /(o+)/ && $1 == "oo"; i += 1; end; puts n'
# M-5: builtins without a fork.
row '1000 x echo' 0.1 - 1000 '1000.times { echo x >/dev/null }; puts 1000'
row 'rsh -c nil' 0.05 - '' 'nil'
if [ "$gate" = 1 ] && [ "$bad" -gt 0 ]; then
	echo "bench: $bad row(s) over the limit"
	exit 1
fi
exit 0
