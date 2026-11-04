# rsh: RubyShell

Ruby with a shell layer, in one small program.

A script is Ruby. A line whose first word Ruby does not know is a command line, with pipes, redirects, globs, here-docs, `&&` and `||`. A Ruby value inside a command is always exactly one argument, and nothing ever goes through `sh`, so there is no quoting trap and no injection. A failing command raises `Rsh::Failed`, which you can rescue; `relaxed!` turns that off.

```ruby
#!/usr/bin/rsh
dest = ARGV[0] || "/srv/app"
cd ~/src/app
git pull --ff-only && make -j#{Etc.nprocessors}
rsync -a build/ #{dest}/
errors = `journalctl -b -p err`.lines.reject { |l| l =~ /fwupd/ }
puts "#{errors.size} errors" unless errors.empty?
```

- `rsh script.rsh`, `./script.rsh` with the shebang, `rsh -e 'puts 1'`.
- `rsh script.rb` runs a plain Ruby file with the shell layer off (see below).
- Interactive: line editing, completion, jobs, history, a coloured prompt. `rsh -l` as a login shell.
- One static binary under 3 MB, about 2.5 ms to start.
- `rsh --explain file` shows the mode, and which lines are Ruby and which are commands.

## Ruby files

A `.rb` file is plain Ruby: `rsh script.rb` runs it with the shell layer off, so an existing Ruby script means what it means in Ruby. A `.rb` file turns the shell layer on with the comment `# rsh: shell` as its first line (or the line after a `#!` line). `.rsh` files, files with no ending, `-e`, `-c` and standard input always have the shell layer.

```ruby
# script.rb: no command lines here, this is Ruby
puts ARGV.map(&:upcase).join(" ")
```

```ruby
#!/usr/bin/rsh
# rsh: shell
git pull --ff-only && make
```

`rsh --explain script.rb` prints `mode: ruby` (or `mode: shell`) before the lines. The Ruby is the one built into rsh, a subset of the language: `docs/rsh.md` lists what it leaves out.

## Build

```sh
make          # fetches the pinned interpreter sources (sha256 checked) and builds build/rsh
make test     # unit checks, golden scripts, examples, interactive test
make install PREFIX=/usr DESTDIR=stage   # build/rsh -> stage/usr/bin/rsh, rsh.1 -> stage/usr/share/man/man1
```

`PREFIX` defaults to `/usr/local`; `DESTDIR` is for packages. `LINK=dynamic` links glibc and pcre2 shared instead of static. `make uninstall` removes both files. The version is in `VERSION`, changes in `CHANGELOG.md`. CI (`.github/workflows/ci.yml`) builds, runs `make test`, checks the 3 MB limit and uploads the binary.

Needs a C++17 compiler, `make`, `bison`, `ruby` (for the interpreter's own build) and `pcre2`.

## Layout

- `src/` the shell layer (transpiler, runner, line editor), `src/prelude.rb` the Ruby side.
- `tests/golden/`, `tests/examples/`, `tests/interactive.rb`, `tests/run.sh`.
- `rsh.1` the manual page. `docs/rsh.md` the long description, `docs/design.md` the design and its options.
- `packaging.PKGBUILD` the package build file, `CHANGELOG.md`, `VERSION`.

On WolOS, `wol script run|check|on` accepts `.rsh` files, and `.rb` files with an rsh shebang (`#!/usr/bin/rsh`); rsh is a selectable terminal shell.

## Licence

GPL-2.0-or-later.
