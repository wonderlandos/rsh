# Changelog

## 0.1.4

QA-100 round 2 (audits `rsh.md` H-1..L-19 and `rsh-r2.md` R2-1..R2-20): the speed of Regexp loops, the
end of a script (signals, `at_exit`), a handful of reader mistakes, the interactive shell's typed-ahead
input, and the files a clean build chroot needs. Each correctness fix has a test that fails on 0.1.3
and passes here (`tests/golden/r2_*.rsh`, `tests/run.sh` cli cases, `tests/unit.cpp`,
`tests/interactive.rb`).

### Performance (H-1, M-10, M-5)

Measured by `bench/run.sh` (its `--gate` is part of `make test`): `scan(/\w+/)` on 686 KB 84.6 s -> 0.05 s,
`split(/ /)` 64 s -> 0.05 s, `gsub(/foo/, "F")` 9.7 s -> 0.007 s, a million `=~` 13.0 s -> 1.4 s,
a pattern per line over 100,000 lines 86 MB -> 17 MB, `sub(/re/)` in a Ruby stage 3.34 s -> 0.21 s,
1000 `echo` 0.14 s -> 0.009 s.

- **H-1** A subject is checked for valid UTF-8 once, not once per match (`PCRE2_NO_UTF_CHECK`); a
  subject that is not valid UTF-8 goes through a twin of the match on its valid pieces that gives the
  same answer as the engine (`tests/unit_regexp.cpp`, about 2 million random comparisons), so
  `"caf\xC3".scan(/./)` is still 3 characters. Patterns with `\A` or `\G` on such a subject keep the
  engine's own (slow) path. `tests/golden/regexp_semantics.rsh`.
- **M-10** One shared match-data block, a 16-entry cache of compiled patterns (a literal `/re/` is
  rebuilt by the interpreter on every evaluation), no per-call Arrays for `match?` and `=~`.
- **M-5** `echo`, `pwd` and `type` run in this process when alone on a line and not captured (a
  captured one keeps its own process: a long output would fill the pipe), and a failed write is
  `rsh: echo: write error: ...` with status 1 (`tests/golden/r2_echo_full.rsh`).

### Correctness

- **H-2, R2-13** `at_exit`, `trap` (`Signal.trap`, `"IGNORE"`, `"DEFAULT"`, `trap("EXIT")`), `Signal`.
  TERM, HUP and INT are noted when they come and acted on at the next safe point (a command line, an
  iteration of `loop`, the end of `sleep`): a trap block runs, or `SignalException` / `Interrupt` is
  raised so `ensure` and `at_exit` run, and rsh ends by the signal (143, 129, 130). Code with no safe
  point is ended by the signal after 2 seconds. The interactive shell handles TERM the same way and
  Ctrl-C interrupts Ruby code. TERM or HUP that comes while a foreground command runs is passed on to
  that command, so the script ends at once (not after the 2-second watchdog) with `ensure` and `at_exit`
  run. A signal ignored by the parent (`nohup`) stays ignored
  (`golden/r2_signals.rsh`, `run.sh` cli cases, `interactive.rb`).
- **H-3, L-5** `Dir.home`, `Dir.home("user")`, `Process.ppid`, `$PROGRAM_NAME` (`golden/r2_ruby_bits.rsh`).
  The Ruby side of these loads on first use (`src/prelude_more.rb`), so `rsh -c nil` does not pay for it.
- **M-1** `echo {a,b}`: a brace pattern right after the command word is a word. **M-4** `! cmd` is a
  command line (negation). Both change the reader's rule for those lines only; every line that was
  there reads as before, and `EXPLAIN_SHA` in `tests/contract.sh` was printed again for the lines added to
  `tests/golden/classify.rsh` (the two reader fixes and R2-4, R2-7 below).
- **M-2** A `|` at the end of a line continues on the next line; at the end of the file it is
  "a command is missing after |". **M-3** In an unquoted here-document a backslash goes only before
  `$`, a back-quote, `\` and a new line (it ate every backslash). **M-8** `if x then echo a end`: the
  `end` closes the Ruby construct. (`golden/r2_reader.rsh`, `heredoc.rsh`, `tests/unit.cpp`)
- **M-6** `Process.kill` knows every signal name of the system and a Symbol; an unknown name is
  `ArgumentError` and nothing is sent (it sent signal 0 and said it had).
- **M-7, R2-15, R2-16** Tab completion quotes a name so it stays one word (space, quote, `$`, `*`,
  `#`, a tab ...), completes inside an open quote, and offers commands after `|`, `;`, `&&`, `||` and `(`.
- **M-9** `history` lists the earlier sessions' entries and this one's. **M-11** `rsh DIR` is `Is a
  directory`, 126; a path that exists but cannot run is 126, not 127 (`tests/contract.sh`).
