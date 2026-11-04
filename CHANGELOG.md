# Changelog

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
