# rsh 1.0 compatibility

rsh 1.0.0 (2026-11-05) freezes the language, the command line, the exit statuses, the messages and the
`Sh` library. This page says what that covers, what is deprecated (nothing), and what a 1.x release may
add. The reasons and the tests are in [design.md](design.md) (the specification, S1-S8, every rule names
its test) and [STABILITY.md](STABILITY.md) (the contract and its numbers); where they and this page
differ, they win and this page is wrong. `tests/contract.sh` and `tests/frozen.explain` fail when a frozen
item changes; `make test` runs them.

## 1. The promise

- **1.x.y** changes rsh only to fix a mistake (behaviour that contradicts design.md or this page), for a
  serious security problem, for a large speed-up, or to add something section 5 allows. A script that
  works on 1.0.0 works on every 1.x with the same output, the same statuses and the same messages (the
  wording after the place in a message may be made clearer, section 3).
- **2.0** is the only release that may change or remove anything listed in sections 2 to 4.
- A change to a frozen item that is a security fix goes in a 1.x only when no other fix exists, is said
  first in the CHANGELOG under "Security", and names the replacement.
- rsh is a script interpreter, as `bash` runs `.sh` files. The interactive mode (`rsh` on a terminal, `-i`)
  uses the same rules for each line and keeps its tests green, but its editing, completion, colours,
  history search and prompt are not part of the promise; they may change in a 1.x. rsh is not a login shell.

## 2. The language (design.md S1-S3)

Frozen:

| area | what |
|---|---|
| a file | Ruby with command lines in every file, `.rsh`, `.rb` or none; `# rsh: ruby` as the first line (or after `#!`) turns the layer off, `# rsh: shell` says the default, another word is status 2; UTF-8 BOM dropped, CR LF read as LF |
| statement starts | the places where a statement can start (S1.2) and nothing else is classified; strings, `%w[]`, here-documents, regexps and brackets are skipped as Ruby skips them |
| the rule | a command line when the first word is a plain word or a path (`./ ../ / ~`), is not a Ruby keyword, not a local assigned earlier, not a method the reader sees (S1.4), not one of the Ruby words, not Capitalised, and is not followed by `. ( [ :: , = += -= \|\|= &&= ? !` glued to it (S1.3) |
| the Ruby words | the list `RUBY_WORDS` of `src/transpile.cpp`; frozen, grows only in a major version |
| methods the reader sees | `def`, `define_method`, `define_singleton_method`, `alias_method`, `alias`, `attr_*`, `Struct.new`, `Data.define` with literal names, `rsh_methods`, in the file and in `require_relative "<literal>"` and `source <literal path>` files |
| escape hatches | `$ cmd`, `name(...)`, `self.name`, `ruby do ... end`, `ruby { }`, `ruby!`, `NAME=value cmd`, `! cmd` |
| builtins | `cd pushd popd dirs export unset exec umask jobs fg bg wait source echo type pwd history`; `cd dir do ... end` |
| sh forms that are errors | `[ ]`, `$(...)`, `set -e`, `. file`, `true && cmd`, `$1`..`$9`, `$#`, `$@`, `${NAME:-x}` and other `${...}` are reader mistakes (status 2) that say what to write. They stay errors in every 1.x: no 1.x gives them a meaning |
| quoting | `'text'`, `"text #{ruby}"`, `\x`, glued pieces, an unquoted ` #` starts a comment |
| values | `#{expr}` is one argument whatever it holds; an Array is one argument per element and must be a whole word; `nil`, a NUL byte, an Array inside a word are errors; a value is never split or read as a pattern |
| variables | `$NAME`, `${NAME}` (environment; unset is an error), `$?`, `$*` as a whole word, `$!`, `$$` as words of a command line |
| tilde, patterns | `~`, `~/x`, `~user` (an unknown user is an error, not the literal text); `* ? [a-z] {a,b} **`; a match starting with `-` is given as `./-name`; no match is a failed command, status 1 |
| pipelines | `\|`, `\|&`, a Ruby stage `{ \|line\| ... }`, pipefail with `$?.statuses`, `\|` at a line end continues |
| redirections | `> >> < 2> 2>> 2>&1 &> &>> <<< <<EOF <<~EOF <<'EOF'`, `N>` `N>>` `N<` for one digit N, `>\|` as `>`; in the order written |
| reserved, an error today | a descriptor of two digits (`10>f`), `<&-`, `>&-`, `N<<<`, `{fd}>f`: reader mistakes, status 2. A 1.x may give them a meaning (section 5) |
| lists | `&& \|\| ; &`; after `&&`/`\|\|` the next word is read as at a statement start; `\` continues a line |
| captures | `` `cmd` `` and `%x{}` with the same reader, never `/bin/sh`; a failed capture raises `Rsh::Failed` unless in a condition or under `relaxed!` |
| failure | a failed command that is not a condition raises `Rsh::Failed`; `\|\| true`, `relaxed!`, `relaxed!(false)`; uncaught it ends the script with `rsh: FILE:LINE: PROG exited N` and that status |
| run time | `Rsh::Failed` (`status argv stderr`), `Rsh::Error`, `Rsh::Status` (`exitstatus success? signaled? termsig statuses to_i to_s ==`), `Rsh.last_pid`, `Rsh.nullglob=`, `Rsh.relaxed=`/`relaxed?`, `Rsh.eval`, `Rsh.run`, `Rsh.translate` |
| safety | no text reaches `/bin/sh`; `system` and `Rsh.run` take separate arguments; `open("\|...")`, `IO.popen`, a run-time back-quote are errors |
| signals | TERM, HUP, INT acted on at the next safe point; a trap block runs; with no trap the script ends by that signal after `ensure` and `at_exit`; a signal ignored by the parent stays ignored; a signal inside `at_exit` ends rsh by that signal after the other blocks |

## 3. The command line, statuses, messages, files (STABILITY.md sections 1-8)

Frozen: the flags `-c -e -s --name -n --lint --explain --ruby -i -l -v --version -h --help --`, the text of
`rsh -h`, `--explain` output format, the exit statuses (0, N, 128+N, 130, 1, 2, 127, 126, 70, `exit N`;
`-n`/`--lint` 0, 1, 2), the shape of messages (`rsh: FILE:LINE: TEXT`, a Ruby error adds ` (Class)`,
`PROG exited N`, `PROG was stopped by signal N`, `rsh: PROG: command not found`, `-` for a script on
standard input), the init and history file names and rules, the build inputs (interpreter commit,
pcre2 limits). The words after the place in a message may be improved in a 1.x; the shape and the
status may not.

## 4. The library

The built-in Ruby is the one `docs/rsh.md` ("What is inside") lists. It may gain methods in a 1.x
(section 5); it never loses or changes one.

### 4.1 `Sh`

Built in, no `require`, no program started, Ruby values back. Names and parameters are frozen
(`tests/contract.sh` "STD_SHA").

| method | meaning |
|---|---|
| `Sh.find(dir = ".", name:, type:, older:, newer:, maxdepth:, hidden: true)` | sorted paths below `dir` (never `dir` itself), links not followed; `name` a pattern on the file name; `type` `:file :dir :link`; `older`/`newer` seconds or a Time; `maxdepth` 1 or more; a file as `dir` is `Errno::ENOTDIR`; an unreadable folder is said and skipped |
| `Sh.grep(pattern, *files)` | matching lines; `file:` in front with more than one file; a String is plain text |
| `Sh.which(name)` | full path on `PATH` or `nil` |
| `Sh.exists?(p)`, `file?(p)`, `dir?(p)` | true or false |
| `Sh.age(p)`, `size(p)`, `du(p)` | seconds since the last change; bytes; bytes below |
| `Sh.read(p)`, `write(p, text)`, `append(p, text)`, `lines(p)` | a file's text; `lines` without line ends |
| `Sh.mkdir_p(*dirs)`, `touch(*paths)`, `ln_s(target, link)`, `chmod(mode, *paths)`, `rm(*paths)` | as the programs; `chmod` takes a number; `rm` of a missing file is `Errno::ENOENT` |
| `Sh.cp(src, dst)` | plain files only (a FIFO or device is `ArgumentError`), in pieces, mode kept, onto the same file `ArgumentError`, written to a new file and renamed over `dst` (never through a planted link), the system's error with the destination's name |
| `Sh.cp_r(src, dst)` | a tree; links copied as links; a FIFO, socket or device skipped with a notice; into itself `ArgumentError` |
| `Sh.mv(src, dst)` | rename, or copy and remove across file systems |
| `Sh.rm_rf(*paths)` | removes a tree walking from each folder's own descriptor (`openat` with `O_NOFOLLOW`, `unlinkat`), never through a link; a missing path is fine; refuses `""`, `/`, `.`, `..` and what resolves to `/` or the current folder (`ArgumentError`, nothing removed) |
| `Sh.tmpdir(prefix = "rsh", suffix = "") { \|dir\| }` | a new folder with mode 0700, removed after the block, also when it raises |

### 4.2 The Ruby-standard names

| name | frozen surface |
|---|---|
| `FileUtils` | `mkdir_p makedirs mkpath mkdir rmdir rm rm_f rm_r rm_rf rmtree cp cp_r mv ln_s touch chmod pwd`; an option (`noop:`, `verbose:`) is `ArgumentError`, never ignored; `mv` of several sources needs a folder |
| `Find` | `Find.find(*paths) { \|path\| }`, `Find.prune` |
| `Shellwords` | `escape split join shellescape shellsplit shellwords shelljoin`; `String#shellescape`, `#shellsplit`, `Array#shelljoin` |
| `Dir.mktmpdir` | `Dir.mktmpdir(prefix = nil) { \|dir\| }` and the `[prefix, suffix]` form |
| `require` | `json set time etc fileutils find shellwords tmpdir` (all built in); anything else is `LoadError` with the list |