- **M-12** `# rsh: WORD` with another word than `ruby` or `shell` is status 2 ("unknown mode").
- **R2-1** The first program of a foreground job takes the terminal in the child before it runs
  (fork instead of posix_spawn under job control): a program that reads at once is no longer stopped by
  SIGTTIN (`interactive.rb`; a race, 1 of 20 sessions on the audit machine's 0.1.3 binary, 15 of 20 on
  the auditor's).
- **R2-2, R2-8, L-17** The editor keeps typed-ahead and pasted input (`TCSADRAIN`, not `TCSAFLUSH`) and
  draws once per burst: a 5,000-character paste wrote 12.8 MB to the terminal before, under 50 KB now.
- **R2-3, R2-12** `-n` and `--lint` warn about a bare word alone in a group (`x = 1 + (y)`) and a
  command whose first argument is a Ruby operator (`to_der == other`); `--lint` exits 1 for them. The
  rule itself is unchanged (a documented behaviour that real scripts trip on: a decision for the lead).
- **R2-4** `Point=Struct.new(:x) do`, `Limit=10; puts Limit`: a Capitalised `NAME=` followed by Ruby is
  Ruby; `NAME=value cmd` stays a command line. **R2-7** after `&&` or `||` the Ruby words `exit raise
  abort return break next` and `:` end the command line (`cmd || exit 1` exits 1). **R2-17** `X=~ cmd`
  stays Ruby and is documented.
- **R2-5, R2-6** CR LF is read as LF and a leading UTF-8 byte-order mark is dropped, so here-documents,
  `\` continuations and `# rsh: ruby` work in files saved on Windows.
- **R2-9, R2-10** Ctrl-R then an arrow or Ctrl-A leaves the search and does the move; ESC then Enter is
  Enter. **R2-11** `gets`, `readline`, `readlines`, `each_line`, `File.foreach` and `File.readlines` take
  `chomp: true`. **R2-18** `--explain` exits 2 for Ruby that does not compile, as `-n` does.
  **R2-19** `exit` or Ctrl-D with jobs running warns once. **R2-20, L-16** sh forms (`[ -f x ]`,
  `x=$(cmd)`, `set -e`, `. file`, `true && cmd`) are reader errors that say what to write.
- Lows: **L-3** `$$` is the process id; **L-7** `echo -e` knows `\xHH`, `\0NNN`, `\a \b \f \v`;
  **L-8** `-c`, `-e`, `--name` without a value say what is missing; **L-9** an `Rsh.eval` syntax error names
  its own text (`(rsh eval):1:`); **L-12** a NUL byte in the source is named; **L-13** `wait N` for no
  such job says so; **L-15** `--lint` warns about a fixed `/tmp` name as a redirect target.
- Decided, not changed: **L-1** the one-line `cd d do pwd end` keeps its message; **L-4** `ENV["a b"] =` is
  accepted, as Ruby does; **L-6** `pwd` shows the physical folder; **L-10**, **L-11**; **L-18** SIGPIPE
  of a program is a failed command (listed in `docs/rsh.md`, "Not done yet"); **R2-14** the missing
  editor keys and the interpreter gaps are listed there too.

### Packaging (from the clean-chroot build of the `rsh` package, PKG-image)

- `packaging.PKGBUILD`: `makedepends` gains `ruby-rake` (the interpreter's `minirake` runs `rake`), and
  `checkdepends=(procps-ng)` (`tests/golden/sec_m5_jobs.rsh` counts zombies with `ps`).
- `tests/interactive.rb` completes `/etc/os-release` instead of `/etc/hostname` (a build chroot has no
  machine name); the `prepare()` `sed` in wolos-pkgbuilds' `rsh/PKGBUILD` is then not needed.
- The Makefile builds `src/prelude_more.rb` into the program like the other two preludes.

## 0.1.3

Security and stability: every finding of the 2026-11-04 audit is fixed, each with a regression test
built from its reproducer (`tests/golden/sec_*.rsh`, `tests/unit.cpp`, `tests/interactive.rb`), and
the stability contract is written down (`docs/STABILITY.md`) and checked (`tests/contract.sh`).

- **C1** A command line runs from its own copy of the parsed line, never from a reference into the
  table that a Ruby stage, `source`, `system` or `Rsh.eval` can grow while it runs; `system` and
  `Rsh.run` use a private line and no longer remove another file's last line. Before: use after free,
  "no such command line", a crash, or a line number reused by another file
  (`tests/golden/sec_c1_lines.rsh`).
- **C2** No text reaches `/bin/sh`: `open("|...")` and `IO.popen` (`IO._popen`) are `Rsh::Error` with
  the rsh way in the message; the interpreter is built with `MRB_NO_IO_POPEN`
  (`sec_c2_no_shell.rsh`).
- **H1** `umask` takes 1 to 4 octal digits; anything else is an error (status 2) and the mask stays
  (before: `umask u=rwx,go=rx` set 0000) (`sec_h1_umask.rsh`).
- **H2** A NUL byte in an argument, an Array element, a `NAME=` value or a redirect target is an
  `Rsh::Error` (before: the argument was cut short) (`sec_h2_nul.rsh`, unit).
- **H3** As root, or started set-user-id/set-group-id, the interactive shell takes the home from
  `/etc/passwd`, ignores `$HOME`/`$XDG_*` for init.rsh and history, and refuses group-writable files;
  init.rsh and history are always opened without following links and used only when owned by the
  effective user and not world-writable (`tests/interactive.rb`, unit).
- **M1** The interpreter's other Kernel functions are Ruby words: `open putc readline readlines
  __dir__ __callee__ global_variables local_variables usleep tap itself object_id enum_for to_enum
  yield_self methods singleton_methods public_methods private_methods protected_methods
  remove_instance_variable singleton_method`; `__dir__` exists. The list is frozen
  (`sec_m1_words.rsh` with `.explain`).
- **M2** A file found by a pattern whose name starts with `-` is given as `./-name`; `--lint` warns
  about a value as the first argument of a program (`sec_m2_dash.rsh`, contract).
- **M3** File names in Tab completion, the current folder in the prompt, history in Ctrl-R and a
  recalled line are shown with control characters as `^X` and C1/invalid bytes as `\xNN`
  (`printable`, unit and `tests/interactive.rb`).
- **M4** History: a line that starts with a space or looks like it holds a secret is not kept; the
  file is opened with `O_NOFOLLOW`, must be the user's own, and an older file is set to 0600; a bad
  file or folder turns the file off for the session with a message (unit, `tests/interactive.rb`).
- **M5** Background jobs of a script are reaped before each command line (no zombies); `wait` still
  gives an ended job's status; `fg`/`bg` without job control or a process group are refused instead
  of signalling rsh's own group (`sec_m5_jobs.rsh`).
- **M6** `NAME=v exec prog` passes `NAME` to `prog` (`execve` with the line's variables)
  (`sec_m6_exec.rsh`).
- **M7** Pattern matching has one match context: match limit 1,000,000, depth limit 1,000,000,
  heap 256 MB; past them `RegexpError` "too complex" in milliseconds (before: about 0.1 s per call,
  then a RuntimeError) (`sec_m7_regexp.rsh`).
- **M8** Loaded text (`Rsh.eval`, `require_relative`, `load`, `source`) runs under a protect, so an
  exception no longer skips rsh's clean-up: the 91-byte leak is gone and the list of files being
  loaded stays right. `make test` now runs everything again on a sanitizer build (`make asan`:
  `LINK=dynamic`, `-fsanitize=address,undefined`, leak check on) (`sec_m8_load.rsh`,
  `meta_eval.rsh` under the sanitizer). An error in a sourced file is reported as
  `rsh: FILE:LINE: message (Class)`.
- **L1** A `{a,b}` pattern stops at 100,000 words with an error (command lines and `Dir.glob`).
- **L2** `**` never follows a symbolic link, also where the file system gives no file type.
- **L3** An empty `PATH` part is skipped, not read as the current folder.
- **L4** `unset` checks the name (`unset A=b` is status 2).
- **L5** An Array in a `NAME=` value or a here-document is an error (join it), as inside a word.
- **L6** `File.expand_path("~user")` reads `/etc/passwd` (no C library account lookup in the static
  program).
- **L7** CI actions are pinned to a commit.
- **L8** The shipped link mode is written down: `LINK=static` by default and for the CI artifact,
  `LINK=dynamic` in the WolOS package (`docs/STABILITY.md`, `docs/design.md`).
- **L9** The line editor counts display columns (wide characters 2, combining marks 0).
- **L10** `cd`, `pwd`, `pushd` have no 4096-byte limit and say "the current folder was removed"
  instead of giving an empty folder; `cd` from a removed folder keeps the old `OLDPWD`.
  (L1-L5, L10: `sec_lows.rsh`; L9: unit; L7: contract.)

Found while fixing (not in the audit):

- `Regexp.new(...)` as the first pattern of a script read its arguments after loading the Regexp
  side, which reuses the call's argument slots: a later `rescue` could crash (0.1.2 segfaulted on
  `begin; Regexp.new("^(a+)+$") =~ "aaa...b"; rescue => e; end`). The arguments are read first now.
- The interpreter's default cap of 131,072 Array entries is off (`MRB_ARY_LENGTH_MAX=0`):
  `File.readlines` of a longer file, or a longer `map`, failed with "array size too big".
- `rsh -h` lists `-h` and `--`.

The contract (`docs/STABILITY.md`): the classification rule and `RUBY_WORDS`, the `--explain`
format, the flags, the exit statuses (`--lint`: 0 clean, 1 warnings, 2 errors), the message format,
value safety, the file names and modes, the build inputs and the release gate. `tests/contract.sh`
holds digests of the frozen texts and fails when one changes.

Measured (the 32-core build box with other jobs running, static, a fork-and-wait timer, 400
interleaved runs per round, nine rounds): `rsh -c nil` median 1.11-1.22 ms against 1.10-1.23 ms for
0.1.2, a difference per round of -0.014 to +0.030 ms (within the noise; 0.1.2 was 1.16 ms); size
2,937,160 bytes (0.1.2: 2,928,744). `make test` with the sanitizer run takes about 30 s.

## 0.1.2

- The README examples are tests: each ```` ```ruby ```` block of `README.md` is in a
  `tests/golden/readme_*.rsh` with its expected output, run with stand-ins for `curl`, `ssh`, `nft`,
  `make`, `df` and `sleep` (`tests/fakebin/readme/`), and `tests/run.sh` fails when a block of the
  README is in none of them. A `NAME.setup` file next to a test makes the files it works on.
- `String#[]` and `String#slice` take a Regexp (`line[/\d+/]`, `line[/(\w+) (\d+)/, 2]`, a group
  name); before, a TypeError. `File.mtime` exists. `case ... when /re/` sets `$~` and `$1`.
- Start-up: the Ruby side of Regexp (MatchData, `Regexp.escape`, the String methods that take a
  pattern) loads the first time a pattern is made or one of those methods is called, not at
  start-up. `rsh -c nil` is back at the 0.1.0 time (median 1.16 ms against 1.16 ms for 0.1.0 and
  1.20 ms for 0.1.1, 400 interleaved runs); a script that makes a pattern pays for the load then
  (about 0.05 ms more than 0.1.1 for a one-line `=~`).
- `tests/golden/core_words.rsh` (`warn`, `abort`, `File.mtime`, `String#[]` with a Regexp),
  `tests/golden/regexp_lazy.rsh` (the methods before and after the Regexp side loads).
- README: the install lines name the same tarball in both commands.

## 0.1.1

- `.rb` files have the shell layer, like `.rsh` files: Ruby and command lines mix the same way.
  `# rsh: ruby` as the first line (or the line after `#!`) turns it off for a plain Ruby file.
  This replaces the 0.1.0 rule (`.rb` plain unless `# rsh: shell`).
- Metaprogramming: `eval`, `binding`, `instance_eval`/`class_eval` with a string and `ObjectSpace`
  are built in. `Rsh.eval(text[, binding])` runs a text with the shell layer; `eval` stays plain
  Ruby. `Rsh.translate(text)` shows the Ruby it runs.
- The reader knows methods made with a literal name (`define_method :x`, `attr_reader :x`,
  `alias_method`, `Struct.new(:x)`, `Data.define(:x)`), and new declarations for the others:
  `rsh_methods :name, ...`, `ruby do ... end` / `ruby { ... }`, and `ruby!` for the rest of the
  file. `--explain` lists the known methods and the Ruby-only lines. Metaprogramming words
  (`send`, `class_eval`, `prepend`, ...) and conversions (`to_s`, `to_a`, ...) are Ruby words.
- Fixes: a top-level `def` after `SomeClass.class_eval("...")` no longer lands in that class;
  `def <=>(o)` and other operator methods, lambda parameters `->(x) { x }`, a `(` at the start of a
  statement and `#{name}` in a command line are read as Ruby; `String#sub`/`gsub` with a string
  pattern and a block; `warn`, `abort`, `private_method_defined?`, `public_method_defined?`,
  `Data#with` exist.
- `tests/golden/meta_*`, `examples/meta/` (a deploy language made with `method_missing`, a task
  runner made with `define_method`).

## 0.1.0

First release of rsh as its own repository (it was a directory of the WolOS repository before).

- Ruby with a shell layer: a line whose first word Ruby does not know is a command line, with
  pipes, redirects, globs, here-docs, `&&` and `||`. A Ruby value in a command is one argument;
  nothing goes through `sh`. A failing command raises `Rsh::Failed`; `relaxed!` turns that off.
- `rsh file.rsh` and `#!/usr/bin/rsh` scripts, `-e`/`-c`, `-s`, `-n`, `--lint`, `--explain`,
  `--ruby`.
- `rsh file.rb` runs a plain Ruby file with the shell layer off, so a Ruby script behaves as
  Ruby. A `.rb` file opts in to the shell layer with `# rsh: shell` as its first line (or the
  line after a `#!` line). `--explain` prints `mode: shell` or `mode: ruby`.
- Interactive shell: line editing, completion, jobs, history, coloured prompt; `-l` as login shell.
- One static binary under 3 MB. `make install PREFIX=...` installs the program and its manual page.
- CI: build, `make test`, size check, binary uploaded as an artifact.
