# rsh: Ruby with a shell layer (design, phase 1)

Historical: written when rsh was a directory of the WolOS repository; paths such as `rsh/` and
`tests/rsh/` are now the top of this repository. Later change: `.rb` files are plain Ruby (see
README.md, "Ruby files").

Worker RSH, 2026-11-03. Worktree `wt/rsh`, branch `wave3/rsh` from `wave3/ruby` (6217f7f).
Lead order 2026-11-03 23:06Z: Ruby must work like sh (`#!/usr/bin/sh`, `./script`), "rubyshell ...
rsh", maybe C++ with an embedded interpreter. Builds on ADR 0041 (`wol script`, KINDS, clean env, helper gate, audit).
Labels: [mem] = from memory/ADRs, [fresh] = reasoned or measured now.

## 0. One-paragraph summary

`rsh` is one static binary (`/usr/bin/rsh`): the embedded Ruby interpreter plus a C++ shell layer.
A script is Ruby; any *statement* whose first word is not something Ruby knows is a *command line*
with shell syntax (pipes, redirects, `&&`, `||`, globs, `&`, here-docs). rsh rewrites each command
line into one call of its runner before compiling, keeping line numbers, so the interpreter never sees
shell text and no string ever reaches `/bin/sh`. Ruby values enter commands only as whole arguments
(`#{x}` = exactly one argv entry, an Array = several), so quoting is safe by default. `./deploy.rsh`
works through the shebang, `wol script run|on` gets a `rsh` row in KINDS, the terminal gets an `rsh`
entry beside pwsh, and wolos-pkgbuilds gets a package `rsh`.

```ruby
#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
dest = ARGV[0] || "/srv/app"
cd ~/src/app
git pull --ff-only && make -j#{Etc.nprocessors}
rsync -a build/ #{dest}/            # dest is one argument, whatever it contains
errors = `journalctl -b -p err`.lines.reject { |l| l =~ /fwupd/ }
puts "#{errors.size} errors" unless errors.empty?
if systemctl is-active --quiet app
  systemctl --user restart app
end
```

## (a) The shell layer

### a.1 How a line is classified (deterministic, no PATH lookup, no run-time state)

