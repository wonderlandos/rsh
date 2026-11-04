#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The stability contract (docs/STABILITY.md): fails when a frozen item changes. Frozen: the flags
# and the -h text, the exit statuses (with --lint 0/1/2), the message formats, value safety, the
# --explain output of tests/golden/classify.rsh, the Ruby words and the builtins, the file names,
# the build inputs, the release gate and the CI pins. A change here is a major version: then run
# `sh tests/contract.sh --print path/to/rsh` and put the new digests below, with the reason in
# CHANGELOG.md.
# Usage: sh tests/contract.sh [--print] path/to/rsh
set -u
print=0
if [ "${1:-}" = "--print" ]; then
	print=1
	shift
fi
here=$(cd "$(dirname "$0")" && pwd)
top=$here/..
rsh=${1:-$top/build/rsh}
case $rsh in /*) ;; *) rsh=$PWD/$rsh ;; esac
pass=0
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
cd "$tmp" || exit 99

# ---- frozen digests (0.1.3) ------------------------------------------------------------------
# EXPLAIN_SHA re-printed in 0.1.4: classify.rsh gained the cases of the reader fixes M-1 (`echo
# {a,b}`), M-4 (`! cmd`), R2-4 (`Pair=Struct.new(:x) do` is Ruby) and R2-7 (`cmd || exit 1`); the
# lines that were there read as before (CHANGELOG.md, 0.1.4).
USAGE_SHA=ade70bab185af47172c3afca88964ec5e08b13a1eedff5e12f1936a24257130e
EXPLAIN_SHA=926c120c89eb747ea34d65027d122a628869a3d784512c5e04681c3f69f55fee
WORDS_SHA=06f3f08a2192c35b08799e82141a30efb8198a9e03227c84f15e2a8d62a2d76a
BUILTINS_SHA=76e3078a9e447355f0e2f4cd969b5e2939a4f548e9e74b3c9b042285ad16e166
BUILD_SHA=1b2c61f0bdeb43241be9020716011ec8d7293d49dd15f805c7009fcb4de1ef6a
# 0.9.0: the names of the built-in Sh, FileUtils, Find and Shellwords (docs/design.md S8). Printed again
# in 0.9.1: Sh.tmpdir gained an optional suffix (for Dir.mktmpdir([prefix, suffix]), audit 2 L10).
STD_SHA=f1f281a1839ebad532fb90f0648c539320b2e1edd6950cc86da0e6eb4de3e9e5

ok() { # label condition...
	label=$1
	shift
	if "$@"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL contract: $label"; fi
}
sha() { sha256sum | cut -c1-64; }
digest() { # label frozen actual
	if [ "$print" = 1 ]; then
		echo "$1=$3"
	else
		ok "$1 changed (frozen item; docs/STABILITY.md)" [ "$2" = "$3" ]
	fi
}
status() { # expected command...: the exit status of the command, output dropped
	want=$1
	shift
	"$@" >/dev/null 2>&1
	[ "$?" = "$want" ]
}
err_is() { # expected-stderr command...
	want=$1
	shift
	got=$("$@" 2>&1 >/dev/null)
	[ "$got" = "$want" ] || { echo "  wanted: $want"; echo "  got:    $got"; return 1; }
}

# 3. Flags: the -h text is frozen; each flag does what it says.
digest USAGE_SHA "$USAGE_SHA" "$("$rsh" -h | sha)"
ok "--help = -h" [ "$("$rsh" --help)" = "$("$rsh" -h)" ]
ok "-v" [ "$("$rsh" -v | cut -d' ' -f1)" = rsh ]
ok "--version = -v" [ "$("$rsh" --version)" = "$("$rsh" -v)" ]
ok "-c" [ "$("$rsh" -c 'echo c')" = c ]
ok "-e" [ "$("$rsh" -e 'puts 1 + 1')" = 2 ]
ok "-s with arguments after --" [ "$(echo 'puts ARGV.join(",")' | "$rsh" -s -- a b)" = a,b ]
ok "--name" [ "$(echo 'puts $0' | "$rsh" -s --name x.rsh)" = x.rsh ]
printf 'echo f\n' >f.rsh
ok "-- then the file" [ "$("$rsh" -- f.rsh)" = f ]
ok "-n runs nothing" [ -z "$("$rsh" -n f.rsh)" ]
ok "--ruby" [ "$("$rsh" --ruby f.rsh | cut -c1-10)" = "__rsh_cmd(" ]
ok "--explain" [ "$("$rsh" --explain f.rsh | head -1)" = "mode: shell" ]
# -i and -l without a terminal: plain lines after a prompt; a private home for the history.
inter() { echo "$2" | env HOME="$tmp" XDG_STATE_HOME= XDG_CONFIG_HOME= "$rsh" "$1" 2>/dev/null | grep -c " $3\$"; }
ok "-i reads lines without a terminal" [ "$(inter -i 'echo ii' ii)" = 1 ]
ok "-l = -i" [ "$(inter -l 'echo ll' ll)" = 1 ]
ok "an unknown flag is status 2" status 2 "$rsh" --no-such-flag

# 4. Exit statuses.
ok "0 ok" status 0 "$rsh" -c 'echo ok'
ok "a failed command: its status" status 7 "$rsh" -c 'sh -c "exit 7"'
ok "a signal: 128+N" status 143 "$rsh" -c "sh -c 'kill -TERM \$\$'"
ok "130 interrupt" status 130 "$rsh" -c 'raise Interrupt'
ok "1 Ruby error" status 1 "$rsh" -c 'raise "x"'
ok "2 syntax" status 2 "$rsh" -c 'x = ('
ok "127 file not readable" status 127 "$rsh" ./no-such-script.rsh
ok "127 command not found" status 127 "$rsh" -c 'zz-no-such-command-zz'
printf 'not a program\n' >notexec
chmod +x notexec
ok "126 cannot run" status 126 "$rsh" -c './notexec'
printf 'echo x\n' >noxbit
chmod 644 noxbit
ok "126 found, not executable (0.1.4)" status 126 "$rsh" -c './noxbit'
ok "126 a folder as the script (0.1.4)" status 126 "$rsh" .
ok "127 a missing program after || is still 127 (0.1.4)" status 127 "$rsh" -c 'sh -c "exit 1" || zz-no-such-command-zz'
ok "exit after || is Ruby's exit (0.1.4)" status 1 "$rsh" -c 'zz-no-such-command-zz 2>/dev/null || exit 1'
ok "exit N" status 5 "$rsh" -c 'exit 5'
printf 'puts 1\n' >clean.rsh
printf 'zz-no-such-command-zz\n' >warn.rsh
printf 'x = (\n' >bad.rsh
ok "--lint 0 clean" status 0 "$rsh" --lint clean.rsh
ok "--lint 1 warnings" status 1 "$rsh" --lint warn.rsh
ok "--lint 2 errors" status 2 "$rsh" --lint bad.rsh
ok "-n 0 clean" status 0 "$rsh" -n warn.rsh
ok "-n 2 errors" status 2 "$rsh" -n bad.rsh

# 5. Message formats.
printf 'echo one\nsh -c "exit 3"\n' >failed.rsh
ok "Failed: rsh: FILE:LINE: PROG exited N" err_is "rsh: failed.rsh:2: sh exited 3" "$rsh" failed.rsh
printf "sh -c 'kill -TERM \$\$'\n" >sig.rsh
ok "Failed: was stopped by signal N" err_is "rsh: sig.rsh:1: sh was stopped by signal 15" "$rsh" sig.rsh
printf 'puts 1\nraise ArgumentError, "bad"\n' >rb.rsh
ok "Ruby error: rsh: FILE:LINE: TEXT (Class)" err_is "rsh: rb.rsh:2: bad (ArgumentError)" "$rsh" rb.rsh
ok "Rsh::Failed#message" [ "$("$rsh" -c 'begin; sh -c "exit 4"; rescue Rsh::Failed => e; puts e.message; end')" = "sh exited 4" ]
ok "lint: FILE:LINE: warning: TEXT" [ "$("$rsh" --lint warn.rsh 2>&1 | cut -d' ' -f1-2)" = "warn.rsh:1: warning:" ]
ok "syntax: rsh: FILE:LINE: TEXT" err_is "rsh: bad.rsh:2: syntax error, unexpected end of file" "$rsh" bad.rsh
ok "command not found" [ "$("$rsh" -c 'zz-no-such-command-zz' 2>&1 | head -1)" = "rsh: zz-no-such-command-zz: command not found" ]

# 6. Value safety.
ok "one value, one argument" [ "$("$rsh" -c 'x = "a b"; $ printf "[%s]" #{x}')" = "[a b]" ]
ok "an Array, one argument each" [ "$("$rsh" -c 'x = %w[a b]; $ printf "[%s]" #{x}')" = "[a][b]" ]
ok "nil is an error" status 1 "$rsh" -c 'x = nil; echo #{x}'
ok "NUL is an error" status 1 "$rsh" -c 'x = "a\0b"; echo #{x}'
ok "a value is not a pattern" [ "$("$rsh" -c 'x = "*"; echo #{x}')" = "*" ]
ok "no /bin/sh: open(\"|...\")" status 1 "$rsh" -c 'open("|true")'
ok "no /bin/sh: IO.popen" status 1 "$rsh" -c 'IO.popen("true")'
ok "no /bin/sh: a run-time capture" status 1 "$rsh" -c 'send(96.chr.to_sym, "true")'
ok "system takes arguments, not a line" status 1 "$rsh" -c 'system("echo a b")'

# 1, 2. The rule, --explain, the Ruby words and the builtins.
digest EXPLAIN_SHA "$EXPLAIN_SHA" "$(cat "$here/golden/classify.rsh" "$here/golden/classify.explain" | sha)"
cp "$here/golden/classify.rsh" classify.rsh
ok "--explain of classify.rsh is classify.explain" [ "$("$rsh" --explain classify.rsh)" = "$(cat "$here/golden/classify.explain")" ]
words=$(sed -n '/^const char\* const RUBY_WORDS\[\] = {/,/nullptr};/p' "$top/src/transpile.cpp" | grep -o '"[^"]*"' | tr -d '"')
digest WORDS_SHA "$WORDS_SHA" "$(echo "$words" | sha)"
builtins=$(sed -n '/^static const char\* const BUILTINS\[\] = {/,/nullptr};/p' "$top/src/exec.cpp" | grep -o '"[^"]*"' | tr -d '"')
digest BUILTINS_SHA "$BUILTINS_SHA" "$(echo "$builtins" | sha)"
ok "sleep is a command" [ "$(printf 'sleep 0\n' >s.rsh && "$rsh" --explain s.rsh | sed -n 2p | cut -c6)" = C ]

# 1 (0.9.0): how every test script and example is read is frozen, file by file. tests/frozen.explain
# holds "SHA256  FILE" of `rsh --explain FILE` for each; a file not in the list is not checked (a new
# test adds its line with --print-explain, an existing line only changes in a major version).
if [ "${CONTRACT_PRINT_EXPLAIN:-0}" = 1 ]; then
	for f in "$top"/tests/golden/*.rsh "$top"/tests/golden/*.rb "$top"/tests/golden/*.lib "$top"/tests/examples/*.rsh "$top"/examples/meta/*.rsh; do
		echo "$("$rsh" --explain "$f" 2>/dev/null | sha)  ${f#"$top"/}"
	done
	exit 0
fi
changed=
while read -r want f; do
	[ -f "$top/$f" ] || { changed="$changed $f(missing)"; continue; }
	[ "$("$rsh" --explain "$top/$f" 2>/dev/null | sha)" = "$want" ] || changed="$changed $f"
done <"$here/frozen.explain"
ok "every frozen script reads as before (tests/frozen.explain)${changed:+:$changed}" [ -z "$changed" ]

# 1 (0.9.0): the built-in helpers' names are frozen (docs/design.md S8); Rsh.last_pid (D2) is there.
std=$("$rsh" -c 'Rsh.__std; [Sh, FileUtils, Find, Shellwords].each { |m| puts "#{m}: #{m.instance_methods(false).reject { |x| x.to_s.start_with?("__") }.sort.join(" ")}" }; puts "Sh.find: #{Sh.instance_method(:find).parameters.inspect}"; puts "Sh.tmpdir: #{Sh.instance_method(:tmpdir).parameters.inspect}"; puts "String: #{[:shellescape, :shellsplit].select { |x| String.method_defined?(x) }.inspect}"; puts "Array: #{Array.method_defined?(:shelljoin)}"; puts "Dir.mktmpdir: #{Dir.method(:mktmpdir).parameters.inspect}"; puts "Rsh.last_pid: #{Rsh.respond_to?(:last_pid)}"')
digest STD_SHA "$STD_SHA" "$(echo "$std" | sha)"
ok "Rsh.last_pid" [ "$("$rsh" -c 'p Rsh.last_pid')" = nil ]

# 3, 9 (0.9.0): the version is one number everywhere.
ver=$(cat "$top/VERSION")
ok "rsh -v is VERSION" [ "$("$rsh" -v)" = "rsh $ver" ]
ok "packaging.PKGBUILD pkgver is VERSION" grep -qx "pkgver=$ver" "$top/packaging.PKGBUILD"
ok "CHANGELOG.md has a section for VERSION" grep -qx "## $ver" "$top/CHANGELOG.md"

# 5 (0.9.0): a script on standard input is named "-" in messages, as a file is.
ok "stdin script: rsh: -:LINE: PROG exited N" err_is "rsh: -:1: sh exited 3" sh -c "printf 'sh -c \"exit 3\"\n' | \"$rsh\""

# 7. Files.
ok "history file name" grep -q '"/rsh/history"' "$top/src/main.cpp"
ok "init file name" grep -q '"/rsh/init.rsh"' "$top/src/main.cpp"
ok "state default" grep -q '"/.local/state"' "$top/src/main.cpp"
ok "config default" grep -q '"/.config"' "$top/src/main.cpp"

# 8. Build inputs: the interpreter, its gems and defines, the libraries, the regexp limits.
build=$( (grep -E '^MRUBY_(COMMIT|SHA256)|^LIBS' "$top/Makefile"
	grep -v '^ *#' "$top/build_config.rb"
	grep -E 'constexpr uint32_t MATCH_LIMIT' "$top/src/rt.cpp") | sha)
digest BUILD_SHA "$BUILD_SHA" "$build"

# 9. The release gate and the CI pins.
ok "make test runs the contract" grep -q 'sh tests/contract.sh' "$top/Makefile"
ok "make test runs the sanitizer build" grep -q 'MAKE) --no-print-directory asan' "$top/Makefile"
ok "the sanitizer run is on by default" grep -q '^SANITIZE ?= 1' "$top/Makefile"
ok "the sanitizer build checks leaks" grep -q 'detect_leaks=1' "$top/Makefile"
if [ -f "$top/.github/workflows/ci.yml" ]; then
	unpinned=$(grep -E '^\s*-? *uses:' "$top/.github/workflows/ci.yml" | grep -vE '@[0-9a-f]{40}( |$)' || true)
	ok "CI actions pinned to a commit${unpinned:+: $unpinned}" [ -z "$unpinned" ]
fi

[ "$print" = 1 ] && exit 0
echo "rsh contract: $pass passed, $fail failed"
[ "$fail" = 0 ]
