# Changelog

## 0.9.0

The release candidate for 1.0 (`docs/STABILITY.md`, sections 10-12): rsh as a script interpreter, as
`bash` runs `.sh` files. `docs/design.md` is now the specification (each rule names its test; the phase-1
design moved to `docs/design-history.md`). Every fix below has a test that fails on 0.1.4 and passes
here. How every test script and example is read did not change (`tests/frozen.explain`, the same digests
from the 0.1.4 binary).

### Fixes

- **G1** A script read from standard input (`rsh < x`, `rsh -s`) names its place in messages:
  `rsh: -:2: ls exited 2` (it was `rsh: ls exited 2`). `tests/run.sh` cli "G1" (2), `tests/contract.sh`
  "stdin script".
- **G2** `export NAME` for a name that is not set is a mistake that says what to write (status 1); it made
  an empty variable, so `FOO=1; export FOO` (FOO=1 is Ruby in rsh) exported nothing. `golden/r9_script.rsh`.
- **G3** `${NAME:-x}`, `${#NAME}` and the other sh forms of `${...}` are reader mistakes (status 2) that
  say to write `#{ENV.fetch("NAME", "default")}`; they failed only when the line ran, with a garbled name.
  cli "G3".
- **G4** `$?` in a later part of a line is the status of the part that just ran: `cmd || echo "$?"` showed
  the status from before the line. `golden/r9_spec.rsh` (S2.3).