rsh has a small C++ Ruby lexer (strings, `%w[]`/`%q()` literals, heredocs, regex vs divide,
brackets, keywords) whose only job is to find *statement starts*: start of file, after a newline or
`;` that ends a statement, after `then`/`do`/`else`/`begin`/`ensure`/`rescue ...`/a newline-ended
`when`, after `{` or `do` plus optional `|params|`, after a grouping `(` (not a call's `(`), and after
the condition keywords `if unless while until elsif` (so `if grep -q x f` works).

At a statement start the first word makes it a **command line** when ALL hold:
1. it is a word: identifier, or starts with `./ ../ / ~` (a path is always a command);
2. it is not a Ruby keyword;
3. it is not a local variable assigned earlier in scope (Ruby's own rule: a local wins);
4. it is not a method `def`-ined in the file or in a `require_relative "<literal>"` file (a pre-pass
   collects them), nor in the closed list `RUBY_WORDS` (puts print p pp printf sprintf format require
   require_relative load raise fail loop sleep exit exit! abort at_exit catch throw lambda proc rand
   srand gets warn binding block_given? caller Integer Float String Array Hash attr_reader
   attr_writer attr_accessor private public protected module_function include extend
   define_method system trap); a constant (Capitalised) is Ruby;
5. the next character is not one of `. ( [ :: , = += -= ||= &&= ? !` glued to the word, or
   ` = ` (assignment with spaces).
Escape hatches: a leading `$ ` forces a command line (`$ puts x` runs /usr/bin/puts); `foo(...)` or
`self.foo` forces Ruby. `NAME=val cmd ...` (no spaces, more words after) is a per-command
environment prefix; `x=1` alone is Ruby.
`rsh --explain file` prints each line as `R` (Ruby) or `C` (command); `rsh -n` warns when a command
word is not on `PATH` and is a valid Ruby method name ("did you mean `foo(...)`?").

### a.2 Inside a command line

| Feature | Syntax | Notes |
|---|---|---|
| words, quotes | `'lit'`, `"text #{ruby}"`, `\x` | as sh; double quotes interpolate Ruby, not `$(...)` |
| Ruby values | `#{expr}` | one argv entry; Array -> one entry per element; never split, never globbed |
| env | `$HOME`, `${HOME}` | from `ENV`, one argv entry; `$1..$9` is an error ("use #{ARGV[0]}"), `$*` = ARGV as words, `$?` = last status |
| globs | `* ? [a-z] ** {a,b}` on unquoted text | no match = error (status 1), `Rsh.nullglob = true` to change |
| tilde | `~`, `~/x`, `~user` | `~user` read from /etc/passwd (no NSS, static binary) |
| pipelines | `a | b`, `a |& b` | pipefail: `$?` = last non-zero, `$?.statuses` all |
| Ruby stage | `a | { |line| ... } | b` | block per line: String = replace, truthy = keep, nil/false = drop |
| redirects | `> >> < 2> 2>> 2>&1 &> <<< "s"` | `> #{path}` takes one word |
| here-docs | `<<EOF`, `<<~EOF` (interpolates `#{}`), `<<'EOF'` (literal) | body lines are not classified |
| lists | `a && b`, `a || b`, `a ; b` | `&&`/`||` are shell operators inside a command line |
| background | `a &` | job; `jobs`, `fg %1`, `bg`, `wait`, `$!` |
| end | newline, `;`, unquoted ` #` comment, a lone `}` closing an enclosing brace block | `\` at end continues |
| capture | `` `cmd args` `` / `%x{}` | rsh's own grammar, not /bin/sh; returns `Rsh::Output < String` (`.lines`, `.ok?`), sets `$?` |

`$?` in Ruby is an `Rsh::Status` (`exitstatus`, `success?`, `signaled?`, `termsig`, `statuses`),
the shape Ruby users know from `Process::Status`.

Value of a command line: `true`/`false` by exit status, so `ok = (git diff --quiet)` and
`if make test` work.

**Failure (decision for CTO, see Q1):** in a script a failing command line that is not a condition
(if/unless/while/until, left of `&&`/`||`) raises `Rsh::CommandError` with the status; uncaught it
prints `rsh: deploy.rsh:12: make exited 2` and exits 2. This is `set -e` done soundly (an exception
you can `rescue`). `cmd || true` ignores. Interactive mode never raises.

### a.3 Builtins (in C++, run in-process)
`cd [dir|-]` (also `cd dir do ... end` = Ruby block, back after), `pushd popd dirs`, `export
NAME=val`, `unset NAME`, `exec cmd`, `umask`, `jobs fg bg wait`, `source file`, `echo` (fast path,
same output as coreutils' for `-n -e`), `type`, `history`, `alias`(interactive only).
`exit` is Ruby's. `system("prog", "arg")` is argv-only (a single string is an error: no shell string
anywhere). There is no `sh "string"` helper; `sh -c '...'` as a command stays possible and explicit.

### a.4 How it runs
Script text -> lexer/classifier (C++) -> generated Ruby with the same line count, each command line
`__rsh_cmd(<frozen node table index>, [<interpolated values>])` -> compiled in-process -> run.
Errors point at the original file and line. The node table (parsed pipeline AST) is C++ data; Ruby
only passes values. C++ called from the interpreter never holds RAII objects across a raise (the
interpreter unwinds with longjmp): copy, release, then raise.

## (b) What comes from where

Interpreter: the embedded Ruby (dependency named only in build files), pinned commit
`831da26b9021de0369d17b71b5667e2941a1a32d` (release 4.0.0), tarball sha256
`e4fb1bbad8276878e84e3c3b6161ac35a4216853de404f8463675aa2d1920b4f` (1.35 MB) [fresh, fetched now].
Fetched by `rsh/Makefile` into `rsh/build/`, sha256 checked, never vendored; `MRUBY_TARBALL=` lets
the PKGBUILD hand in its own copy (build() offline).

Gems compiled in: compiler, array-ext, string-ext, hash-ext, enum-ext, enumerator, enum-lazy,
numeric-ext, kernel-ext, object-ext, proc-ext, symbol-ext, range-ext, compar-ext, class-ext,
toplevel-ext, struct, data, set, sprintf, math, time, strftime, io, dir, errno, pack, random, sleep,
exit, error, catch, method, metaprog, encoding, fiber.
Left out: eval and binding (RB rules: no eval), socket (network goes through commands: curl, wol),
bigint, rational, complex, cmath, task, debugger and the bin-* tools.

Missing from that interpreter, written in C++ in rsh:
- **Regexp** (literals compile to `Regexp.new` when the class exists): over libc `regcomp` (ERE +
  GNU `\w \s \b`, `\d` translated); `=~ match match? MatchData $~ $1..`, String `sub gsub scan split
  match? =~`, `i m x` flags. Not supported: lazy quantifiers, lookaround, named groups -> a clear
  compile error. (Q2: alternative is a pinned full regex engine, +~0.5 MB, one more dependency.)
- **JSON** (`JSON.parse`, `to_json`, `JSON.pretty_generate`): small strict parser/writer, plain
  Hash/Array/String only (no object building, RB rule).
- **Process spawning**: `posix_spawn` (+ file actions for redirects, `posix_spawnattr_setpgroup`,
  signal mask/default reset), pipes, `waitpid`; `Process.pid`, `Process.kill`, `Etc.nprocessors`,
  `ENV` kept in sync with the process environment, `Dir.glob` with rsh's glob.
- **Job control** (interactive): own process group per job, `tcsetpgrp`, Ctrl-Z/SIGTSTP, SIGCONT,
  `jobs fg bg`, SIGCHLD reaping, reporting `[1]+ Done`.
- **Line editor** (interactive): raw termios, UTF-8 widths, Emacs keys, history in
  `~/.local/state/rsh/history` (0600), Ctrl-R search, Tab completion (builtins, PATH commands,
  files, Ruby locals/methods), multi-line input while Ruby is incomplete (the parser says
  "incomplete"), **live highlighting** (command word green when found on PATH, red when not, Ruby
  keywords/strings/regex coloured) using the classifier, so the prompt shows what a line will do.
  Prompt from `def prompt` in `~/.config/rsh/init.rsh` (read only when interactive).

Binary: `-static`, `-Os` for the interpreter, `-O2` for rsh, `-ffunction-sections -Wl,--gc-sections`,
stripped; no iostream/locale in the C++ (keeps libstdc++ out of the binary); no NSS calls
(getpwnam etc.) so static glibc is safe. Target **< 3 MB**, start-up **< 10 ms** for
`rsh -c 'true'` and a 1-line script, measured (200 runs, median and p95, cold page cache noted).
Estimate [fresh, unmeasured]: 1.3-2.0 MB, 1-3 ms.

## (c) Plugging in

**wol script** (`crates/controller/src/script.rs`, one new KINDS row):
`id "rsh", interpreters ["rsh"], extensions [".rsh"], program "/usr/bin/rsh", run_args [],
stdin_args ["-s", "--"], syntax ["-n"], lint Some(("rsh", ["--lint"])), lint_only []`.
`--lint` = `-n` plus warnings: unknown command words, `sh -c` with `#{}` inside, world-writable
`chmod`, fixed `/tmp` names (mirrors RB rules). `wol script on host x.rsh` sends the file on ssh stdin
to `env -i ... /usr/bin/rsh -s -- args` (same clean env, same `WOL_HELPER`); a host without rsh
fails with "the other computer has no /usr/bin/rsh". Children of rsh inherit the clean env, so a
`wol` call in a script meets the helper gate as with sh. Shebangs `#!/usr/bin/rsh` and
`#!/usr/bin/env rsh` are recognised by the existing interpreter-name lookup.

**Terminal:** add `/usr/bin/rsh` to `SHELLS` in `crates/core/src/terminal_shell.rs` and to the
same table in `/usr/bin/wol-term-shell` (a test keeps them equal), menu order zsh, bash, rsh, fish,
nu, pwsh; Settings > General "Shell in the terminal" lists it when installed. Login shell stays zsh
(rsh is not POSIX; session and ssh hand the login shell POSIX text) [mem #932]. Those files live on
`wave3/pwsh`, not on `wave3/ruby` (Q3).

**Image:** `rsh` in wolos-iso `profile/packages.desktop` (CTO, other repo).

**Package:** `packaging.PKGBUILD` in this repository (the build files name the embedded interpreter's source; nothing else does).
Installs `/usr/bin/rsh`, `/usr/share/man/man1/rsh.1`. Not added to `/etc/shells` (not a login shell).

## (d) Three grammar options

1. **Ruby first, command lines by first word** (a.1; favourite). Feels like sh for the common case
   (`git pull`, `./build`, `cd x`), Ruby for logic; one control-flow language; the `$ ` prefix and
   `foo(...)` remove any doubt.
2. **Shell first, Ruby inside markers**: POSIX-like `if/then/fi`, Ruby in `{{ ... }}` or `ruby do ...
   end`. Most familiar to sh users, but two control-flow languages, Ruby is second class, and it is
   "one more language" again, which the lead rejected.
3. **Ruby with an explicit command marker only**: every command written `$ git pull` (or in
   backticks). Zero ambiguity, trivial lexer, but no script reads like sh, and `./deploy.rsh` lines
   all carry a sigil.

**Premortem of 1** ("it is six months later and rsh failed; why?"):
- A method from somewhere the pre-pass cannot see (`define_method`, a gem, `require` with a
  computed path) is taken for a command: "rsh: foo: command not found". Mitigation: the error names
  the rule and the fix (`foo(...)`), `rsh -n`/`--lint` warns before running, `--explain` shows
  classification; the rule is static so the same file means the same thing on every machine.
- The lexer mis-reads exotic Ruby (`?a` char literals, `%` literals, heredoc with method call,
  regex after an identifier) and finds a false statement start. Mitigation: classification only at
  statement starts, golden tests of the transpile output for every Ruby literal form, a fuzz-style
  test that ordinary Ruby files (no command lines) come out byte-identical.
- People expect bash (`[[ ]]`, `f() {}`, `case ... esac`, `$(...)`). Mitigation: control flow is
  Ruby on purpose; `rsh -n` recognises those forms and says the Ruby way; man page has a table
  "sh -> rsh".
- `RUBY_WORDS` collides with commands people use (`printf`, `sleep`, `test`): kept short, documented,
  `$ printf` forces the command; `test` stays a command.
- The errexit default surprises sh users (Q1).
- The name: `rsh` is also the old remote-shell client on some systems; on the WolOS base the
  inetutils build does not ship it [fresh, NOT verified: check `pacman -F usr/bin/rsh` on the
  snapshot]; if it is there the package needs `conflicts=`.

## (e) Tests

- **C++ unit tests** (no C++ test framework in the tree [fresh: only plugins/wol-hypr has C++, no
  tests]; plain `assert`-style `CHECK` macro in `rsh/tests/unit.cpp`): lexer statement starts,
  classifier cases (each rule of a.1 both ways), command-line parser (quotes, `#{}`, redirects,
  here-docs, globs, `{a,b}`), glob matcher, Regexp translation, JSON round-trip, argv safety
  (`#{"; rm -rf /"}` stays one argument). `make -C rsh test`.
- **Golden scripts** `tests/rsh/golden/NAME.rsh` with `NAME.out` (stdout + `status: N`) and
  `NAME.rb` (expected transpiled Ruby for the classifier cases); runner `tests/rsh/run.sh` (sh,
  `--update` rewrites). Ordinary Ruby files from the tree pass through unchanged (byte equality).
- **Examples** `tests/rsh/examples/`: `deploy.rsh`, `backup.rsh`, `logscan.rsh`, `remote.rsh`
  (uses `wol script on` / ssh to localhost when allowed, else dry-run flag), `pipeline.rsh`
  (map/filter with a Ruby stage); each run in a temp dir by the golden runner.
- **rules_rsh** `crates/controller/tests/rules_rsh.rs` (beside `rules_ruby.rs`, which lives in
  controller, not shared; Q4): KINDS has the rsh row matching docs/modules/rsh.md; every `.rsh` has
  the shebang + SPDX header; `rsh -n` clean on all `.rsh` when the binary is present
  (`WOLOS_REQUIRE_RSH=1` makes missing fail, as with RuboCop); Makefile pins commit + sha256 and
  never has a vendored copy; Doctrine: no upstream names in `rsh/src`, the man page,
  docs/modules/rsh.md (build files exempt); man page and docs exist; size < 3 MB and start-up check
  in `make -C rsh bench` (not in cargo tests).
- `tools/check-headers.sh` accepts an rsh shebang.

## Layout (phase 2)
`rsh/Makefile`, `rsh/VERSION`, `rsh/build_config.rb` (gem list), `rsh/src/{main,lexer,classify,
cmdline,run,jobs,edit,glob,regexp,json,builtins}.cpp/.hpp`, `rsh/rsh.1`, `rsh/tests/unit.cpp`,
`tests/rsh/{run.sh,golden/,examples/}`, `docs/modules/rsh.md`, KINDS row + tests in
`crates/controller/src/script.rs`, `crates/controller/tests/rules_rsh.rs`, `tools/check-headers.sh`,
CHANGELOG line. ADR: proposed text "0046 rsh" for the CTO to file (workers do not write ADRs).

## Questions for the CTO
- Q1: errexit-by-exception default in scripts (recommended) vs sh's "carry on" default?
- Q2: Regexp over libc (recommended, no new dependency, ERE subset) vs pinned full regex engine?
- Q3: terminal row needs `terminal_shell.rs`/`wol-term-shell` from `wave3/pwsh`; base my branch on a
  merge of ruby+pwsh, or leave the terminal row for you after merge (recommended: I write the
  two-line change as a patch file in .claude/v012/rsh/)?
- Q4: `rules_rsh.rs` in `crates/controller/tests` beside `rules_ruby.rs` (recommended) rather than
  crates/shared/tests?
- Q5: phase-2 scope: interactive line editor + job control + highlighting is the biggest part;
  build scripts first (classifier, runner, Regexp, JSON, wol script kind, tests, examples), then
  interactive in the same branch (recommended), or split interactive to a second worker?
- Q6: the lead's "code is not being highlighted": I read it as editor/site/terminal highlighting
  of Ruby, outside rsh; rsh's own live highlighting covers the interactive prompt only. Who owns the
  rest?
