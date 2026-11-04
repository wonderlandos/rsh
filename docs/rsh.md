# rsh: Ruby with command lines

Code: `src/transpile.cpp` reads a script and rewrites its command lines, `src/exec.cpp` runs them, `src/rt.cpp`, `src/prelude.rb` and `src/prelude_regexp.rb` (loaded on first use) are the Ruby side, `src/edit.cpp` is the line editor, `src/main.cpp` is the command. Manual page: `rsh.1`. Tests: `tests/` (below). On WolOS the integration (the `wol script` row, the terminal menu, the package) is described in the WolOS documentation.

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
4. it is not a method the file makes in a way the reader can see (below), and not one of Ruby's own words: `puts print p pp printf sprintf format require require_relative load raise fail loop exit exit! abort at_exit catch throw lambda proc rand srand gets warn binding block_given? caller Integer Float String Array Hash attr_reader attr_writer attr_accessor private public protected module_function include extend define_method system trap freeze __method__`, the metaprogramming words `send public_send __send__ eval instance_eval instance_exec class_eval class_exec module_eval module_exec define_singleton_method instance_variable_get instance_variable_set remove_method undef_method alias_method prepend using refine private_constant public_constant private_class_method public_class_method remove_const const_get const_set singleton_class method rsh_methods`, the conversions `to_a to_s to_h to_i to_f to_sym to_proc inspect dup clone instance_variables`, and (0.1.3) the rest of the interpreter's own functions `open putc readline readlines __dir__ __callee__ global_variables local_variables usleep tap itself object_id enum_for to_enum yield_self methods singleton_methods public_methods private_methods protected_methods remove_instance_variable singleton_method` (so `open "notes.txt"` opens the file and never runs a program called `open`); a Capitalised word is Ruby. The list is one list (`RUBY_WORDS` in `src/transpile.cpp`), frozen from 0.1.3 (`docs/STABILITY.md`): it only grows, in a major version. `sleep` is a command (`sleep 5 &` works; `sleep(0.5)` is Ruby);
5. the next character is not one of `. ( [ :: , = += -= ||= &&= ? !` glued to the word, and it is not ` = `.

The methods the reader can see (rule 4), anywhere in the file or in a `require_relative "<literal>"` file, before or after the line that uses them:

- `def name` (also `def self.name`, `private def name`);
- a literal name after `define_method`, `define_singleton_method`, `alias_method`, `alias`, `attr_reader`, `attr_accessor`, `attr_writer`, `Struct.new(...)`, `Data.define(...)`: `:name`, `"name"`, `'name'`, `%i[a b]` or `%w[a b]`;
- `rsh_methods :name, ...` (the same literal forms): the declaration for methods that only exist at run time, made by `method_missing` or by `define_method` over a list. At run time `rsh_methods` does nothing but add the names for `Rsh.eval`.

A name in a variable (`define_method(verb)`, `define_method("#{verb}!")`) is known only at run time, so the reader cannot count it; declare it with `rsh_methods` or call it as `name(...)` or `self.name`.

