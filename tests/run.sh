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

# docs/rsh.md (0.9.0): every ```ruby block appears, line for line, in a test script
# (tests/examples/doc_*.rsh, tests/golden/doc_*.rb); the manual page's example is
# tests/examples/doc_man.rsh.
if [ "$have_names" = 0 ] && [ "$update" = 0 ]; then
	missing=$(awk -v dir="$here" '
		function slurp(f,   l, s) { s = ""; while ((getline l < f) > 0) s = s l "\n"; close(f); return s }
		BEGIN {
			cmd = "ls " dir "/examples/doc_*.rsh " dir "/golden/doc_*.rb 2>/dev/null"
			while ((cmd | getline f) > 0) all = all slurp(f) "\f"
			close(cmd)
		}
		/^```ruby$/ { inb = 1; blk = ""; start = NR; next }
		inb && /^```$/ { inb = 0; if (index(all, blk) == 0) print "docs/rsh.md:" start; next }
		inb { blk = blk $0 "\n" }
	' "$here/../docs/rsh.md")
	if [ -z "$missing" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL docs/rsh.md: a \`\`\`ruby block with no doc_* test holding it:"
		echo "$missing"
	fi
fi

# rsh.1 (0.9.0): the example of its "Sh" section is tests/examples/doc_sh.rsh, line for line.
if [ "$have_names" = 0 ] && [ "$update" = 0 ]; then
	sed -n '/^\.SS Sh:/,/^\.fi/p' "$here/../rsh.1" | sed -n '/^\.nf/,/^\.fi/p' | sed '1d;$d' | sed 's/\\-/-/g; s/\\e/\\/g' >"$tmp/man_sh"
	if [ -s "$tmp/man_sh" ] && awk -v want="$(cat "$tmp/man_sh")" '{ all = all $0 "\n" } END { exit index(all, want "\n") ? 0 : 1 }' "$here/examples/doc_sh.rsh"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL rsh.1: the Sh example is not tests/examples/doc_sh.rsh"
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
# 0.1.4 reader and CLI fixes (QA-100 round 2).
printf 'echo a |\n' >"$tmp/cli/pipe_end.rsh"
printf 'def f(\n' >"$tmp/cli/rb_bad.rsh"
printf 'x = 1 + (y)\nto_der == other\n' >"$tmp/cli/groups.rsh"
printf '# rsh: rubi\nputs 1\n' >"$tmp/cli/mode_typo.rb"
printf 'at_exit { puts "at_exit ran" }\nbegin\n  loop {}\nensure\n  puts "ensure ran"\nend\n' >"$tmp/cli/term.rsh"
printf 'while true; end\n' >"$tmp/cli/tight.rsh"
printf 'echo hi\n' >"$tmp/cli/noexec.rsh"
chmod 644 "$tmp/cli/noexec.rsh"
cli "-n: a | at the end of a line is a mistake (M-2)" '! "$rsh" -n "$tmp/cli/pipe_end.rsh" 2>/dev/null'
cli "--explain exits 2 for Ruby that does not compile, as -n (R2-18)" '! "$rsh" --explain "$tmp/cli/rb_bad.rsh" >/dev/null 2>&1'
cli "-n warns about a bare word alone in ( ) (R2-3)" '"$rsh" -n "$tmp/cli/groups.rsh" 2>&1 | grep -q "groups.rsh:1: warning: .y. inside ( ) is run as a command"'
cli "-n warns about a command whose first argument is == (R2-12)" '"$rsh" -n "$tmp/cli/groups.rsh" 2>&1 | grep -q "groups.rsh:2: warning: .==. is the first argument"'
cli "--lint is 1 for the reader's warnings (R2-3)" '"$rsh" --lint "$tmp/cli/groups.rsh" >/dev/null 2>&1; [ $? = 1 ]'
cli "an unknown # rsh: mode is status 2 (M-12)" '"$rsh" "$tmp/cli/mode_typo.rb" 2>&1 | grep -q "unknown mode .rubi." && ! "$rsh" -n "$tmp/cli/mode_typo.rb" 2>/dev/null'
cli "rsh DIR is 126, Is a directory (M-11)" '"$rsh" "$tmp/cli" 2>/dev/null; [ $? = 126 ]'
cli "a script path that is not executable is 126 (M-11)" '(cd "$tmp/cli" && "$rsh" -c ./noexec.rsh 2>/dev/null); [ $? = 126 ]'
printf 'echo a\0b\n' >"$tmp/cli/nul.rsh"
cli "a NUL byte in the source is named, not written raw (L-12)" '"$rsh" "$tmp/cli/nul.rsh" 2>&1 | grep -q "nul.rsh:1: unexpected NUL byte"'
cli "--lint: a fixed name under /tmp as a redirect target (L-15)" '"$rsh" --lint -c "echo hi > /tmp/fixed" 2>&1 | grep -q "fixed name under /tmp"'
cli "an uncaught Rsh.eval syntax error names its text (L-9)" '"$rsh" -c "Rsh.eval(\"x = (\")" 2>&1 | grep -q "^rsh: (rsh eval):1: syntax error"'
cli "wait for a job that is not there says so (L-13)" '"$rsh" -c "wait 12345" 2>&1 | grep -q "wait: 12345: no such job"'
cli "-c with no text says so (L-8)" '"$rsh" -c 2>&1 | grep -q -- "-c needs TEXT"'
if command -v timeout >/dev/null; then
	cli "TERM during a Ruby loop runs ensure and at_exit, then ends by the signal (H-2)" \
		'[ "$(timeout --preserve-status -s TERM 0.5 "$rsh" "$tmp/cli/term.rsh" 2>&1; echo "s$?")" = "$(printf "ensure ran\nat_exit ran\ns143")" ]'
	cli "INT during a Ruby loop: the same, 130 (H-2)" \
		'[ "$(timeout --preserve-status -s INT 0.5 "$rsh" "$tmp/cli/term.rsh" 2>&1; echo "s$?")" = "$(printf "ensure ran\nat_exit ran\ns130")" ]'
	printf 'at_exit { puts "at_exit ran" }\nbegin\n  $ sleep 5\nensure\n  puts "ensure ran"\nend\n' >"$tmp/cli/waiting.rsh"
	# TERM to the shell alone (not its group) while a command runs: the command gets it, ensure and
	# at_exit run, and it takes well under the 2-second watchdog (it waited the 2 s and ran neither).
	cli "TERM while a foreground command runs: forwarded, ensure and at_exit run, status 143 (H-2)" \
		'(a=$(date +%s%N); "$rsh" "$tmp/cli/waiting.rsh" >"$tmp/cli/waiting.out" 2>&1 & p=$!; sleep 0.3; kill -TERM $p; wait $p; rc=$?; b=$(date +%s%N); [ $rc = 143 ] && [ "$(cat "$tmp/cli/waiting.out")" = "$(printf "ensure ran\nat_exit ran")" ] && [ $(((b - a) / 1000000)) -lt 1500 ]) 2>/dev/null'
	cli "TERM in code with no safe point still ends it (the 2-second watchdog)" \
		'timeout --preserve-status -k 5 -s TERM 0.3 "$rsh" "$tmp/cli/tight.rsh" 2>/dev/null; [ $? = 143 ]'
fi
# 0.9.0 (docs/STABILITY.md, "0.9.0 gaps"): the script interpreter's fixes, each failing on 0.1.4.
cli "G1: a script on standard input names its place as -:LINE" \
	'[ "$(printf "\nls /nonexistent-rsh-g1 2>/dev/null\n" | "$rsh" 2>&1)" = "rsh: -:2: ls exited 2" ]'
cli "G1: the same with -s" '[ "$(printf "raise %s\n" "\"g1\"" | "$rsh" -s 2>&1)" = "rsh: -:1: g1 (RuntimeError)" ]'
cli "G3: \${NAME:-x} is a reader mistake that says what to write (status 2)" \
	'out=$("$rsh" -c "echo \${NOPE:-x}" 2>&1); [ $? = 2 ] && printf "%s" "$out" | grep -q "is sh: .{...} takes a variable name only; write #{ENV.fetch(\"NOPE\", \"default\")}"'
printf 'one\ntwo\nthree\n' >"$tmp/cli/lines.txt"
printf 'l = gets\necho got #{l.chomp}\ncat\np gets\n' >"$tmp/cli/gets_then_cat.rsh"
cli "G5: gets, then a command reading the same standard input (a file): the command gets the rest" \
	'[ "$("$rsh" "$tmp/cli/gets_then_cat.rsh" <"$tmp/cli/lines.txt")" = "$(printf "got one\ntwo\nthree\nnil")" ]'
printf 'printf "b\\na\\n" | sort | head -1\n' >"$tmp/cli/printf_pipe.rsh"
cli "G10: a syntax error on a line that starts with printf says it is Ruby's printf and how to run the program" \
	'"$rsh" "$tmp/cli/printf_pipe.rsh" 2>&1 | grep -q "printf_pipe.rsh:1: syntax error.*(.printf. at the start of a line is Ruby.s printf; for the program write \$ printf ...)" && "$rsh" -n "$tmp/cli/printf_pipe.rsh" 2>&1 | grep -q "for the program write"'
printf 'x = (\n' >"$tmp/cli/bad_lib.rsh"
cli "G11: a syntax error in a sourced file names that file and its line" \
	'(cd "$tmp/cli" && "$rsh" -c "source bad_lib.rsh" 2>&1 | grep -q "^rsh: bad_lib.rsh:2: syntax error")'
# 0.9.0 review (6): a folder that cannot be read is said on standard error and the walk goes on, as
# find(1) (not as root, who reads every folder).
if [ "$(id -u)" != 0 ]; then
	mkdir -p "$tmp/cli/walk/locked/in" "$tmp/cli/walk/open"
	chmod 0 "$tmp/cli/walk/locked"
	cli "S6: Sh.find, Sh.du and Find.find report an unreadable folder and go on" \
		'out=$(cd "$tmp/cli/walk" && "$rsh" -c "p Sh.find(\".\", type: :dir); p Sh.du(\".\") >= 0; Find.find(\".\") { }" 2>&1); printf "%s" "$out" | grep -q "rsh: find: ./locked: Permission denied" && printf "%s" "$out" | grep -q "rsh: du: ./locked: Permission denied" && printf "%s" "$out" | grep -q "\[\"./locked\", \"./open\"\]"'
	chmod 755 "$tmp/cli/walk/locked"
fi
mkfifo "$tmp/cli/fifo" 2>/dev/null && printf 'source fifo\nputs 1\n' >"$tmp/cli/src_fifo.rsh" &&
	cli "D3: the read-ahead never opens a FIFO (rsh -n would hang)" 'timeout 5 "$rsh" -n "$tmp/cli/src_fifo.rsh" 2>/dev/null'
# M1: a long interactive session does not grow (an arena slot and a string per input, the command
# lines of every input kept: about 250 and 600 bytes a line). 20,000 inputs through -i, RSS after
# 2,000 and after 20,000; under 1 MB of growth (0.1.4: about 7 MB; 0.9.0: about 0.3 MB).
awk 'BEGIN {
	rss = "File.read(\"/proc/self/status\")[/VmRSS:\\s+(\\d+)/, 1].to_i"
	for (i = 1; i <= 20000; i++) {
		print (i <= 10000 ? "x = 1" : "echo hi > /dev/null")
		if (i == 2000) print "$m1a = " rss
	}
	print "puts \"M1 growth #{" rss " - $m1a}\""
}' >"$tmp/cli/session.txt"
# (not on the sanitizer build: its quarantine holds freed memory on purpose)
if [ -z "${ASAN_OPTIONS:-}" ]; then
	cli "M1: 20,000 interactive inputs grow the shell by under 1 MB" \
		'g=$(HOME="$tmp/cli" "$rsh" -i <"$tmp/cli/session.txt" 2>/dev/null | sed -n "s/.*M1 growth //p"); [ -n "$g" ] && [ "$g" -lt 1024 ]'
fi
echo "rsh golden: $pass passed, $fail failed"
[ "$fail" = 0 ]
