# rsh stability contract

rsh is a script interpreter that is rarely updated, like the GNU tools: once 1.0 is out it changes for a
serious security problem, a large speed-up or a large missing function, and for nothing else. From 0.1.3
the items below are frozen: a script, a program that reads rsh's output, or a package that builds it can
depend on them. Changing any of them is a major version (2.0); a minor or patch version only fixes
mistakes and adds things that do not change these. `tests/contract.sh` fails when one of them changes;
`make test` runs it. 0.9.0 is the release candidate for 1.0: section 11 is the 1.0 contract (all its
items met), section 10 the gaps found for it, section 12 the measured numbers.

What rsh is for (the lead's scope, 2026-11-04): running scripts, as `bash` runs `.sh` files. `rsh FILE`,
`#!/usr/bin/rsh`, `-c`, a script on standard input, its arguments, exit statuses, signals, pipelines,
redirections, here-documents, patterns, quoting, the environment, `cd`/`exec`/`source`, and the rule that
mixes Ruby and command lines. The interactive mode stays as it is (its tests stay green) but is not part
of the 1.0 promise beyond "no regressions"; rsh is not meant as a login shell.

Where an item names a file, that file is the reference; this page says what is frozen in it.

## 1. How a line is read (the classification rule)

The five rules of `docs/rsh.md`, "How a line is read", decide whether a statement is Ruby or a
command line. They look only at the text of the file, never at `PATH` or the machine.

- The escape hatches: a leading `$ ` forces a command line; `name(...)` and `self.name` force Ruby;
  `ruby do ... end` and `ruby { ... }` make every statement inside Ruby; `ruby!` alone on its line does
  the same to the end of the file; `# rsh: ruby` as the first line (or the line after `#!`) turns the
  shell layer off for the file, `# rsh: shell` there says the default.
- The Ruby words (rule 4) are one list, `RUBY_WORDS` in `src/transpile.cpp`. The list is frozen as it
  is in 0.1.3; it only grows, and only in a major version. `sleep` is a command, not a Ruby word.
- The built-in commands are the list `BUILTINS` in `src/exec.cpp`: `cd pushd popd dirs export unset
  exec umask jobs fg bg wait source echo type pwd history`.
- `tests/golden/classify.rsh` with `tests/golden/classify.explain` is the frozen proof of every case.

## 2. The `--explain` format

```
mode: shell                                  (or: mode: ruby (the shell layer is off: ...))
methods: NAME NAME ...                       (only when there are any)
ruby only: line N (a ruby block)             (one line per span, as they occur)
ruby only: lines N-M (a ruby block)
ruby only: line N to the end (ruby!)
%4d K  TEXT                                  (every line of the file)
```

`K` is `R` (Ruby), `C` (a command line), `X` (a back-quote capture inside Ruby) or a blank for a blank
line. Machine readers may depend on this. The exit status is 0, or 2 when the file has a mistake.

## 3. Flags

`-c TEXT`, `-e TEXT`, `-s`, `--name FILE`, `-n`, `--lint`, `--explain`, `--ruby`, `-i`, `-l`, `-v`
(`--version`), `-h` (`--help`), `--`. No flag gets a new meaning. A new flag may be added in a minor
version; an unknown flag is an error (status 2). `rsh -h` prints the list (frozen text).

## 4. Exit status

| status | when |
|---|---|
| 0 | the script ended normally |
| N | a command failed and the script did not handle it: that command's status |
| 128+N | that command was ended by signal N |
| 130 | interrupted (Ctrl-C, `Interrupt`) |
| 1 | a Ruby error the script did not rescue |
| 2 | a syntax or reader mistake in the file; a bad flag |
| 127 | the script file cannot be read; a command that is not found |
| 126 | a command found but not runnable |
| 70 | the interpreter could not start |
| `exit N` | N |

`rsh -n` and `rsh --lint`: 0 clean, 1 warnings only (`--lint`), 2 errors. `wol script check` tells
warnings from errors by this.

## 5. Message format

- Every message of rsh is one line on standard error that starts with `rsh: `.
- A mistake at a place in a file: `rsh: FILE:LINE: TEXT`; a Ruby error adds ` (ClassName)`.
- An unhandled failed command: `rsh: FILE:LINE: PROG exited N` or `rsh: FILE:LINE: PROG was stopped
  by signal N`. `Rsh::Failed#message` is the text after `FILE:LINE: `.
- `-n` and `--lint`: `FILE:LINE: TEXT` and `FILE:LINE: warning: TEXT` (no `rsh: `, as compilers do).
- A command that is not found: `rsh: PROG: command not found`.
- A script read from standard input (`rsh`, `rsh -s`, `rsh -`) is the file `-`: `rsh: -:LINE: ...` (0.9.0;
  before, the place was left out); with `-s --name FILE` it is FILE.
- A mistake in a file loaded with `source`, `require_relative` or `load` names that file and its line.

The texts after the place may be made clearer in a minor version; the shape above does not change.

## 6. Values in command lines

- One Ruby value is one argument; an Array is one argument per element; `nil` is an error.
- A NUL byte in an argument, an Array element, a `NAME=` value or a redirect target is an error (a
  here-document may hold one).
- A value is never split, never expanded as a pattern; a file found by a pattern whose name starts
  with `-` is given as `./-name`.
- No text ever reaches `/bin/sh`: command lines are parsed once, when the file is read; `system` takes
  separate arguments; `` Kernel#` `` with a run-time string, `IO.popen` and `open("|...")` are
  `Rsh::Error`, and the interpreter is built without popen.
- An empty part of `PATH` is skipped (it is not the current folder); with `PATH` unset rsh uses
  `/usr/local/bin:/usr/bin:/bin`; with `HOME` unset `~` is `/`.

## 7. Files

- Init: `$XDG_CONFIG_HOME/rsh/init.rsh` (else `~/.config/rsh/init.rsh`), interactive only.
- History: `$XDG_STATE_HOME/rsh/history` (else `~/.local/state/rsh/history`), mode 0600, one entry per
  line, a newline in an entry written `\n` and a backslash `\\`. A line that starts with a space, or
  holds what looks like a secret (`NAME=` with TOKEN, SECRET, PASSWORD, PASSWD, PASSPHRASE, API_KEY,
  APIKEY, PRIVATE_KEY, ACCESS_KEY, CREDENTIAL in the name; an Authorization or Bearer header; a known
  token prefix; a private key block), is not kept.
- Both files are used only when they are plain files (never through a link), owned by the effective
  user and not writable by others. As root, or when started set-user-id or set-group-id, rsh takes
  the home from `/etc/passwd` for the effective user, ignores `$HOME` and `$XDG_*` for these files,
  and also refuses files its group can write. Otherwise it says why on standard error and goes on
  without them.

## 8. Build inputs

- The interpreter: the commit and sha256 in `Makefile` (`MRUBY_COMMIT`, `MRUBY_SHA256`).
- What is compiled into it: the gem list and the defines in `build_config.rb` (`MRB_UTF8_STRING`,
  `MRB_STR_LENGTH_MAX=0`, `MRB_ARY_LENGTH_MAX=0`, `MRB_NO_IO_POPEN`).
- The only other library: pcre2 (8-bit), with a match limit of 1,000,000, a depth limit of 1,000,000
  and a heap limit of 256 MB per match (`src/rt.cpp`); no JIT.
- Link mode: `LINK=static` (the default; the CI artifact) is one file with no shared libraries.
  The WolOS package (`packaging.PKGBUILD`) ships `LINK=dynamic`: glibc and libpcre2-8 shared, the C++
  runtime linked in. Both are supported; the package is the one installed as `/usr/bin/rsh` on WolOS.
- CI actions are pinned to a commit.

Changing any of these is a major version; `tests/contract.sh` holds their digest.

## 9. Release gate

A release is tagged only when, on the build machine:

1. `make test` is green. It runs the unit checks, every golden and example, `tests/contract.sh`,
   the doctrine and size check (under 3 MB) and the interactive checks, then builds `make asan`
   (`LINK=dynamic`, `-fsanitize=address,undefined`, leak check on) and runs the unit checks, every
   golden (the audit reproducers among them: `tests/golden/sec_*.rsh`) and the contract on it. Any
   sanitizer report fails the run. (`make test SANITIZE=0` leaves the sanitizer half out for a host
   without the sanitizer libraries; such a run is not a release gate.)
2. `make bench` shows start-up within 5% of the previous release (median of 400 runs of
   `rsh -c nil`), and the size under 3 MB.
3. `CHANGELOG.md` names every change; `VERSION`, `packaging.PKGBUILD` (`pkgver`, and its sha256 once
   the tag exists) agree (`tests/contract.sh` checks the first three).
4. (0.9.0) `make fuzz FUZZ_SECONDS=1800` (on a machine with clang) ends with no crash, no time-out and no
   sanitizer report, and the sanitizer half of `make test` has run again and again for an hour with no
   report (`SECONDS_TOTAL=3600 sh tests/soak.sh`; section 12).

## 10. 0.9.0 gaps

Found by running rsh as a script interpreter for a day's worth of scripts (every form of section 11.1,
the scripts of `README.md`, `docs/rsh.md` and `examples/`), by the fuzz and sanitizer runs of section
12 and by a 10,000-input session. Priority: **P1** a person hits it on the first day, **P2** in the
first weeks, **P3** rarely. Each fix has a test that fails on 0.1.4 and passes on 0.9.0.

