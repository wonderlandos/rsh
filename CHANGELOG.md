# Changelog

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