## 5. What a 1.x may add

- New `Sh` helpers and new keyword options of an existing one (a call that raised `ArgumentError` because
  of an unknown keyword may start to work). Existing names, parameters, return values and errors do not
  change.
- Methods and classes of the built-in Ruby that are missing today (the list of "Not in the interpreter" in
  `docs/rsh.md` may shrink).
- New flags (an unknown flag is status 2 today; section 3 lists the frozen ones).
- A meaning for the reserved syntax of section 2: two-digit and `{fd}` descriptors, `<&-` and `>&-`,
  `N<<<`. A script that does not use them is unaffected.
- A new Ruby word or builtin is not a 1.x change: both lists are frozen and change only in 2.0.
- Clearer wording of messages, more hints in a mistake's text, new `--lint` warnings (a warning is
  status 1 for `--lint` only; it never changes how a file runs).
- Speed, memory and size improvements that change no output. The size limit of the release gate is 3.4 MiB.

A 1.x may not: change how any line of a 1.0 script is read (`tests/frozen.explain`), change a status or
the shape of a message, make an accepted line an error (except the reserved forms above, which are errors
already), or make an error an accepted line when the error is one of the sh forms of section 2.

## 6. Deprecations

There are none in 1.0.0. rsh 1.0 has nothing that a later release will remove. If a 1.x deprecates
something, it says so in the CHANGELOG and in the message of a `--lint` warning at least one minor
release before 2.0 removes it.

## 7. Known limits that are part of 1.0

They are behaviour, not bugs (STABILITY.md "Known limits" K1-K8 and the audit-2 rows left): a pipe
cannot be given back after `gets` (K1); the parser is quadratic in the statements of one file (K2); 125
nested `if`, 62 nested `do` blocks, about 510 recursive calls and one 65,530-byte string literal in the
source (K3); `#{}` values are read once per line (K4); no ARGF (K5); a script on standard input is read
whole (K6); a smaller library without sockets and `Open3` (K7); SIGPIPE'd output is status 141 (K8);
`"$*"` inside a word, `~nosuchuser` (an error), `-n` messages without `rsh: `.

## 8. Platforms and builds

x86_64 Linux, glibc, pcre2 10.45 or later (8-bit). Static (`make`, the CI artifact) and dynamic
(`LINK=dynamic`, the WolOS package `rsh`) builds are both supported. The built-in interpreter is the one
named by its commit in the `Makefile`; changing it is a major version.