### Fixed

| id | P | what was wrong | test |
|---|---|---|---|
| G1 | P1 | A script read from standard input lost the place in every message (`rsh: ls exited 2`, no `-:LINE:`) | `tests/run.sh` cli "G1" (2), `tests/contract.sh` "stdin script" |
| G2 | P1 | `export NAME` for a name that is not set made an empty variable, so the sh habit `FOO=1; export FOO` (FOO=1 is Ruby in rsh) exported nothing, silently; now `rsh: export: FOO is not set: write export FOO=value ...`, status 1 | `golden/r9_script.rsh` |
| G4 | P1 | `cmd \|\| echo "$?"`: `$?` in a later part of the same line was the status from before the line (0), not the part that just ran | `golden/r9_spec.rsh` (S2.3) |
| M1 | P1 | A long interactive or `-i` session grew about 250 bytes per input (the input's code string and a slot of the interpreter's arena were never freed) and about 600 more per command line (every command line typed was kept); history in memory had no bound. Now: freed, a command line is dropped once its input ran (unless a method, block or proc made by it may run it later), history in memory is capped at 6,000 entries (the file keeps all) | `tests/run.sh` cli "M1" (20,000 inputs, under 1 MB; 0.1.4: about 7 MB) |
| G3 | P2 | `${NAME:-x}`, `${#NAME}`, `${NAME%.c}`: read as a variable with that odd name, failing only when the line ran ("$NOPE:-x is not set"); now a reader mistake (status 2) that says to write `#{ENV.fetch("NAME", "default")}` | `tests/run.sh` cli "G3" |
| G5 | P2 | `answer = gets`, then a command that reads the same standard input (`rsh job.rsh < list.txt`): the command started after the up to 4 KB the interpreter had read ahead; now the read-ahead is given back when standard input is a file | `tests/run.sh` cli "G5" |
| G7 | P2 | `source FILE WORDS` dropped the words; now the file sees `ARGV = WORDS` while it runs, then the script's ARGV comes back | `golden/r9_script.rsh` |
| G9 | P2 | No way in Ruby to ask about a file or the user without a program: `File.stat`, `File.lstat`, `File.ftype`, `File::Stat` (mode, size, uid, gid, times, `file?`, `directory?`, `symlink?`, `setuid?` ...), `File.readable?`, `writable?`, `executable?`, `owned?` (effective ids), `Process.uid`, `euid`, `gid`, `egid` (`abort "run me as root" unless Process.euid == 0`) | `golden/r9_script.rsh` |
| G10 | P2 | `printf "%s\n" x \| sort \| head`: the line is Ruby (rule 4: `printf` is a Ruby word) and the compiler's message did not say why; a syntax error on a line that starts with `printf`, `open`, `format`, `test`, `exec`, `eval`, `send`, `method`, `trap` or `p` now adds "(`printf` at the start of a line is Ruby's printf; for the program write $ printf ...)", in a run and in `-n` | `tests/run.sh` cli "G10" |
| G11 | P2 | A syntax error in a file loaded with `source` said `rsh: line 2: ...` with no file name | `tests/run.sh` cli "G11" |
| G12 | P3 | `require "time"` worked but gave nothing: `Time#iso8601`/`xmlschema`, `Time.iso8601` and `Time.parse` (ISO 8601 forms; another form is ArgumentError, not a guess) | `golden/r9_script.rsh` |
| G13 | P3 | The `require` message listed the built-in libraries as json, set, time (etc was missing) | `golden/r9_script.rsh` |

