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

What 0.1.4 changed in the reader (each has a case in `tests/golden/classify.rsh`, the frozen `.explain` digest was printed again for those lines only, `CHANGELOG.md`): a brace pattern right after the command word is a word (`echo {a,b}`; `foo { |x| ... }` is still a block); `! cmd` at a statement start is a command line with its status turned round (`! grep -q x f`, `if ! test -d d`; `!x` glued stays Ruby); a Capitalised `NAME=` followed by Ruby (`Point=Struct.new(:x) do`, `Limit=10; puts Limit`) is Ruby, while `NAME=value cmd` is still a command line; after `&&` or `||` the Ruby control words `exit raise abort return break next` and `:` end the command line (`cmd || exit 1` exits 1; a program that is missing is still 127); a `|` at the end of a line continues on the next line, and at the end of the file it is an error; after `then`, `else` or `do` on one line, a plain `end` closes the Ruby construct (`if x then echo a end`). `X=~ cmd` is still the match operator (Ruby); write `X=$HOME cmd`. A file is read after dropping a leading UTF-8 byte-order mark and with every CR LF as LF, so `# rsh: ruby` and here-documents work in files saved on Windows. A mode comment with another word (`# rsh: rubi`) is an error (status 2), not the shell layer by default. Some sh forms are reader errors that say what to write instead: `[ ... ]`, `$(...)`, `set -e`, `. file`, `true && cmd`. `-n` and `--lint` warn, without failing `-n`, about a bare word alone in a group (`x = 1 + (y)` runs a program `y`: write `x = 1 + y`) and a command whose first argument is a Ruby operator (`to_der == other`: a comparison?); `--explain` exits 2, as `-n` does, when the Ruby does not compile.

## Inside a command line

