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

- `rsh script.rsh`, `./script.rsh` with the shebang, `rsh script.rb` (plain Ruby files run too), `rsh -e 'puts 1'`.
- Interactive: line editing, completion, jobs, history, a coloured prompt. `rsh -l` as a login shell.
- One static binary under 3 MB, about 2.5 ms to start.
- `rsh --explain file` shows which lines are Ruby and which are commands.

## Build

```sh
make          # fetches the pinned interpreter sources (sha256 checked) and builds build/rsh
make test     # unit checks, golden scripts, examples, interactive test
```

Needs a C++17 compiler, `make`, `bison`, `ruby` (for the interpreter's own build) and `pcre2`.

## Layout

- `src/` the shell layer (transpiler, runner, line editor), `src/prelude.rb` the Ruby side.
- `tests/golden/`, `tests/examples/`, `tests/interactive.rb`, `tests/run.sh`.
- `rsh.1` the manual page. `docs/rsh.md` the long description, `docs/design.md` the design and its options.
- `packaging.PKGBUILD` the WolOS package.

On WolOS, `wol script run|check|on` accepts `.rsh` files, and rsh is a selectable terminal shell.

## Licence

GPL-2.0-or-later.
