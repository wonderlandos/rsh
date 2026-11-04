#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# rsh golden and example runner.
#   golden/NAME.rsh   (or NAME.rb, the same rule: plain Ruby only with `# rsh: ruby`) run in an empty directory; stdout plus a last line "status: N" must equal
#                     NAME.out; if NAME.explain exists, `rsh --explain NAME.rsh` must equal it
#   examples/NAME.rsh the same, with fakebin/ first on PATH (stand-ins for ssh, rsync, git ...),
#                     arguments from NAME.args when that file exists (written as for sh: quotes allowed)
#   ../examples/meta/NAME.rsh (the documented examples) the same, expected files in examples/meta/
#   golden/readme_NAME.rsh the examples of README.md, with fakebin/readme/ first on PATH (stand-ins
#                     for curl, ssh, nft, make, df, sleep); every ```ruby block of README.md must be
#                     found, line for line, in one of them, so the README shows what really runs
#   NAME.setup        (any of the above) sh script run first in the empty directory: the files the
#                     script works on
# Usage: RSH=path/to/rsh sh run.sh [--update] [name ...]
# An (even empty) NAME.explain file opts a script in to the classification check, a NAME.err
# file to the standard-error check.
# --update writes the expected files from the current output (look at the diff before you keep it).
set -u
here=$(cd "$(dirname "$0")" && pwd)
rsh=${RSH:-$here/../build/rsh}
case $rsh in /*) ;; *) rsh=$PWD/$rsh ;; esac
update=0
if [ "${1:-}" = "--update" ]; then
	update=1
	shift
fi
names=" $* "
have_names=0
[ $# -gt 0 ] && have_names=1
pass=0
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

# Same output on every machine: C locale, a fixed home, no colour, a private working directory.
run_one() { # dir name ext [script] -> writes $tmp/got.out
	dir=$1
	name=$2
	ext=$3
	script=${4:-$here/$dir/$name.$ext}
	work=$tmp/work
	rm -rf "$work"
	mkdir -p "$work/home"
	args=
	[ -f "$here/$dir/$name.args" ] && args=$(cat "$here/$dir/$name.args")
	eval "set -- $args"
	path=$PATH
	case $dir in examples*) path=$here/fakebin:$PATH ;; esac
	case $dir/$name in golden/readme_*) path=$here/fakebin/readme:$PATH ;; esac
	(
		cd "$work" || exit 99
		if [ -f "$here/$dir/$name.setup" ]; then
			env -i PATH="$PATH" LC_ALL=C sh "$here/$dir/$name.setup" || exit 99
		fi
		# The sanitizer settings of `make test` (Makefile ASAN_ENV) pass through; nothing else does.
		env -i PATH="$path" HOME="$work/home" LC_ALL=C TERM=dumb USER=tester RSH_TESTS="$here" \
			${ASAN_OPTIONS:+ASAN_OPTIONS="$ASAN_OPTIONS"} ${UBSAN_OPTIONS:+UBSAN_OPTIONS="$UBSAN_OPTIONS"} \
			"$rsh" "$script" "$@" 2>"$tmp/got.err" >"$tmp/got.body"
		echo "status: $?" >>"$tmp/got.body"
	)
	sed "s#$work#WORK#g" "$tmp/got.body" >"$tmp/got.out"
}

check() { # expected-file got-file label
	if [ "$update" = 1 ]; then
		cp "$2" "$1"
		echo "updated $3"
	elif cmp -s "$1" "$2"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $3"
		diff "$1" "$2" | head -20
	fi
}

for dir in golden examples; do
	for f in "$here/$dir"/*.rsh "$here/$dir"/*.rb; do
		[ -f "$f" ] || continue
		ext=${f##*.}
		name=$(basename "$f" ".$ext")
		if [ "$have_names" = 1 ]; then
			case "$names" in *" $name "*) ;; *) continue ;; esac
		fi
		run_one "$dir" "$name" "$ext"
		check "$here/$dir/$name.out" "$tmp/got.out" "$dir/$name"
		if [ -f "$here/$dir/$name.err" ]; then
			sed "s#$tmp/work#WORK#g; s#$here/##g" "$tmp/got.err" >"$tmp/got.err2"
			check "$here/$dir/$name.err" "$tmp/got.err2" "$dir/$name (stderr)"
		fi
		if [ -f "$here/$dir/$name.explain" ]; then
			"$rsh" --explain "$f" 2>&1 | sed "s#$here/##" >"$tmp/got.explain"
			check "$here/$dir/$name.explain" "$tmp/got.explain" "$dir/$name (explain)"
		fi
	done
done

for f in "$here/../examples/meta"/*.rsh; do
	[ -f "$f" ] || continue
	name=$(basename "$f" .rsh)
	if [ "$have_names" = 1 ]; then
		case "$names" in *" $name "*) ;; *) continue ;; esac
	fi
	run_one examples/meta "$name" rsh "$f"
	check "$here/examples/meta/$name.out" "$tmp/got.out" "examples/meta/$name"
done

# README.md: every ```ruby block appears, line for line, in a tests/golden/readme_*.rsh.
if [ "$have_names" = 0 ] && [ "$update" = 0 ]; then
	readme=$here/../README.md
	missing=$(awk -v dir="$here/golden" '
		function slurp(f,   l, s) { s = ""; while ((getline l < f) > 0) s = s l "\n"; close(f); return s }
		BEGIN {
			cmd = "ls " dir "/readme_*.rsh 2>/dev/null"
			while ((cmd | getline f) > 0) all = all slurp(f) "\f"
			close(cmd)
		}
		/^```ruby$/ { inb = 1; blk = ""; start = NR; next }
		inb && /^```$/ { inb = 0; if (index(all, blk) == 0) print "README.md:" start; next }
		inb { blk = blk $0 "\n" }
	' "$readme")
	if [ -z "$missing" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL readme: a \`\`\`ruby block with no golden/readme_*.rsh holding it (copy it in, with a .out):"
		echo "$missing"
	fi
fi

# Modes on the command line: -n and --ruby follow the same rule as running (shell layer on,
# `# rsh: ruby` first turns it off), for .rb files as for any other.
mkdir -p "$tmp/cli"
printf '# rsh: ruby\nx = 1\nputs x\n' >"$tmp/cli/plain.rb"
printf 'echo hi\n' >"$tmp/cli/shell.rb"
printf 'puts ARGV.join("|")\n' >"$tmp/cli/args.rb"
printf '# rsh: ruby\ncd ~/src/app\n' >"$tmp/cli/plain_cmd.rb"
printf 'cd ~/src/app\n' >"$tmp/cli/shell_cmd.rb"
cli() { # label ok-expression
	if eval "$2"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL cli: $1"; fi
}
cli "-n accepts plain Ruby" '"$rsh" -n "$tmp/cli/plain.rb" 2>/dev/null'
cli "-n reads a # rsh: ruby file as Ruby (a command line is a mistake there)" '! "$rsh" -n "$tmp/cli/plain_cmd.rb" >/dev/null 2>&1'
cli "-n reads the same line as a command without it" '"$rsh" -n "$tmp/cli/shell_cmd.rb" 2>/dev/null'
cli "-n accepts a .rb with command lines" '"$rsh" -n "$tmp/cli/shell.rb" 2>/dev/null'
cli "--ruby of a # rsh: ruby file is the text itself" '[ "$("$rsh" --ruby "$tmp/cli/plain.rb")" = "$(cat "$tmp/cli/plain.rb")" ]'
cli "--ruby of a .rb is transpiled" '[ "$("$rsh" --ruby "$tmp/cli/shell.rb")" != "$(cat "$tmp/cli/shell.rb")" ]'
cli "--explain names the mode (ruby)" '"$rsh" --explain "$tmp/cli/plain.rb" | grep -q "^mode: ruby"'
cli "--explain names the mode (shell)" '"$rsh" --explain "$tmp/cli/shell.rb" | grep -q "^mode: shell"'
cli "-e keeps the shell layer" '[ "$("$rsh" -e "echo viae")" = "viae" ]'
cli "-s --name x.rb has the shell layer" '[ "$(printf "echo hi\n" | "$rsh" -s --name x.rb)" = hi ]'
cli "-s with # rsh: ruby is plain Ruby" '! printf "# rsh: ruby\necho hi\n" | "$rsh" -s --name x.rb >/dev/null 2>&1'
cli "-s --name x.rsh has the shell layer" '[ "$(printf "echo hi\n" | "$rsh" -s --name x.rsh)" = hi ]'
cli "-s --name names the script in \$0" '[ "$(printf "puts \$0\n" | "$rsh" -s --name dir/x.rb)" = dir/x.rb ]'
cli "start-up does not load the Regexp side (it loads on first use)" '[ "$("$rsh" -c "p Rsh.__regexp")" = true ]'
cli "a pattern loads it" '[ "$("$rsh" -c "x = /a/; p Rsh.__regexp")" = false ]'
cli "a plain .rb file gets its arguments" '[ "$("$rsh" "$tmp/cli/args.rb" a "b c")" = "a|b c" ]'
echo "rsh golden: $pass passed, $fail failed"
[ "$fail" = 0 ]