| Feature | Syntax | Notes |
|---|---|---|
| words, quotes | `'text'`, `"text #{ruby}"`, `\x` | as in a shell; no `$(...)` |
| Ruby values | `#{expr}` anywhere in a word | one argument; an Array gives one argument per element; never split again, never expanded as a pattern; `nil` is an error |
| environment | `$HOME`, `${HOME}`, `$?` | `$1..$9` is an error ("use ARGV") |
| patterns | `* ? [a-z] {a,b} **` | unquoted text only; no match is a failed command, status 1 (`Rsh.nullglob = true`: no words) |
| tilde | `~`, `~/x`, `~user` | |
| pipelines | `a \| b`, `a \|& b` | status is the last stage that failed, `$?.statuses` has every stage |
| Ruby stage | `a \| { \|line\| ... } \| b` | a String replaces the line, `nil` or `false` drops it, any other value keeps it |
| redirects | `> >> < 2> 2>> 2>&1 &> <<<` | `> #{path}` takes one word |
| here-documents | `<<EOF`, `<<~EOF`, `<<'EOF'` | the first two take `#{}`, and a backslash goes only before `$`, a back-quote, `\` and a new line, as in sh (`C:\Users` stays); the third is literal; body lines are not classified |
| lists | `a && b`, `a \|\| b`, `a ; b` | after `&&` or `\|\|` the next word is a command again (`puts` there is the program, use `echo`) |
| background | `a &` | `jobs`, `fg`, `bg`, `wait` |
| capture | `` `cmd args` ``, `%x{...}` | rsh's own grammar, never `/bin/sh`; gives a String, status in `$?` |

No string reaches `/bin/sh`: a command line is parsed once, when the file is read, into a table; Ruby only passes values. `system("prog", "arg")` takes separate arguments (one string with spaces is an error). `open("|cmd")`, `IO.popen` and a back-quote built at run time are `Rsh::Error` (the interpreter is built without popen). A NUL byte in an argument, a `NAME=` value or a redirect target is an `Rsh::Error` (a program would get a shorter argument than the script holds); a here-document may hold one. A file found by a pattern whose name starts with `-` is given as `./-name`, so it is never an option. An Array in a `NAME=` value or in a here-document is an error (join it), as inside a word. An empty part of `PATH` is skipped, not read as the current folder. A command line is also a value: `true` or `false` by status, so `if make test` and `ok = (git diff --quiet)` work.

## Failure

**No match and a file that cannot be opened (0.9.0, decision D1).** A pattern with no match (`rm *.tmp`) and a redirect that cannot open its file (`cat < missing`, `echo x > no-folder/f`) are a failed command with status 1, as a program that is not found is 127: in a condition they are `false` (`ls *.log 2>/dev/null || echo none` takes the `||`; a no-match says nothing there, an open failure says why on standard error), outside one they raise `Rsh::Failed` whose message is the reason (`rsh: tidy.rsh:3: no file matches *.tmp`). A `nil` value, a NUL byte and an Array inside a word stay `Rsh::Error`: those are mistakes in the script, not in the files.

A command line that fails and is not a condition (`if`, `unless`, `while`, `until`, left of `&&` or `||`) raises `Rsh::Failed` with `status`, `argv` and `stderr` (the tail of what the command wrote on its error output). Uncaught, the script ends with `rsh: deploy.rsh:12: make exited 2` and that status. `begin ... rescue Rsh::Failed => e` handles it, `|| true` ignores it, `relaxed!` turns raising off for the rest of the script (`relaxed!(false)` back on) and `$?` is set either way: an `Rsh::Status` with `exitstatus`, `success?`, `signaled?`, `termsig` and `statuses`. In the interactive shell nothing is raised. In scripts the error output of a foreground command goes through a pipe so `stderr` has its tail (the child's error output is then not a terminal). A writer ended by SIGPIPE before the last stage of a pipeline (`yes | head`) does not fail the pipeline; it shows in `$?.statuses`. A capture returns a plain String; its status is in `$?`.

Commands built in: `cd` (also `cd dir do ... end`, which returns afterwards), `pushd popd dirs export unset exec umask jobs fg bg wait source echo type pwd history`. `exit` is Ruby's. `umask` takes octal digits only (`umask 022`); anything else is an error (status 2) and the mask stays. `unset` checks the name. `export NAME=value` sets the environment; `export NAME` keeps a NAME that is set and is a mistake for one that is not (0.9.0: `FOO=1` is Ruby in rsh, so `FOO=1; export FOO` exported an empty FOO; write `export FOO=#{FOO}`). `source FILE WORDS` runs FILE in this process with `ARGV` = WORDS while it runs (0.9.0), and a mistake in FILE is reported with FILE's name and line. A `source` whose path is written out (`source lib.rsh`, `source "lib.rsh"`, decision D3) is read ahead like a `require_relative`: its `def`s are Ruby at a bare call (`greet "world"`). A written-out path is relative to the folder of the script it is in, both when the file is read and when the line runs (never the current folder, so the same script reads the same everywhere, also after a `cd`); `-c`, a script on standard input and the interactive shell have no file, so for them it is the current folder. A path made at run time (`#{}`, `$`, `~`, a pattern) is from the current folder when the line runs, and the reader cannot see its methods: call them as `name(...)`. Only a plain file is read ahead (never a FIFO or a device). `wait PID` waits for the job that holds that process, `wait %N` for job N; a plain number is first a pid, then a job number (a pid wins); `Rsh.last_pid` is the pid of the last `cmd &` in Ruby (decision D2; `nil` before the first; Ruby's `$!` is its exception variable). In a command line `$?` is the status of the part that ran last (`cmd || echo "failed: $?"`, 0.9.0); the `#{...}` values of a line are all read once, when the line starts. `NAME=v exec prog` gives `prog` the variable. In a script, background jobs are reaped before each command line (no zombies) and `wait` still gives an ended job's status; `fg` and `bg` need job control (the interactive shell) and are an error in a script. `pwd`, `pushd` and `cd` say "the current folder was removed" when it was. `echo`, `pwd` and `type` run in this process when they are alone on a line and not captured (no fork), and a failed write (`echo x > /dev/full`) is `rsh: echo: write error: ...` with status 1; `echo -e` knows `\n \t \e \r \a \b \f \v \xHH \0NNN`. `history` lists the entries of earlier sessions and this one's, numbered. `wait N` for a job that is not there says so (127). A path with a `/` that exists but cannot run is 126 (`Permission denied`, `Is a directory`), and `rsh DIR` is `rsh: DIR: Is a directory`, 126.

