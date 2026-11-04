# rsh specification (1.0 candidate, 0.9.0)

This is what rsh does with a script. It is the reference for 1.0 (`docs/STABILITY.md`, section 11): a
rule here changes only in a major version. Every rule names the tests that hold it; `tests/run.sh`
runs the golden scripts (standard output plus `status: N`, and standard error where a `.err` file is
given), `tests/contract.sh` the frozen items, `tests/frozen.explain` how every test script is read.
The longer description with the reasons is `docs/rsh.md`; the design history (the options weighed in
phase 1, the early questions) is `docs/design-history.md`.

rsh runs scripts, as `bash` runs `.sh` files. The interactive mode (`rsh` on a terminal, `-i`) uses the
same rules for each input; it is described in `docs/rsh.md` and is not a login shell.

## S1. Reading a file

**S1.1 A file is Ruby with command lines.** Every file has the shell layer whatever its name (`.rsh`,
`.rb`, none), unless its first line, or the line after a `#!` line, is `# rsh: ruby` (then the file is
plain Ruby and no line is a command line). `# rsh: shell` there says the default; another word there is
a mistake (status 2). A leading UTF-8 byte-order mark is dropped and CR LF is read as LF. The same holds
for `-c`, `-e`, `-s` and a file loaded with `source`, `require_relative` or `load` (by its own first line).
Tests: `golden/ruby_*.rb`, `golden/r2_bom*.r*`, `golden/r2_crlf.rsh`, cli "M-12", "-n reads a # rsh:
ruby file".

**S1.2 Statement starts.** Only the start of a statement is classified: the start of the file, after a
new line or `;` that ends a statement, after `then`, `do`, `else`, `begin`, `ensure`, after `{` or `do`
with its `|params|`, after a grouping `(`, and after `if unless while until elsif`. Strings, `%w[]`
literals, here-documents, regular expressions and brackets are skipped as Ruby skips them.
Tests: `golden/classify.rsh` with `classify.explain`, `tests/unit.cpp`.

**S1.3 The rule.** At a statement start the line is a command line when all of these hold, else it is
Ruby:
1. the first word is a plain word, or starts with `./`, `../`, `/` or `~` (a path is always a command);
2. it is not a Ruby keyword;
3. it is not a local variable assigned earlier in scope;
4. it is not a method the file makes in a way the reader sees (S1.4), not one of the Ruby words
   (`RUBY_WORDS` in `src/transpile.cpp`, frozen: `docs/STABILITY.md` section 1), and not Capitalised;
5. the next character is not one of `. ( [ :: , = += -= ||= &&= ? !` glued to the word, and it is not
   ` = `.

The rule reads only the text, never `PATH` or the machine. Tests: `golden/classify.rsh`,
`golden/core_words.rsh`, `golden/sec_m1_words.rsh` (with `.explain`), contract "WORDS_SHA",
"EXPLAIN_SHA", `tests/frozen.explain`.

**S1.4 Methods the reader sees**, anywhere in the file or in a `require_relative "<literal>"` file or
a `source <literal path>` file (relative to the script's own folder, when read and when run, never the
current folder; only a plain file; for `-c` and standard input the current folder; a path
with `#{}`, `$`, `~` or a pattern is known only at run time):
`def name` (also `def self.name`, `private def name`); a literal name after `define_method`,
`define_singleton_method`, `alias_method`, `alias`, `attr_reader`, `attr_accessor`, `attr_writer`,
`Struct.new(...)`, `Data.define(...)`; and `rsh_methods :name, ...`. Tests: `golden/meta_*.rsh`,
`golden/readme_meta.rsh`, `examples/meta/*.rsh`, `golden/r9_decisions.rsh` (D3).

