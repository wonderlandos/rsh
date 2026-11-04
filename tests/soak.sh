#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The sanitizer hour of the release gate (docs/STABILITY.md, sections 9 and 12): after `make asan`
# and `make fuzz` (whose corpus it reads), runs the sanitizer half of `make test` again and again for
# SECONDS_TOTAL seconds (default 3600), plus the sanitizer build reading 300 random corpus files with
# -n, --explain and --ruby each round. Any failure, report, time-out or signal is counted.
# Usage: SECONDS_TOTAL=3600 sh tests/soak.sh
cd "$(dirname "$0")/.." || exit 99
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=99 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:exitcode=98
log=$(mktemp -d)
trap 'rm -rf "$log"' EXIT
end=$(($(date +%s) + ${SECONDS_TOTAL:-3600}))
rounds=0 bad=0 files=0
while [ "$(date +%s)" -lt "$end" ]; do
	./build/asan/unit >"$log/unit" 2>&1 || { bad=$((bad + 1)); echo "unit failed"; tail -5 "$log/unit"; }
	./build/asan/unit_regexp >"$log/ure" 2>&1 || { bad=$((bad + 1)); echo "unit_regexp failed"; }
	RSH=$PWD/build/asan/rsh sh tests/run.sh >"$log/run" 2>&1 || { bad=$((bad + 1)); echo "run.sh failed"; grep FAIL "$log/run" | head; }
	sh tests/contract.sh build/asan/rsh >"$log/contract" 2>&1 || { bad=$((bad + 1)); echo "contract failed"; grep FAIL "$log/contract" | head; }
	for f in $(ls build/fuzz/corpus 2>/dev/null | shuf -n 300); do
		for m in -n --explain --ruby; do
			timeout 10 ./build/asan/rsh $m "build/fuzz/corpus/$f" >/dev/null 2>"$log/one"
			rc=$?
			files=$((files + 1))
			if [ $rc = 99 ] || [ $rc = 98 ] || [ $rc = 124 ] || [ $rc -ge 128 ]; then
				bad=$((bad + 1))
				echo "corpus $f $m: status $rc"
				head -5 "$log/one"
			fi
		done
	done
	rounds=$((rounds + 1))
	echo "round $rounds done at $(date +%T), corpus runs $files, problems $bad"
done
echo "soak: $rounds rounds, $files corpus runs, $bad problems"
[ "$bad" = 0 ]