Signals and the end of a script (0.1.4): `at_exit { }` (last registered first), `trap("TERM") { }`, `trap("INT", "IGNORE")`, `trap("EXIT") { }` (after the `at_exit` blocks), `Signal.trap`, `Signal.list` and `Signal.signame` exist, `Process.kill` takes any signal name of the system, a number or a Symbol (an unknown name is `ArgumentError` and nothing is sent), `Process.ppid`, `Dir.home` / `Dir.home("user")` (from `/etc/passwd`) and `$PROGRAM_NAME`. A signal is noted when it comes and acted on at the next safe point: a command line, an iteration of `loop`, the end of `sleep`. A `trap` block runs there; TERM, HUP and INT with no trap raise `SignalException` / `Interrupt`, so `ensure` clauses and `at_exit` blocks run, and rsh ends by that signal (status 128+N, as sh shows it). Code that reaches no safe point (`while true; end`) is ended by the signal itself after 2 seconds, or by a second one a second later. TERM or HUP that comes while a foreground command runs is passed on to that command, and the script ends when it does. In the interactive shell Ctrl-C interrupts Ruby code (the shell goes on) and TERM ends it the same way. `exit` or Ctrl-D with jobs running says so once; the second one leaves. Typed-ahead and pasted lines are kept and run one after the other; a long paste is drawn once, not once per character. Tab completion quotes a name so that it stays one word (`sp\ ace`), works inside an open quote and after `|`, `;`, `&&`, `||` and `(`; Ctrl-R followed by an arrow or Ctrl-A leaves the search and does the move; ESC then Enter is Enter. `gets`, `readline`, `readlines`, `each_line` and `File.foreach` / `File.readlines` take `chomp: true`. The first program of a foreground job takes the terminal itself before it runs (a program that reads at once is no longer stopped by SIGTTIN).

## Sh: the shell things as Ruby values

`Sh` is built in (no `require`; it loads the first time it is used, so start-up does not pay for it). Each helper works on the files itself, starts no program and gives a Ruby value: an Array of paths you can hand to a command (`rm #{list}`, one argument each), a String, `true` or `false`. The set is small on purpose; anything more is the real program, which a command line still runs (`find . -newer x -printf ...`).

```ruby
# pack the logs older than a week, then say what is left
old = Sh.find("logs", name: "*.log", older: 7 * 86400)
if old.any?
  tar -czf old-logs.tgz #{old}
  Sh.rm(old)
end
puts "packed #{old.size}, kept #{Sh.find("logs", type: :file).size}"
Sh.tmpdir do |dir|
  Sh.cp_r("logs", dir)
  puts Sh.grep("ok", Sh.find(dir, name: "*.log"))
end
abort "rsync is missing" unless Sh.which("rsync")
```

| helper | gives |
|---|---|
| `Sh.find(dir = ".", name:, type:, older:, newer:, maxdepth:, hidden: true)` | the paths below `dir`, sorted; `name` a pattern on the file name (`*.log`, `*.{c,h}`), `type` `:file`, `:dir` or `:link`, `older`/`newer` seconds or a `Time`, `maxdepth` 1 for `dir`'s own entries; links are not followed; a folder that cannot be read is said on standard error and skipped, as find(1) (also in `Sh.du` and `Find.find`) |
| `Sh.grep(pattern, *files)` | the lines that match (a String is plain text), `file:` in front with more than one file |
| `Sh.which(name)` | the program's full path on `PATH`, or `nil` |
| `Sh.exists?`, `file?`, `dir?`, `age` (seconds since the last change), `size`, `du` (bytes below) | about one path |
| `Sh.read`, `write`, `append`, `lines` | a file's text |
| `Sh.mkdir_p`, `touch`, `cp`, `cp_r`, `mv`, `ln_s`, `chmod`, `rm`, `rm_rf` | as the programs; `rm_rf` never follows a link, a missing path is fine, and it refuses `""`, `/`, `.`, `..` and any path that resolves to `/` or the current folder (ArgumentError, nothing removed); `cp` onto the same file is ArgumentError and writes a new file renamed over the old one; `cp_r` into itself is ArgumentError; `rm` of a missing file is `Errno::ENOENT` |
| `Sh.tmpdir(prefix = "rsh") { \|dir\| }` | a new folder only you can read, removed after the block (also when it raises) |