- **G5** `answer = gets`, then a command reading the same standard input from a file: the command gets the
  rest (the interpreter's read-ahead is given back). A pipe cannot be given back (known limit K1). cli "G5".
- **G7** `source FILE WORDS` gives the file `ARGV = WORDS` while it runs (the words were dropped).
  `golden/r9_script.rsh`.
- **G9** `File.stat`, `File.lstat`, `File.ftype`, `File::Stat`, `File.readable?`, `writable?`,
  `executable?`, `owned?`, `Process.uid`, `euid`, `gid`, `egid`. `golden/r9_script.rsh`.
- **G10** A syntax error on a line that starts with a Ruby word that is also a program (`printf`, `open`,
  `format`, `test`, `exec`, `eval`, `send`, `method`, `trap`, `p`) says so and how to run the program
  (`$ printf ...`), in a run and in `-n`. cli "G10".
- **G11** A syntax error in a `source`d file names that file (it was `rsh: line 2: ...`). cli "G11".
- **G12** `require "time"` worked but added nothing; now there are `Time#iso8601`/`xmlschema`, `Time.iso8601`
  and `Time.parse` for ISO 8601 forms;
  another form is ArgumentError. `golden/r9_script.rsh`.
- **G13** The `require` message names every built-in library (json, set, time, etc). `golden/r9_script.rsh`.
- **M1** Memory of a long session: each interactive or `-i` input left its code string and an arena slot
  of the interpreter behind (about 250 bytes), and every command line typed was kept (about 600 bytes);
  a `source`d file's text stayed in the arena too. Now freed; a command line is dropped once its input
  ran unless code made by it (a method, a block, a proc) may run it later; history in memory is capped
  at 6,000 entries (the file keeps all). 10,000 commands: +632 KB (0.1.4: +4,364 KB). cli "M1".

### Tests and gates

- `tests/golden/r9_spec.rsh`: the specification's rules that had no golden case of their own.
- The examples of `docs/rsh.md` and of the manual page are tests (`tests/examples/doc_deploy.rsh`,
  `doc_man.rsh`, `tests/golden/doc_plain.rb`, with a `journalctl` stand-in), and `tests/run.sh` fails
  when a ```ruby block of `docs/rsh.md` is in no test, as it already did for `README.md`.
- `tests/frozen.explain` and its check in `tests/contract.sh`: `--explain` of every test script and
  example, file by file; the contract also checks that `VERSION`, `rsh -v`, `packaging.PKGBUILD` and
  `CHANGELOG.md` agree.
- `tests/soak.sh`: the sanitizer hour of the release gate (150 clean rounds for 0.9.0).
- `tests/fuzz/reader.cpp` and `make fuzz` (needs clang): the reader, the one-line parser and the
  interpreter's parser under libFuzzer with the address and undefined-behaviour sanitizers. The
  Makefile adds the interpreter's IO header to the include path (G5 reads its buffer); no new library.

### Packaging

- `packaging.PKGBUILD`: `pkgver=0.9.0`; the tarball sha256 is `SKIP` until the tag exists; the
  description says what rsh is for (a script interpreter).

### Review fixes (FIX-FIRST, during 0.9.0)

- `Sh.rm_rf` / `FileUtils.rm_rf` refuse `""`, `/`, `.`, `..`, a path ending in `/.` or `/..`, and any path
  that resolves to `/` or the current folder (ArgumentError, nothing removed). `golden/r9_sh.rsh`.
- `Sh.cp` / `FileUtils.cp` onto the same file (by name or a link) is ArgumentError instead of emptying
  it; a copy is written to a new file next to the destination and renamed over it. `golden/r9_sh.rsh`.
- `cmd &` with a pattern that matches nothing or a redirect that cannot open starts nothing and raises
  `Rsh::Failed` (the script stops, as in 0.1.4; `Rsh.last_pid` unchanged). `golden/r9_decisions.rsh`.
- The read-ahead of `source` and `require_relative` opens only plain files (never a FIFO or
  `/dev/stdin`). cli "D3".
- `cp_r` into itself or below itself is ArgumentError; `Sh.find`, `Find.find`, `Sh.du` and `cp_r` say
  an unreadable folder on standard error and go on, as find(1) (cli "S6"); Errno errors carry the
  system's text ("No such file or directory - x"); `Sh`, `FileUtils`, `Find`, `Shellwords` answer
  `respond_to?` before they are loaded and `require "fileutils"` (and the others) loads them;
  `Rsh::Failed#argv` of a D1 failure is the failing command's words (`["rm", "*.tmp"]`, `["cat", "-n"]`).
- `wait N` matches a pid only for N > 0 (an ended process no longer matches `-PID`); a pid wins over a
  job number, `%N` is always the job.
- `Shellwords.split` as Ruby's: a backslash before a new line keeps both, a lone final backslash stays,
  a backslash-new line inside double quotes is a new line.
- `STD_SHA` also covers `Sh.find`'s and `Sh.tmpdir`'s parameters, `String#shellescape`/`#shellsplit`,
  `Array#shelljoin`, `Dir.mktmpdir` and `Rsh.last_pid`; `tests/frozen.explain` covers `*.lib` files.

### Decisions (the CTO, during 0.9.0) and the lead's request

- **D1** A pattern with no match and a redirect that cannot open are a failed command with status 1, as a
  missing program is 127: `false` in a condition (`ls *.log 2>/dev/null || echo none`; a no-match is
  silent there, an open failure says why on standard error), `Rsh::Failed` outside one with the reason
  as its message. They were `Rsh::Error` even in a condition. `nil`, NUL and an Array inside a word stay
  `Rsh::Error`. `golden/r9_decisions.rsh`; `golden/glob.out` now shows `Rsh::Failed`.
- **D2** `Rsh.last_pid`: the pid of the last `cmd &` (`nil` before the first); `wait PID` waits for that
  process's job (`wait %N` for job N; a plain number is a pid first, then a job number: a pid wins).
  `golden/r9_decisions.rsh`, contract "Rsh.last_pid".
- **D3** A `source <literal path>` file is read ahead like `require_relative`, so its methods are Ruby at a
  bare call. A written-out path is now relative to the script's own folder, when read and when the line
  runs (it was the current folder at run time); a path made at run time (`#{}`, `$`, `~`, a pattern)
  keeps the current folder. Only a plain file is read ahead. `golden/r9_decisions.rsh` (with
  `r9_decisions.lib`, frozen in `tests/frozen.explain`), cli "D3" (a FIFO is never opened). Three older
  tests that sourced a file made in the current folder at run time now name it with `#{}`
  (`sec_c1_lines`, `sec_m8_load`, `r9_script`).
- **S1** `Sh`, built in and loaded on first use (start-up about 2 ms, as before): `find` `grep` `which`
  `exists?` `file?` `dir?` `age` `size` `du` `read` `write` `append` `lines` `mkdir_p` `touch` `cp` `cp_r`
  `mv` `ln_s` `chmod` `rm` `rm_rf` `tmpdir`; and the Ruby-standard `FileUtils` (options refused, never
  ignored), `Find.find`/`Find.prune`, `Shellwords` with `String#shellescape`/`#shellsplit` and
  `Array#shelljoin`, `Dir.mktmpdir`; `require` of `fileutils`, `find`, `shellwords`, `tmpdir` works. No
  program is started. `src/prelude_std.rb`, `Rsh.__touch` in `src/rt.cpp`; names frozen (contract
  "STD_SHA", `docs/design.md` S8); `golden/r9_sh.rsh` (each helper), `examples/doc_sh.rsh` (the example
  of `docs/rsh.md` and `rsh.1`, both checked line for line).

## 0.1.4

(Committed as 970a477 but never tagged: its `VERSION` and `packaging.PKGBUILD` still said 0.1.3. 0.9.0
is the next release; this section is the untagged step between.)

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