**S1.5 Escape hatches.** `$ cmd` forces a command line; `name(...)` and `self.name` force Ruby; `ruby do
... end` / `ruby { ... }` make every statement inside Ruby, `ruby!` alone on its line does it to the end
of the file (in both, `$ cmd` and back-quotes still run programs); `NAME=value cmd ...` is a command line
with a variable for that command, `x=1` alone and `Name=Ruby...` are Ruby; `! cmd` is a command line
with its status turned round. Tests: `golden/classify.rsh`, `golden/meta_ruby_block.rsh`,
`golden/r2_reader.rsh`, `golden/r9_spec.rsh` (S2.5, S2.8).

**S1.6 sh forms are reader mistakes that say what to write** (status 2, nothing runs): `[ ... ]`,
`$(...)`, `set -e`, `. file`, `true && cmd`, `$1`..`$9`, `$#`, `$@`, and `${...}` with anything but a
variable name (`${NAME:-x}`). Tests: `golden/r2_sh_hints.rsh`, cli "G3".

## S2. Inside a command line

**S2.1 Words and quotes.** `'text'` is literal; `"text #{ruby}"` interpolates Ruby (not `$(...)`); `\x`
takes one character as it is; pieces glue into one word (`"a"'b'c`). An unquoted ` #` starts a comment.
Tests: `golden/basics.rsh`, `golden/r9_spec.rsh` (S2.1).

**S2.2 One value, one argument.** `#{expr}` is exactly one argument whatever it holds; an Array is one
argument per element (and must be a whole word); `nil` is an error; a NUL byte in an argument, an
Array element, a `NAME=` value or a redirect target is an error. A value is never split again and never
read as a pattern. Tests: `golden/r9_spec.rsh` (S2.2), `golden/sec_h2_nul.rsh`, contract "6. Value
safety".

**S2.3 Variables.** `$NAME` and `${NAME}` come from the environment (one argument; unset is an error that
names `ENV.fetch`); `$?` is the status of the part of the line that ran last (the line before, at the
start of a line); `$*` is every script argument, one word each; `$!` the last background process;
`$$` rsh's process id. The `#{...}` values of a line are all read once, when the line starts. Tests:
`golden/r9_spec.rsh` (S2.3), `golden/sec_lows.rsh`.

**S2.4 Tilde and patterns.** `~`, `~/x`, `~user` (from `/etc/passwd`). On unquoted text `* ? [a-z] {a,b}
**`; no match is a failed command, status 1 (S3; `Rsh.nullglob = true`: no words); a file found whose
name starts with `-` is given as `./-name`. Tests: `golden/glob.rsh`, `golden/r9_spec.rsh` (S2.4),
`golden/sec_m2_dash.rsh`, `golden/r9_decisions.rsh` (D1).

**S2.5 `NAME=value cmd`** sets NAME for that command only. Tests: `golden/r9_spec.rsh` (S2.5),
`golden/sec_m6_exec.rsh`.

**S2.6 Pipelines.** `a | b`, `a |& b` (standard error too), a Ruby stage `a | { |line| ... } | b` (a
String replaces the line, `nil` or `false` drops it, another value keeps it). The status is the last
stage that failed (pipefail); `$?.statuses` has every stage; a writer ended by SIGPIPE before the last
stage does not fail the pipeline. A `|` at the end of a line continues on the next. Tests:
`golden/pipes.rsh`, `golden/readme_pipeline.rsh`, `golden/r9_spec.rsh` (S2.6, S2.8).