The Ruby-standard names are built in too, for people who know them: `FileUtils` (`mkdir_p` `makedirs` `mkpath` `mkdir` `rmdir` `rm` `rm_f` `rm_r` `rm_rf` `rmtree` `cp` `cp_r` `mv` `ln_s` `touch` `chmod` `pwd`; an option such as `noop:` or `verbose:` is an ArgumentError, never ignored), `Find.find(*paths) { |path| }` with `Find.prune`, `Shellwords` (`escape`, `split`, `join`; `String#shellescape`, `#shellsplit`, `Array#shelljoin`: text for another computer's shell, as `ssh` hands it on) and `Dir.mktmpdir`. `require "fileutils"`, `"find"`, `"shellwords"` and `"tmpdir"` work and do nothing more. The names are frozen (`docs/design.md` S8, `tests/contract.sh` "STD_SHA"); `tests/golden/r9_sh.rsh` has a case for each.

## What is inside

The interpreter is built into the program (named in `Makefile` and `build_config.rb`, credited with its licence in the package). It is fetched at one pinned commit and checked by sha256 and never kept in the tree. In for metaprogramming: `eval`, `binding`, `instance_eval`/`class_eval` with a string, `ObjectSpace`. Left out on purpose: sockets (network goes through commands: `curl`, `wol`), big and rational numbers. Not in the interpreter: refinements (`refine`, `using`), `defined?`, `$!` (it stays `nil`; use `rescue => e`), the block parameter `it` (`_1` works), `TracePoint`, `ObjectSpace::WeakMap`, `const_source_location`; `ObjectSpace.each_object` needs a block. Added by rsh: `warn`, `abort`, `__dir__`, `File.mtime`, (0.9.0) `File.stat`, `File.lstat`, `File.ftype` and `File::Stat`, `File.readable?`, `writable?`, `executable?`, `owned?` (effective ids, as Ruby), `Process.uid`, `euid`, `gid`, `egid`, and with `require "time"` `Time#iso8601`/`xmlschema`, `Time.iso8601` and `Time.parse` for ISO 8601 (another form is ArgumentError), `File.expand_path("~user")` from `/etc/passwd` (not the C library's account modules), `private_method_defined?`, `public_method_defined?`, `protected_method_defined?`, `Data#with`. Written for rsh: process spawning with redirects, process groups and job control, `ENV` kept in step with the process environment, `Dir.glob`, `File.write`, `File.readlines`, JSON (`JSON.parse`, `to_json`, `JSON.pretty_generate`), and `Regexp` over the system's regular-expression library (named groups, look-ahead, lazy quantifiers; `=~ match match? MatchData $~ $1`, `sub gsub scan split`, `str[/re/]` and `str[/re/, group]`). The Ruby side of Regexp loads the first time a pattern is made or `scan`, `match`, `match?`, `=~` or one of `Regexp`'s own methods is called, so a script without patterns does not read it at start-up. `require` knows only `json set time etc fileutils find shellwords tmpdir` (all built in; the last four are `FileUtils`, `Find`, `Shellwords` and `Dir.mktmpdir`, see "Sh"); your own files load with `require_relative`. A match has limits (1,000,000 steps, 256 MB of backtracking): a pattern that would need more on a text raises `RegexpError` ("too complex") instead of running for seconds. Strings and Arrays have no fixed size cap (the interpreter's default cap of 131072 Array entries is off).

Interactive (`rsh` on a terminal, `-i`, or `-l`, which the terminal passes): a line editor (UTF-8 and wide-character widths, Emacs keys, history in `~/.local/state/rsh/history`, mode 0600, Ctrl-R search), Tab completion (builtins, programs on `PATH`, files, your variables and methods), more input while the Ruby is not finished, job control (own process group per job, Ctrl-Z, `fg`, `bg`, `[1]+ Done`), and colours while you type from the same reader that runs the line: the command word is green when found and red when not, keywords, strings and numbers have their own colours. `~/.config/rsh/init.rsh` runs first; `def prompt` in it gives the prompt. History does not keep a line that starts with a space or looks like it holds a secret (`export GITHUB_TOKEN=...`, `--password=...`, an Authorization header, a known token prefix). init.rsh and the history file are used only when they are plain files (never through a link), yours, and not writable by others; an older history file is set to 0600. As root (or set-user-id) the home comes from `/etc/passwd`, `$HOME` and `$XDG_*` are not used for these files, and a file its group can write is refused too, so `sudo -E rsh` never runs another person's init.rsh. File names, the current folder and history entries are shown with control characters made visible (`^[`), so a name cannot send escape sequences to the terminal.

## Build and size

`make` builds `build/rsh` (needs a C and C++ compiler, `ruby` and `bison` for the interpreter's own build script, and `libpcre2-8`; `LINK=static` is the default and needs `libpcre2-8.a`, `LINK=dynamic` is for distributions without it). `make test` runs the unit checks, the golden scripts, the stability contract (`tests/contract.sh`, `docs/STABILITY.md`) and then all of them again on the sanitizer build (`make asan`: `LINK=dynamic`, `-fsanitize=address,undefined`, leak check on; any report fails); `make bench` the size and start-up. `LINK=static` (the default) is one file with no shared libraries; the WolOS package builds `LINK=dynamic` (glibc and libpcre2-8 shared). A Makefile variable hands in a local copy of the interpreter source (the package build is offline).

Measured on the AWS build box (2026-11-04, x86_64, static, stripped, `-Os`): 2,895,976 bytes; start-up of `rsh -e 'puts 1'` median 2.45 ms (20 runs, min 2.0, max 3.0); `rsh -c nil` median 1.75 ms, p95 2.47 ms (200 runs); a one-line script median 2.32 ms. 0.1.1 (2026-11-04, same box, with eval, binding and ObjectSpace): 2,920,552 bytes; `rsh -c nil` median 1.08 ms against 1.01 ms for 0.1.0 (400 interleaved runs each). 0.1.2 (2026-11-04, a 32-core build box with other jobs running, timed by a small fork-and-wait program, 400 interleaved runs, three rounds): 2,928,744 bytes; `rsh -c nil` median 1.16-1.17 ms, 0.1.0 1.15-1.17 ms, 0.1.1 1.19-1.20 ms; the Ruby side of Regexp now loads on first use, so `rsh -c 'x = "a1" =~ /\d/'` is 1.31-1.33 ms against 1.26-1.27 ms for 0.1.1. 0.1.3 (2026-11-04, the same box, fork-and-wait timer, 400 interleaved runs against 0.1.2): see `CHANGELOG.md`. Limits: under 3 MB, under 10 ms.

0.1.4 speed (`bench/run.sh`, 2026-11-04, a loaded 32-core build box; `make test` runs its gate; `BENCH_SLACK=N` multiplies the time limits on a slow machine, and a busy one gets x5 by itself): one validation of the text per Regexp call and a twin of the match for invalid UTF-8 instead of one per match, no per-call Arrays, and the print-only builtins without a fork. `scan(/\w+/)` on 686 KB 0.05 s (84.6 s in 0.1.3), `split(/ /)` 0.05 s (64 s), `gsub(/foo/, "F")` 0.007 s (9.7 s), a million `=~` with `$1` 1.4 s (13.0 s), `match?` 0.3 s, a pattern per line over 100,000 lines 0.24 s and 17 MB (86 MB), `sub(/re/)` in a Ruby stage over 100,000 lines 0.21 s (3.34 s), 1000 `echo` 0.009 s (0.14 s). `rsh -c nil` start-up is within a few percent of 0.1.3 (the signal handlers and the Ruby side of `Signal`, `trap`, `at_exit`, `Dir.home` and `Process.kill` load on first use, as the Regexp side does).

## Tests

- `tests/unit.cpp`: the reader (statement starts, the command-line rule, the grammar), patterns, argument safety, the `.rb` rule. `make test`.
- `tests/golden/*.rsh` and `*.rb` with `.out` (standard output and a last line `status: N`), `.explain` (`rsh --explain`) and `.err` (standard error): classification, one-argument interpolation, pipelines, redirects, `&&`/`||`, patterns, here-documents, `Rsh::Failed`.
- `tests/golden/meta_*.rsh`, `meta_*.rb`: metaprogramming (define_method, method_missing both ways, instance_eval/class_eval, eval and Rsh.eval, hooks, const_missing, ObjectSpace, procs, Struct, Data, `ruby do`, `ruby!`, `rsh_methods`), with and without the shell layer. `examples/meta/*.rsh` run with expected output in `tests/examples/meta/`.
- `tests/examples/*.rsh`: five whole scripts (deploy, backup, log scan, remote run, a pipeline with Ruby map and filter) with expected output, run with stand-ins for `git`, `make`, `rsync`, `systemctl` and `ssh` from `tests/fakebin/`.
- `tests/golden/readme_*.rsh`: every ```` ```ruby ```` block of `README.md`, as printed there (`tests/run.sh` fails when a block is in none of them; a test may add lines before or after it), run with the stand-ins in `tests/fakebin/readme/` (`curl`, `ssh`, `nft`, `make`, `df`, `sleep`). A `NAME.setup` shell script next to any test makes the files it works on, in its empty directory.
- `tests/run.sh` runs both (`RSH=path sh run.sh [--update] [name ...]`; `--update` rewrites the expected files, read the diff before keeping it).
- `tests/unit_regexp.cpp`: the Regexp twin for invalid UTF-8 against the engine's own matching on about 2 million random subjects and patterns (`make test`, both builds).
- `bench/run.sh` (`--gate` in `make test`): the speed and memory table of the 0.1.4 performance work with a limit per row; `bench/gen.sh` makes its fixed texts.
- `tests/golden/r2_*.rsh`: the reproducers of the 0.1.4 correctness audit (BOM, CRLF, signals and `at_exit`, `cmd || exit 1`, the reader cases, sh hints, `Dir.home`, `chomp:`, `echo` write errors).
- `tests/interactive.rb` drives the editor through a pseudo-terminal (needs `ruby`, skipped without it).
- `tests/doctrine.sh`: no upstream project names in the program's own text; the size limit (3 MB) when a binary is built.
- `tests/golden/sec_*.rsh`: the reproducers of the 0.1.3 security and stability audit, one per finding (C1 the command-line table, C2 no `/bin/sh`, H1 `umask`, H2 NUL bytes, M1 the Ruby words, M2 `./-name`, M5 jobs, M6 `exec`, M7 pattern limits, M8 loads, the lows); M3, M4 and H3 are in `tests/interactive.rb` and `tests/unit.cpp`.
- `tests/golden/r9_*.rsh` (0.9.0): `r9_script.rsh` the script-interpreter fixes (G2, G7, G9, G12, G13), `r9_spec.rsh` the rules of `docs/design.md` that had no case of their own; the other 0.9.0 fixes are `tests/run.sh` cli cases G1, G3, G5, G10, G11 and M1 (memory of a 20,000-input session).
- `tests/frozen.explain`: `--explain` of every test script and example, checked by `tests/contract.sh`.
- `tests/fuzz/reader.cpp`, `make fuzz` (clang): the reader under libFuzzer with the sanitizers (`docs/STABILITY.md` section 12).
- `tests/contract.sh`: the stability contract (`docs/STABILITY.md`): flags, exit statuses, message formats, value safety, the `--explain` of `classify.rsh`, the Ruby words, file names, build inputs, the release gate.

## Not done yet

The known limits K1-K8 of `docs/STABILITY.md`, section 10.

- Ctrl-Z / `fg` / `bg` are written but `tests/interactive.rb` only covers `&`, `jobs`, editing, completion, continuation lines, history search and colours.
- Static linking still warns that the interpreter's file library calls the account database; since 0.1.3 nothing reaches that call (`File.expand_path("~user")` reads `/etc/passwd`).
- Not in the interpreter: `DATA` after `__END__`, `GC.count`, a source string literal over about 1 MB (the interpreter's parser buffer), `Array#slice_when`, `Range#step`, `Hash#compare_by_identity`, `Exception#cause`, `String#sum`, `Integer#[]`, `Time#round`, `sprintf` positional `%1$s`, and a `protected` method called from another instance of its class.
- The editor has no Alt-D, Ctrl-Y (kill ring), Ctrl-T or Ctrl-V; completion offers no `$VAR`, constants or methods, and prints every candidate in one line.
- A program killed by SIGPIPE when rsh's own output is a closed pipe (`rsh -c 'seq 1 200000' | head -1`) is `seq was stopped by signal 13`, status 141: it is a failed command like any other (decided, not changed).
