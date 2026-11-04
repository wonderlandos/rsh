# rsh: Ruby with command lines

Code: `src/transpile.cpp` reads a script and rewrites its command lines, `src/exec.cpp` runs them, `src/rt.cpp` and `src/prelude.rb` are the Ruby side, `src/edit.cpp` is the line editor, `src/main.cpp` is the command. Manual page: `rsh.1`. Tests: `tests/` (below). On WolOS the integration (the `wol script` row, the terminal menu, the package) is described in the WolOS documentation.

## What it is

`rsh` is one program, `/usr/bin/rsh`: Ruby, with a shell layer. A file is Ruby; a statement whose first word is not something Ruby knows is a command line, written as in a terminal (pipes, redirects, `&&`, `||`, patterns, here-documents, `&`). One language for a ten-line backup and for the logic around it, and a script that reads like a shell script and runs with `./deploy.rsh`.

```ruby
#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# deploy: build and copy the app, restart it if it runs.
dest = ARGV[0] || "/srv/app"
cd ~/src/app
git pull --ff-only && make -j#{Etc.nprocessors}
rsync -a build/ #{dest}/            # dest is one argument, whatever it contains
if systemctl is-active --quiet app
  systemctl --user restart app
end
```

Why a new language and not a mix of two: the lead asked that Ruby work like `sh` (`#!/usr/bin/sh`, `./script`). Three grammars were weighed: Ruby first with command lines found by the first word (chosen), shell first with Ruby in markers (two control-flow languages), Ruby with a marker on every command (no script reads like `sh`). The first costs one rule to learn (below) and keeps one control-flow language.

## How a line is read

Only the start of a statement is looked at: the start of the file, after a new line or `;` that ends a statement, after `then`, `do`, `else`, `begin`, `ensure`, after `{` or `do` with its `|params|`, after a grouping `(`, and after the words `if unless while until elsif`. The reader knows Ruby's strings, `%w[]` literals, here-documents, regular expressions and brackets, so a `puts "a b"` inside a string is never a statement start.

At a statement start the line is a command line when all of these hold:

1. the first word is a plain word, or starts with `./`, `../`, `/` or `~` (a path is always a command);
2. it is not a Ruby keyword;
3. it is not a local variable set earlier (Ruby's own rule: a local wins);
4. it is not a method defined in the file (or in a `require_relative "<literal>"` file), and not one of Ruby's own words: `puts print p pp printf sprintf format require require_relative load raise fail loop exit exit! abort at_exit catch throw lambda proc rand srand gets warn binding block_given? caller Integer Float String Array Hash attr_reader attr_writer attr_accessor private public protected module_function include extend define_method system trap`; a Capitalised word is Ruby. `sleep` is a command (`sleep 5 &` works; `sleep(0.5)` is Ruby);
5. the next character is not one of `. ( [ :: , = += -= ||= &&= ? !` glued to the word, and it is not ` = `.

Escape hatches: a leading `$ ` forces a command line (`$ printf '%s\n' x` runs the program, not Ruby's `printf`); `foo(...)` or `self.foo` forces Ruby. `NAME=val cmd ...` sets a variable for that one command; `x=1` alone is Ruby.

`rsh --explain file` prints the mode, then each line as `R` (Ruby), `C` (command line) or `X` (a back-quote capture inside Ruby). `rsh -n` compiles without running; `rsh --lint` adds warnings: a command word that is not on `PATH` and could be a Ruby method ("write foo(...)"), `sh -c` text with a `#{}` in it, a world-writable `chmod`, fixed `/tmp` names. The rule is static, so the same file means the same on every machine. `tests/golden/classify.rsh` and its `.explain` file are the examples of every case.

## Ruby files

`rsh file.rb` runs a plain Ruby file: the shell layer is off, so no line is a command line, whatever its first word, and the file means what it means in Ruby (a line `ls -l` is a Ruby call, not a command). Existing Ruby scripts are not changed by rsh. The Ruby is the one built into rsh (see "What is inside" for what it leaves out).

A `.rb` file asks for the shell layer with the comment `# rsh: shell` as its first line, or as the line after a `#!` line:

```ruby
#!/usr/bin/rsh
# rsh: shell
git pull --ff-only && make
```

Every other file (`.rsh`, no ending, anything else) has the shell layer, and so do `-c`, `-e` and a script on standard input. `-s --name x.rb` gives a script on standard input the name `x.rb` for messages and for this rule. `rsh --explain file` starts with `mode: shell` or `mode: ruby`; `-n` and `--lint` on a plain `.rb` file only check that it is Ruby; `--ruby` shows the text unchanged. A file loaded with `require_relative`, `load` or `source` follows the same rule by its own name and first line.

## Inside a command line

| Feature | Syntax | Notes |
|---|---|---|
| words, quotes | `'text'`, `"text #{ruby}"`, `\x` | as in a shell; no `$(...)` |
| Ruby values | `#{expr}` anywhere in a word | one argument; an Array gives one argument per element; never split again, never expanded as a pattern; `nil` is an error |
| environment | `$HOME`, `${HOME}`, `$?` | `$1..$9` is an error ("use ARGV") |
| patterns | `* ? [a-z] {a,b} **` | unquoted text only; no match is an error (`Rsh.nullglob = true` changes it) |
| tilde | `~`, `~/x`, `~user` | |
| pipelines | `a \| b`, `a \|& b` | status is the last stage that failed, `$?.statuses` has every stage |
| Ruby stage | `a \| { \|line\| ... } \| b` | a String replaces the line, `nil` or `false` drops it, any other value keeps it |
| redirects | `> >> < 2> 2>> 2>&1 &> <<<` | `> #{path}` takes one word |
| here-documents | `<<EOF`, `<<~EOF`, `<<'EOF'` | the first two take `#{}`; the third is literal; body lines are not classified |
| lists | `a && b`, `a \|\| b`, `a ; b` | after `&&` or `\|\|` the next word is a command again (`puts` there is the program, use `echo`) |
| background | `a &` | `jobs`, `fg`, `bg`, `wait` |
| capture | `` `cmd args` ``, `%x{...}` | rsh's own grammar, never `/bin/sh`; gives a String, status in `$?` |

No string reaches `/bin/sh`: a command line is parsed once, when the file is read, into a table; Ruby only passes values. `system("prog", "arg")` takes separate arguments (one string with spaces is an error). A command line is also a value: `true` or `false` by status, so `if make test` and `ok = (git diff --quiet)` work.

## Failure

A command line that fails and is not a condition (`if`, `unless`, `while`, `until`, left of `&&` or `||`) raises `Rsh::Failed` with `status`, `argv` and `stderr` (the tail of what the command wrote on its error output). Uncaught, the script ends with `rsh: deploy.rsh:12: make exited 2` and that status. `begin ... rescue Rsh::Failed => e` handles it, `|| true` ignores it, `relaxed!` turns raising off for the rest of the script (`relaxed!(false)` back on) and `$?` is set either way: an `Rsh::Status` with `exitstatus`, `success?`, `signaled?`, `termsig` and `statuses`. In the interactive shell nothing is raised. In scripts the error output of a foreground command goes through a pipe so `stderr` has its tail (the child's error output is then not a terminal). A writer ended by SIGPIPE before the last stage of a pipeline (`yes | head`) does not fail the pipeline; it shows in `$?.statuses`. A capture returns a plain String; its status is in `$?`.

Commands built in: `cd` (also `cd dir do ... end`, which returns afterwards), `pushd popd dirs export unset exec umask jobs fg bg wait source echo type pwd history`. `exit` is Ruby's.

## What is inside

The interpreter is built into the program (named in `Makefile` and `build_config.rb`, credited with its licence in the package). It is fetched at one pinned commit and checked by sha256 and never kept in the tree. Left out on purpose: `eval` and `binding`, sockets (network goes through commands: `curl`, `wol`), big and rational numbers. Written for rsh: process spawning with redirects, process groups and job control, `ENV` kept in step with the process environment, `Dir.glob`, `File.write`, `File.readlines`, JSON (`JSON.parse`, `to_json`, `JSON.pretty_generate`), and `Regexp` over the system's regular-expression library (named groups, look-ahead, lazy quantifiers; `=~ match match? MatchData $~ $1` and `sub gsub scan split`). `require` knows only `json set time etc` (built in); your own files load with `require_relative`.

Interactive (`rsh` on a terminal, `-i`, or `-l`, which the terminal passes): a line editor (UTF-8 widths, Emacs keys, history in `~/.local/state/rsh/history`, mode 0600, Ctrl-R search), Tab completion (builtins, programs on `PATH`, files, your variables and methods), more input while the Ruby is not finished, job control (own process group per job, Ctrl-Z, `fg`, `bg`, `[1]+ Done`), and colours while you type from the same reader that runs the line: the command word is green when found and red when not, keywords, strings and numbers have their own colours. `~/.config/rsh/init.rsh` runs first; `def prompt` in it gives the prompt.

## Build and size

`make` builds `build/rsh` (needs a C and C++ compiler, `ruby` and `bison` for the interpreter's own build script, and `libpcre2-8`; `LINK=static` is the default and needs `libpcre2-8.a`, `LINK=dynamic` is for distributions without it). `make test` runs the unit checks and the golden scripts, `make bench` the size and start-up. A Makefile variable hands in a local copy of the interpreter source (the package build is offline).

Measured on the AWS build box (2026-11-04, x86_64, static, stripped, `-Os`): 2,895,976 bytes; start-up of `rsh -e 'puts 1'` median 2.45 ms (20 runs, min 2.0, max 3.0); `rsh -c nil` median 1.75 ms, p95 2.47 ms (200 runs); a one-line script median 2.32 ms. Limits: under 3 MB, under 10 ms.

## Tests

- `tests/unit.cpp`: the reader (statement starts, the command-line rule, the grammar), patterns, argument safety, the `.rb` rule. `make test`.
- `tests/golden/*.rsh` and `*.rb` with `.out` (standard output and a last line `status: N`), `.explain` (`rsh --explain`) and `.err` (standard error): classification, one-argument interpolation, pipelines, redirects, `&&`/`||`, patterns, here-documents, `Rsh::Failed`.
- `tests/examples/*.rsh`: five whole scripts (deploy, backup, log scan, remote run, a pipeline with Ruby map and filter) with expected output, run with stand-ins for `git`, `make`, `rsync`, `systemctl` and `ssh` from `tests/fakebin/`.
- `tests/run.sh` runs both (`RSH=path sh run.sh [--update] [name ...]`; `--update` rewrites the expected files, read the diff before keeping it).
- `tests/interactive.rb` drives the editor through a pseudo-terminal (needs `ruby`, skipped without it).
- `tests/doctrine.sh`: no upstream project names in the program's own text; the size limit (3 MB) when a binary is built.

## Not done yet

- Ctrl-Z / `fg` / `bg` are written but `tests/interactive.rb` only covers `&`, `jobs`, editing, completion, continuation lines, history search and colours.
- Static linking warns that `~user` expansion in the interpreter's file library looks up the account database; rsh's own `~user` reads `/etc/passwd`.