**S2.7 Redirections and here-documents.** `> >> < 2> 2>> 2>&1 &> <<<`, `N>` for another descriptor, in
the order written; `> #{path}` takes one word. `<<EOF` and `<<~EOF` take `#{}` (a backslash only before
`$`, a back-quote, `\` and a new line), `<<'EOF'` is literal; body lines are never classified. Tests:
`golden/redirects.rsh`, `golden/heredoc.rsh`, `golden/r9_spec.rsh` (S2.7). A file that cannot be opened
is a failed command, status 1 (S3).

**S2.8 Lists.** `a && b`, `a || b`, `a ; b`, `a &` (background). After `&&` or `||` the next word is a
command again, except the Ruby words `exit raise abort return break next` and `:`, which end the
command line (`cmd || exit 1`). `\` at the end of a line continues it. Tests: `golden/lists.rsh`,
`golden/r2_exit_after_or.rsh`, `golden/r9_spec.rsh` (S2.8).

**S2.9 Captures.** `` `cmd args` `` and `%x{...}` use the same reader (never `/bin/sh`) and give the
output as a String; `$?` is set. A capture built at run time is an error. Tests: `golden/basics.rsh`,
`golden/r9_spec.rsh` (S2.9), `golden/sec_c2_no_shell.rsh`.

**S2.10 A command line is a value**: `true` or `false` by its status (`ok = (git diff --quiet)`).
Tests: `golden/r9_spec.rsh` (S2.10), `golden/pipes.rsh`.

## S3. Failure

A command line that fails and is not a condition (`if`, `unless`, `while`, `until`, left of `&&` or
`||`, a value in brackets) raises `Rsh::Failed` (`status`, `argv`, `stderr`, the tail of the error
output). Uncaught, the script ends with `rsh: FILE:LINE: PROG exited N` (or `was stopped by signal N`)
and that status (128+N for a signal). `|| true` ignores one failure; `relaxed!` turns raising off to
the end of the script (`relaxed!(false)` back on); `$?` is an `Rsh::Status` either way. A pattern with no
match and a redirect that cannot open are a failed command with status 1, as a program that is not
found is 127 (`rsh: PROG: command not found`) and one that cannot run 126: `false` in a condition (a
no-match says nothing there, an open failure says why on standard error), else `Rsh::Failed` whose
message is the reason (`rsh: FILE:LINE: no file matches *.tmp`). A mistake in the line itself (a `nil`
value, a NUL byte, an Array inside a word) is `Rsh::Error`, status 1. A script
that ends normally is status 0. Tests: `golden/failed.rsh`, `golden/readme_errors.rsh`,
`golden/sec_lows.rsh`, `golden/r9_decisions.rsh` (D1), contract "4. Exit statuses" and "5. Message
formats".

## S4. Built-in commands

`cd pushd popd dirs export unset exec umask jobs fg bg wait source echo type pwd history` (the list
`BUILTINS` in `src/exec.cpp`, frozen). `cd` with no folder goes home, `cd -` to the previous folder
(printed), `cd dir do ... end` comes back afterwards. `export NAME=value` sets the environment; `export
NAME` keeps a set NAME and is a mistake for one that is not set. `unset` checks the name. `umask` takes
octal digits. `exec prog` replaces rsh (with only redirects it applies them to rsh). `source FILE WORDS`
runs FILE in this process with ARGV = WORDS while it runs. `wait` waits for background jobs (`wait PID` for the job
of that process, `wait %N` for job N, a plain number a pid first, else a job number; a job that is not there is 127). `Rsh.last_pid` is the pid of the
last `cmd &` in Ruby, `nil` before the first. `echo`, `pwd` and `type` alone on a line run without a fork;
a failed write is `rsh: NAME: write error: ...`, status 1. `fg` and `bg` need the interactive shell.
`system("prog", "arg")` and `Rsh.run("prog", "arg")` take separate arguments, never one string. Tests:
`golden/r9_spec.rsh` (S3, S4), `golden/r9_script.rsh` (G2, G7), `golden/r9_decisions.rsh` (D2), `golden/sec_h1_umask.rsh`,
`golden/sec_m5_jobs.rsh`, `golden/sec_m6_exec.rsh`, `golden/r2_echo_full.rsh`, contract "BUILTINS_SHA".

## S5. Running

**S5.1 Ways to start.** `rsh FILE ARGS`, `./FILE` with `#!/usr/bin/rsh` or `#!/usr/bin/env rsh`, `rsh -c
TEXT ARGS` (`-e` the same), `rsh -s [--name FILE] -- ARGS` and `rsh < FILE` (the script from standard
input, read whole before it runs), `rsh -n` and `--lint` (check only), `--explain`, `--ruby`. ARGV holds
the arguments, `$0` and `__FILE__` the script's name (`-c` for `-c`, `-` for standard input). Flags and
the `-h` text are frozen (`docs/STABILITY.md` section 3). Tests: contract "3. Flags", cli cases in
`tests/run.sh`.

**S5.2 Exit statuses**: `docs/STABILITY.md` section 4. Tests: contract "4. Exit statuses".

**S5.3 Signals.** `trap`, `Signal.trap`, `at_exit`, `trap("EXIT")`. TERM, HUP and INT are acted on at
the next safe point (a command line, an iteration of `loop`, the end of `sleep`): a trap block runs, or
`SignalException`/`Interrupt` is raised so `ensure` and `at_exit` run, and rsh ends by that signal. TERM
or HUP while a foreground command runs is passed to it. Code with no safe point is ended after 2 seconds.
A signal ignored by the parent stays ignored. Tests: `golden/r2_signals.rsh`, `golden/r2_sigexc.rsh`,
cli "H-2" cases.

**S5.4 Messages**: one line on standard error starting `rsh: `, with `FILE:LINE:` when there is a place
(`docs/STABILITY.md` section 5). Tests: contract "5. Message formats" and "stdin script", cli "G1",
"G11", "L-9".

## S6. Safety

No text ever reaches `/bin/sh`: command lines are parsed once, when the file is read; `open("|...")`,
`IO.popen` and a run-time back-quote are `Rsh::Error`, and the interpreter is built without popen.
Tests: `golden/sec_c2_no_shell.rsh`, contract "no /bin/sh".

## S7. The Ruby inside

The interpreter built into rsh, with what `docs/rsh.md` ("What is inside") lists. `require` knows
`json set time etc fileutils find shellwords tmpdir` (built in); your own files load with `require_relative`. Added by rsh for scripts: `File.stat`,
`File.lstat`, `File.ftype`, `File::Stat`, `File.readable?`, `writable?`, `executable?`, `owned?`,
`File.mtime`, `Process.uid euid gid egid ppid kill`, `Dir.home`, `Time#iso8601`, `Time.parse` and
`Time.iso8601` (ISO 8601), JSON, Regexp, `ENV` in step with the process. Tests: `golden/r9_script.rsh`,
`golden/r2_ruby_bits.rsh`, `golden/regexp_*.rsh`, `golden/readme_json.rsh`.

## S8. Sh and the standard helpers

Built in, no `require`, loaded on first use: `Sh` (`find grep which exists? file? dir? age size du read
write append lines mkdir_p touch cp cp_r mv ln_s chmod rm rm_rf tmpdir`), `FileUtils` (`mkdir_p makedirs
mkpath mkdir rmdir rm rm_f rm_r rm_rf rmtree cp cp_r mv ln_s touch chmod pwd`; options are an
ArgumentError), `Find` (`find prune`), `Shellwords` (`escape split join shellescape shellsplit
shellwords shelljoin`, with `String#shellescape`, `#shellsplit`, `Array#shelljoin`) and `Dir.mktmpdir`.
They start no program and give Ruby values; `rm_rf` refuses `""`, `/`, `.`, `..` and what resolves to
`/` or the current folder, `cp` refuses the same file, `cp_r` refuses a copy into itself, and a walk
reports an unreadable folder and goes on; their meaning is `docs/rsh.md`, "Sh". The names are frozen:
`tests/contract.sh` "STD_SHA". A command line keeps its meaning (`find . -name x` is the program). Tests:
`golden/r9_sh.rsh` (each helper), `examples/doc_sh.rsh` (the documented example).
