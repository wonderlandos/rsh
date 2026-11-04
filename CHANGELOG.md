# Changelog

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