Escape hatches: a leading `$ ` forces a command line (`$ printf '%s\n' x` runs the program, not Ruby's `printf`); `foo(...)` or `self.foo` forces Ruby. `NAME=val cmd ...` sets a variable for that one command; `x=1` alone is Ruby. `ruby do ... end` or `ruby { ... }` at a statement start makes every statement inside Ruby (no bare word is a command); `ruby!` alone on its line does the same from there to the end of the file. In both, `$ cmd` and back-quote captures still run programs. Inside `#{...}` in a command line the code is a Ruby expression, as in a Ruby string (`echo #{name}` calls `name`).

`rsh --explain file` prints the mode, the names it reads as methods (`methods: ...`), the Ruby-only lines (`ruby only: lines 9-14 (a ruby block)`, `ruby only: line 30 to the end (ruby!)`), then each line as `R` (Ruby), `C` (command line) or `X` (a back-quote capture inside Ruby). `rsh -n` compiles without running; `rsh --lint` adds warnings: a command word that is not on `PATH` and could be a Ruby method ("write foo(...) or declare it with rsh_methods :foo"), `sh -c` text with a `#{}` in it, a world-writable `chmod`, fixed `/tmp` names, a value as the first argument of a program (`rm #{f}`: a value that starts with `-` is an option; write `rm -- #{f}`). `--lint` exits 0 when clean, 1 with warnings only, 2 with errors. The rule is static, so the same file means the same on every machine. `tests/golden/classify.rsh` and its `.explain` file are the examples of every case.

## Ruby files

Every file has the shell layer, whatever its name: `rsh file.rb` reads a `.rb` file exactly as a `.rsh` file, so Ruby and command lines mix the same way in both. A file that must stay plain Ruby turns the layer off with the comment `# rsh: ruby` as its first line, or as the line after a `#!` line; then no line is a command line, whatever its first word, and the file means what it means in Ruby (a line `ls -l` is a Ruby call). `# rsh: shell` in the same place says the default out loud. `ruby file.rb` (the system's Ruby, when installed) runs Ruby without rsh.

```ruby
#!/usr/bin/rsh
# rsh: ruby
puts ARGV.map(&:upcase).join(" ")      # plain Ruby
```

The same holds for `-c`, `-e` and a script on standard input; `-s --name x.rb` gives such a script its name for messages and `$0`. `rsh --explain file` starts with `mode: shell` or `mode: ruby`; `-n` and `--lint` on a `# rsh: ruby` file only check that it is Ruby; `--ruby` shows its text unchanged. A file loaded with `require_relative`, `load` or `source` follows the same rule by its own first line. The Ruby is the one built into rsh (see "What is inside" for what it leaves out).

## Metaprogramming

Ruby's metaprogramming works with the shell layer on and off: `define_method`, `method_missing` and `respond_to_missing?`, `send`, `public_send`, `instance_eval`, `instance_exec`, `class_eval` (a block or a string, with `__FILE__, __LINE__`), `eval`, `binding`, `define_singleton_method`, `prepend`, `include` and `extend` with `included`, `extended`, `prepended`, `inherited`, `method_added`, `const_missing`, `ObjectSpace.each_object`, procs, lambdas, `curry`, `Method` objects, `Struct`, `Data` and `Comparable`.

What to know with the shell layer on:

- **Methods made at run time.** The reader decides what a line is before the script runs (the rule above). A method made by `method_missing`, or by `define_method` with a name in a variable, is not visible then, so a bare call to it at a statement start is a command line. Write it as `name(...)` or `self.name`, declare it with `rsh_methods :name`, or put the code in `ruby do ... end`.
- **A class that runs programs.** In `method_missing`, `$ #{name} #{args}` runs the program called `name` with every element of `args` as one argument: no shell reads it, nothing is split or expanded. `($ #{name} #{args})` in parentheses gives `true` or `false` and does not raise.
- **Blocks with command lines.** A block given to `instance_eval`, `class_eval`, `define_method`, `Class.new` or any method is read like the rest of the file: command lines in it run when the block runs, with that block's `self`.
- **Two evals.** `eval(text)`, `instance_eval(text)` and `class_eval(text)` are plain Ruby: a line `echo x` in the text is a Ruby call. `Rsh.eval(text)` reads the text with the shell layer and runs it at the top level (`self` is the main object, the caller's local variables are not seen); `Rsh.eval(text, binding)` runs it in that binding (its `self` and local variables, which are never commands). A failing command in it raises `Rsh::Failed` as anywhere. `Rsh.translate(text)` gives the Ruby that `Rsh.eval` would run.
- **Code built as text.** `class_eval <<~RUBY, __FILE__, __LINE__ + 1` with `#{}` filling in names works as in Ruby. A text given to `instance_eval` or `class_eval` does not see the caller's local variables (pass values in with `#{}` or use the block form, which does): in the built-in interpreter the receiver of such a call would otherwise stay the place where later `def` and `class` lines of the caller go.

`tests/golden/meta_*.rsh` and `meta_*.rb` show each case with its output. `examples/meta/deploy_dsl.rsh` is a deploy language made with `method_missing` (`git.checkout "v1"` becomes a step, run locally with `$ #{argv}` or over `ssh` with each word quoted), `examples/meta/tasks.rsh` a task runner whose tasks are methods made with `define_method`.

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

No string reaches `/bin/sh`: a command line is parsed once, when the file is read, into a table; Ruby only passes values. `system("prog", "arg")` takes separate arguments (one string with spaces is an error). `open("|cmd")`, `IO.popen` and a back-quote built at run time are `Rsh::Error` (the interpreter is built without popen). A NUL byte in an argument, a `NAME=` value or a redirect target is an `Rsh::Error` (a program would get a shorter argument than the script holds); a here-document may hold one. A file found by a pattern whose name starts with `-` is given as `./-name`, so it is never an option. An Array in a `NAME=` value or in a here-document is an error (join it), as inside a word. An empty part of `PATH` is skipped, not read as the current folder. A command line is also a value: `true` or `false` by status, so `if make test` and `ok = (git diff --quiet)` work.

## Failure

A command line that fails and is not a condition (`if`, `unless`, `while`, `until`, left of `&&` or `||`) raises `Rsh::Failed` with `status`, `argv` and `stderr` (the tail of what the command wrote on its error output). Uncaught, the script ends with `rsh: deploy.rsh:12: make exited 2` and that status. `begin ... rescue Rsh::Failed => e` handles it, `|| true` ignores it, `relaxed!` turns raising off for the rest of the script (`relaxed!(false)` back on) and `$?` is set either way: an `Rsh::Status` with `exitstatus`, `success?`, `signaled?`, `termsig` and `statuses`. In the interactive shell nothing is raised. In scripts the error output of a foreground command goes through a pipe so `stderr` has its tail (the child's error output is then not a terminal). A writer ended by SIGPIPE before the last stage of a pipeline (`yes | head`) does not fail the pipeline; it shows in `$?.statuses`. A capture returns a plain String; its status is in `$?`.

Commands built in: `cd` (also `cd dir do ... end`, which returns afterwards), `pushd popd dirs export unset exec umask jobs fg bg wait source echo type pwd history`. `exit` is Ruby's. `umask` takes octal digits only (`umask 022`); anything else is an error (status 2) and the mask stays. `unset` checks the name. `NAME=v exec prog` gives `prog` the variable. In a script, background jobs are reaped before each command line (no zombies) and `wait` still gives an ended job's status; `fg` and `bg` need job control (the interactive shell) and are an error in a script. `pwd`, `pushd` and `cd` say "the current folder was removed" when it was.

## What is inside

The interpreter is built into the program (named in `Makefile` and `build_config.rb`, credited with its licence in the package). It is fetched at one pinned commit and checked by sha256 and never kept in the tree. In for metaprogramming: `eval`, `binding`, `instance_eval`/`class_eval` with a string, `ObjectSpace`. Left out on purpose: sockets (network goes through commands: `curl`, `wol`), big and rational numbers. Not in the interpreter: refinements (`refine`, `using`), `defined?`, `$!` (it stays `nil`; use `rescue => e`), the block parameter `it` (`_1` works), `TracePoint`, `ObjectSpace::WeakMap`, `const_source_location`; `ObjectSpace.each_object` needs a block. Added by rsh: `warn`, `abort`, `__dir__`, `File.mtime`, `File.expand_path("~user")` from `/etc/passwd` (not the C library's account modules), `private_method_defined?`, `public_method_defined?`, `protected_method_defined?`, `Data#with`. Written for rsh: process spawning with redirects, process groups and job control, `ENV` kept in step with the process environment, `Dir.glob`, `File.write`, `File.readlines`, JSON (`JSON.parse`, `to_json`, `JSON.pretty_generate`), and `Regexp` over the system's regular-expression library (named groups, look-ahead, lazy quantifiers; `=~ match match? MatchData $~ $1`, `sub gsub scan split`, `str[/re/]` and `str[/re/, group]`). The Ruby side of Regexp loads the first time a pattern is made or `scan`, `match`, `match?`, `=~` or one of `Regexp`'s own methods is called, so a script without patterns does not read it at start-up. `require` knows only `json set time etc` (built in); your own files load with `require_relative`. A match has limits (1,000,000 steps, 256 MB of backtracking): a pattern that would need more on a text raises `RegexpError` ("too complex") instead of running for seconds. Strings and Arrays have no fixed size cap (the interpreter's default cap of 131072 Array entries is off).

Interactive (`rsh` on a terminal, `-i`, or `-l`, which the terminal passes): a line editor (UTF-8 and wide-character widths, Emacs keys, history in `~/.local/state/rsh/history`, mode 0600, Ctrl-R search), Tab completion (builtins, programs on `PATH`, files, your variables and methods), more input while the Ruby is not finished, job control (own process group per job, Ctrl-Z, `fg`, `bg`, `[1]+ Done`), and colours while you type from the same reader that runs the line: the command word is green when found and red when not, keywords, strings and numbers have their own colours. `~/.config/rsh/init.rsh` runs first; `def prompt` in it gives the prompt. History does not keep a line that starts with a space or looks like it holds a secret (`export GITHUB_TOKEN=...`, `--password=...`, an Authorization header, a known token prefix). init.rsh and the history file are used only when they are plain files (never through a link), yours, and not writable by others; an older history file is set to 0600. As root (or set-user-id) the home comes from `/etc/passwd`, `$HOME` and `$XDG_*` are not used for these files, and a file its group can write is refused too, so `sudo -E rsh` never runs another person's init.rsh. File names, the current folder and history entries are shown with control characters made visible (`^[`), so a name cannot send escape sequences to the terminal.

## Build and size

`make` builds `build/rsh` (needs a C and C++ compiler, `ruby` and `bison` for the interpreter's own build script, and `libpcre2-8`; `LINK=static` is the default and needs `libpcre2-8.a`, `LINK=dynamic` is for distributions without it). `make test` runs the unit checks, the golden scripts, the stability contract (`tests/contract.sh`, `docs/STABILITY.md`) and then all of them again on the sanitizer build (`make asan`: `LINK=dynamic`, `-fsanitize=address,undefined`, leak check on; any report fails); `make bench` the size and start-up. `LINK=static` (the default) is one file with no shared libraries; the WolOS package builds `LINK=dynamic` (glibc and libpcre2-8 shared). A Makefile variable hands in a local copy of the interpreter source (the package build is offline).

Measured on the AWS build box (2026-11-04, x86_64, static, stripped, `-Os`): 2,895,976 bytes; start-up of `rsh -e 'puts 1'` median 2.45 ms (20 runs, min 2.0, max 3.0); `rsh -c nil` median 1.75 ms, p95 2.47 ms (200 runs); a one-line script median 2.32 ms. 0.1.1 (2026-11-04, same box, with eval, binding and ObjectSpace): 2,920,552 bytes; `rsh -c nil` median 1.08 ms against 1.01 ms for 0.1.0 (400 interleaved runs each). 0.1.2 (2026-11-04, a 32-core build box with other jobs running, timed by a small fork-and-wait program, 400 interleaved runs, three rounds): 2,928,744 bytes; `rsh -c nil` median 1.16-1.17 ms, 0.1.0 1.15-1.17 ms, 0.1.1 1.19-1.20 ms; the Ruby side of Regexp now loads on first use, so `rsh -c 'x = "a1" =~ /\d/'` is 1.31-1.33 ms against 1.26-1.27 ms for 0.1.1. 0.1.3 (2026-11-04, the same box, fork-and-wait timer, 400 interleaved runs against 0.1.2): see `CHANGELOG.md`. Limits: under 3 MB, under 10 ms.

## Tests

- `tests/unit.cpp`: the reader (statement starts, the command-line rule, the grammar), patterns, argument safety, the `.rb` rule. `make test`.
- `tests/golden/*.rsh` and `*.rb` with `.out` (standard output and a last line `status: N`), `.explain` (`rsh --explain`) and `.err` (standard error): classification, one-argument interpolation, pipelines, redirects, `&&`/`||`, patterns, here-documents, `Rsh::Failed`.
- `tests/golden/meta_*.rsh`, `meta_*.rb`: metaprogramming (define_method, method_missing both ways, instance_eval/class_eval, eval and Rsh.eval, hooks, const_missing, ObjectSpace, procs, Struct, Data, `ruby do`, `ruby!`, `rsh_methods`), with and without the shell layer. `examples/meta/*.rsh` run with expected output in `tests/examples/meta/`.
- `tests/examples/*.rsh`: five whole scripts (deploy, backup, log scan, remote run, a pipeline with Ruby map and filter) with expected output, run with stand-ins for `git`, `make`, `rsync`, `systemctl` and `ssh` from `tests/fakebin/`.
- `tests/golden/readme_*.rsh`: every ```` ```ruby ```` block of `README.md`, as printed there (`tests/run.sh` fails when a block is in none of them; a test may add lines before or after it), run with the stand-ins in `tests/fakebin/readme/` (`curl`, `ssh`, `nft`, `make`, `df`, `sleep`). A `NAME.setup` shell script next to any test makes the files it works on, in its empty directory.
- `tests/run.sh` runs both (`RSH=path sh run.sh [--update] [name ...]`; `--update` rewrites the expected files, read the diff before keeping it).
- `tests/interactive.rb` drives the editor through a pseudo-terminal (needs `ruby`, skipped without it).
- `tests/doctrine.sh`: no upstream project names in the program's own text; the size limit (3 MB) when a binary is built.
- `tests/golden/sec_*.rsh`: the reproducers of the 0.1.3 security and stability audit, one per finding (C1 the command-line table, C2 no `/bin/sh`, H1 `umask`, H2 NUL bytes, M1 the Ruby words, M2 `./-name`, M5 jobs, M6 `exec`, M7 pattern limits, M8 loads, the lows); M3, M4 and H3 are in `tests/interactive.rb` and `tests/unit.cpp`.
- `tests/contract.sh`: the stability contract (`docs/STABILITY.md`): flags, exit statuses, message formats, value safety, the `--explain` of `classify.rsh`, the Ruby words, file names, build inputs, the release gate.

## Not done yet

- Ctrl-Z / `fg` / `bg` are written but `tests/interactive.rb` only covers `&`, `jobs`, editing, completion, continuation lines, history search and colours.
- Static linking still warns that the interpreter's file library calls the account database; since 0.1.3 nothing reaches that call (`File.expand_path("~user")` reads `/etc/passwd`).
- `String#scan`, `gsub` and `split` with a pattern take time that grows with the square of the text size on long texts (each match call checks the rest of the text for valid UTF-8): a 100 KB text with 20,000 matches takes about 0.5 s.