### Decided by the CTO and done (#plan 8620, 8654, 8713; the answer came during 0.9.0)

| id | P | what a person met | now | test |
|---|---|---|---|---|
| D1 | P1 | A pattern with no match and a redirect that cannot open were `Rsh::Error` even in a condition: `ls *.log 2>/dev/null \|\| echo none` and `cat < missing \|\| ...` stopped the script | a failed command with status 1, as a missing program is 127: `false` in a condition (a no-match silent there, an open failure said on standard error), `Rsh::Failed` outside one with the reason as its message (`rsh: FILE:LINE: no file matches *.tmp`); `nil`, NUL and an Array inside a word stay `Rsh::Error` | `golden/r9_decisions.rsh`, `golden/glob.rsh` |
| D2 | P2 | Ruby could not get the pid of `cmd &` (`$!` is Ruby's exception variable), and `wait N` took only a job number | `Rsh.last_pid` (nil before the first); `wait PID` waits for that process's job, `wait %N` for job N | `golden/r9_decisions.rsh`, contract "Rsh.last_pid" |
| D3 | P2 | `source lib.rsh`, then a bare call of a method from lib.rsh ran a program of that name | a `source <literal path>` file is read ahead for its methods, as `require_relative` (relative to the script's own folder when read and when run, never the current folder; only a plain file, never a FIFO or device; for `-c` and standard input the current folder; a path with `#{}`, `$`, `~` or a pattern keeps the old rule: call `name(...)` | `golden/r9_decisions.rsh` |
| S1 | P2 | The lead (2026-11-04): "a stdlib that is basically bash things (find etc), imported by default" | `Sh` built in and loaded on first use (`find grep which exists? file? dir? age size du read write append lines mkdir_p touch cp cp_r mv ln_s chmod rm rm_rf tmpdir`), plus `FileUtils`, `Find`, `Shellwords`, `Dir.mktmpdir`; no program started, Ruby values back; names frozen (`docs/design.md` S8, contract "STD_SHA") | `golden/r9_sh.rsh`, `examples/doc_sh.rsh` |

### Known limits (documented, not changed)

- **K1** After `gets` from a pipe (not a file), a command that reads the same standard input misses what
  the interpreter read ahead (up to 4 KB): a pipe cannot be given back. Read the rest in Ruby, or give
  the command its own input.
- **K2** The built-in interpreter's parser takes time that grows with the square of the number of
  statements in one file: 2,000 lines take under 10 ms, 16,000 lines 0.24 s, 30,000 lines 1 s. Split a
  generated script of that size, or load parts with `require_relative`.
- **K3** Code nested about 1,000 levels deep is "too complex expression" from the interpreter's compiler,
  which prints that line itself, without `rsh: ` in front.
- **K4** `#{...}` values of a command line are read once, when the line starts: `cmd || echo
  #{$?.exitstatus}` sees the status from before the line; write `$?` (a command-line word, read when that
  part runs).
- **K5** `gets` reads standard input, never the files named in ARGV (there is no ARGF): arguments of a
  script are its options, as in sh.
- **K6** A script on standard input is read whole before it runs, so a command in it cannot read the
  script's later lines (sh reads line by line; rsh never runs half a file).
- **K7** The library is the built-in one (`docs/rsh.md`, "What is inside" and "Sh"): `FileUtils`, `Find`,
  `Shellwords` and `Dir.mktmpdir` are there with a small option set (no `noop:`/`verbose:`), but no
  `Process.spawn`/`wait`, no `Open3`, and no `Time.parse` beyond ISO 8601; use the programs.
- **K8** A program ended by SIGPIPE because rsh's own output was closed (`rsh -c 'seq 1 200000' | head -1`)
  is a failed command, status 141, with its message (decided in 0.1.4).

## 11. The 1.0 contract

1.0 is tagged when every item below is met; 0.9.0 meets the ones marked **met**.

1. **Syntax and semantics frozen; `docs/design.md` is the specification and every rule has a golden
   test.** Met: `docs/design.md` is the specification (S1-S8), each numbered rule names its test,
   `tests/golden/r9_spec.rsh` covers the rules that had no case of their own, `r9_decisions.rsh` the
   decisions D1-D3, `r9_sh.rsh` every built-in helper, and `tests/frozen.explain` freezes how every
   test script and example is read (`--explain`, file by file; 0.9.0 reads all 70 exactly as 0.1.4 does:
   D3 only adds method names from a file that exists when the script is read).
2. **Zero known open bugs.** Met: everything found is fixed (G1-G13, M1, D1-D3) or a documented limit
   (K1-K8).
3. **One hour of the sanitizer run and a fuzz of the reader clean; no memory growth in a 10,000-input
   session; start-up under 5 ms.** Met (numbers in section 12).
4. **A complete script interpreter** (the scope above): `rsh FILE`, `#!/usr/bin/rsh` and `#!/usr/bin/env
   rsh`, `-c`/`-e`, `-s` and a script on standard input, ARGV and `$0`, exit statuses, signals,
   pipelines with every stage's status, redirections, here-documents, patterns, quoting, the
   environment, `cd`/`exec`/`source`, and the shell things as Ruby values (`Sh`). Met. The
   login-shell part of the first brief was dropped by the lead (2026-11-04 10:38Z): rsh is not added to
   `/etc/shells` and needs no PAM or profile files.
5. **The command-line options and the script format declared stable, with a test that fails when they
   change.** Met: sections 1-6 and 8 here; `tests/contract.sh` holds the digest of `rsh -h`, of the
   reader's word lists and of the build inputs, checks every flag and exit status, the message shapes,
   `tests/frozen.explain` and that `VERSION`, `rsh -v`, `packaging.PKGBUILD` and `CHANGELOG.md` agree.
6. **Manual page, exit statuses and documentation complete.** Met: `rsh.1` (OPTIONS, EXIT STATUS,
   SIGNALS, ENVIRONMENT, FILES, the 0.9.0 additions), section 4 here, `docs/rsh.md`, `docs/design.md`.
   Every example of `README.md` and of `docs/rsh.md` is a test (`tests/run.sh` fails when a ```ruby
   block is in no test script), and so are the manual page's (`tests/examples/doc_man.rsh`, and its
   "Sh" example, checked line for line against `tests/examples/doc_sh.rsh`).

## 12. Stability numbers (0.9.0)

Measured on the AWS build box (32 cores, x86_64), 2026-11-04, with other jobs running (load about 14).

- **Fuzz of the reader** (`make fuzz`, `tests/fuzz/reader.cpp`: the definition pre-pass, the reader, the
  one-command-line parser and the interpreter's parser on the Ruby it writes, under libFuzzer with the
  address and undefined-behaviour sanitizers; seeds: every test script): 1,818 s with 8 workers on the
  0.9.0 reader, 12,514,627 inputs, **0 crashes, 0 time-outs (5 s), 0 out-of-memory (2 GB), 0 sanitizer
  reports**; 20,734 edges covered, corpus 2,894 inputs. (A first 30-minute run on the reader before the
  G3 change, 10.5 million inputs, found nothing either; its crash counter jumped to 1,638 in one second
  when its program file was replaced under it, and no crash input was written: those were failed
  starts, not findings.)
- **Sanitizer hour** (`make asan`; for 60 minutes, again and again: the unit checks, the Regexp checks,
  every golden and example, the cli cases and the contract on the sanitizer build, plus that build
  reading 300 random fuzz-corpus files with `-n`, `--explain` and `--ruby` each round;
  `tests/soak.sh`): 11:13-12:13, **150 rounds, 0 failures, 0 sanitizer reports**; each
  round 255 unit checks, 2,164,185 Regexp checks, 138 golden/example/cli cases (the M1 memory case is
  left out on this build: its quarantine holds freed memory on purpose) and 70 contract checks;
  135,000 corpus runs with no report, no time-out (10 s) and no signal.
- **Re-run after D1-D3 and Sh** (the final 0.9.0 code, 12:33-12:54, the box otherwise quiet): fuzz
  910 s with 8 workers, 6,226,038 inputs, **0 crashes, 0 time-outs, 0 out-of-memory**, 21,376 edges (the
  `source` read-ahead parses there too); `tests/soak.sh` for 20 minutes, **50 rounds, 45,000 corpus runs,
  0 problems** (the golden count now includes `r9_decisions`, `r9_sh` and `doc_sh`). The full `make test`
  with the sanitizer half is green on this code.
- **Re-run after the review fixes** (13:49-14:05): fuzz 610 s, 4,017,475 inputs, **0 crashes, 0
  time-outs, 0 out-of-memory**; `tests/soak.sh` 15 minutes, **37 rounds, 33,300 corpus runs, 0
  problems**; `make test` with the sanitizer half green; start-up `rsh -c nil` 1.97 ms against 1.91 ms
  for 0.1.4 (+3.1%); size 2,999,048 bytes.
- **Memory**: a 10,000-command session through a pseudo-terminal (six kinds of input in turn: a command,
  Ruby, `true`, a redirect, a capture, a block): RSS 2,960 KB after 1,000 commands, 3,592 KB after
  10,000 (+632 KB, history included; 0.1.4: +4,364 KB, and +6,140 KB for 10,000 `echo hi > /dev/null`).
  A script running 10,000 rounds of four command lines (in-process echo, a capture, a condition, a
  pipeline with a Ruby stage): RSS levels at 3.1 MB. `tests/run.sh` cli "M1" keeps the check.
- **Start-up with Sh** (final code, quiet box, load 1.0, 400 runs each, interleaved twice with 0.1.4):
  `rsh -c nil` median 2.04-2.06 ms against 1.97-1.98 ms (+3.5%, inside the 5% of the release gate), a
  one-line script 2.09 ms against 2.02-2.04 ms; the 10,000-command session again +632 KB. Size
  2,994,888 bytes (limit 3,145,728).
- **Start-up** before Sh (`tests/bench.sh`, 400 runs each, interleaved with 0.1.4): `rsh -c nil` median 1.99-2.68
  ms (0.1.4: 1.95-2.33 ms on the same busy box), a one-line script 1.95-1.98 ms (0.1.4: 1.85-1.92 ms);
  `bench/run.sh --gate` green. Size 2,982,600 bytes (limit 3,145,728).
