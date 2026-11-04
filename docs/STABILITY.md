# rsh stability contract

rsh is a tool that is rarely updated. From 0.1.3 the items below are frozen: a script, a program that
reads rsh's output, or a package that builds it can depend on them. Changing any of them is a major
version (1.0, 2.0); a minor or patch version only fixes mistakes and adds things that do not change
these. `tests/contract.sh` fails when one of them changes; `make test` runs it.

Where an item names a file, that file is the reference; this page says what is frozen in it.

## 1. How a line is read (the classification rule)

The five rules of `docs/rsh.md`, "How a line is read", decide whether a statement is Ruby or a
command line. They look only at the text of the file, never at `PATH` or the machine.

- The escape hatches: a leading `$ ` forces a command line; `name(...)` and `self.name` force Ruby;
  `ruby do ... end` and `ruby { ... }` make every statement inside Ruby; `ruby!` alone on its line does
  the same to the end of the file; `# rsh: ruby` as the first line (or the line after `#!`) turns the
  shell layer off for the file, `# rsh: shell` there says the default.
- The Ruby words (rule 4) are one list, `RUBY_WORDS` in `src/transpile.cpp`. The list is frozen as it
  is in 0.1.3; it only grows, and only in a major version. `sleep` is a command, not a Ruby word.
- The built-in commands are the list `BUILTINS` in `src/exec.cpp`: `cd pushd popd dirs export unset
  exec umask jobs fg bg wait source echo type pwd history`.
- `tests/golden/classify.rsh` with `tests/golden/classify.explain` is the frozen proof of every case.

## 2. The `--explain` format

```
mode: shell                                  (or: mode: ruby (the shell layer is off: ...))
methods: NAME NAME ...                       (only when there are any)
ruby only: line N (a ruby block)             (one line per span, as they occur)
ruby only: lines N-M (a ruby block)
ruby only: line N to the end (ruby!)
%4d K  TEXT                                  (every line of the file)
```

`K` is `R` (Ruby), `C` (a command line), `X` (a back-quote capture inside Ruby) or a blank for a blank
line. Machine readers may depend on this. The exit status is 0, or 2 when the file has a mistake.

## 3. Flags

`-c TEXT`, `-e TEXT`, `-s`, `--name FILE`, `-n`, `--lint`, `--explain`, `--ruby`, `-i`, `-l`, `-v`
(`--version`), `-h` (`--help`), `--`. No flag gets a new meaning. A new flag may be added in a minor
version; an unknown flag is an error (status 2). `rsh -h` prints the list (frozen text).

## 4. Exit status

| status | when |
|---|---|
| 0 | the script ended normally |
| N | a command failed and the script did not handle it: that command's status |
| 128+N | that command was ended by signal N |
| 130 | interrupted (Ctrl-C, `Interrupt`) |
| 1 | a Ruby error the script did not rescue |
| 2 | a syntax or reader mistake in the file; a bad flag |
| 127 | the script file cannot be read; a command that is not found |
| 126 | a command found but not runnable |
| 70 | the interpreter could not start |
| `exit N` | N |

`rsh -n` and `rsh --lint`: 0 clean, 1 warnings only (`--lint`), 2 errors. `wol script check` tells
warnings from errors by this.

## 5. Message format

- Every message of rsh is one line on standard error that starts with `rsh: `.
- A mistake at a place in a file: `rsh: FILE:LINE: TEXT`; a Ruby error adds ` (ClassName)`.
- An unhandled failed command: `rsh: FILE:LINE: PROG exited N` or `rsh: FILE:LINE: PROG was stopped
  by signal N`. `Rsh::Failed#message` is the text after `FILE:LINE: `.
- `-n` and `--lint`: `FILE:LINE: TEXT` and `FILE:LINE: warning: TEXT` (no `rsh: `, as compilers do).
- A command that is not found: `rsh: PROG: command not found`.

The texts after the place may be made clearer in a minor version; the shape above does not change.

## 6. Values in command lines

- One Ruby value is one argument; an Array is one argument per element; `nil` is an error.
- A NUL byte in an argument, an Array element, a `NAME=` value or a redirect target is an error (a
  here-document may hold one).
- A value is never split, never expanded as a pattern; a file found by a pattern whose name starts
  with `-` is given as `./-name`.
- No text ever reaches `/bin/sh`: command lines are parsed once, when the file is read; `system` takes
  separate arguments; `` Kernel#` `` with a run-time string, `IO.popen` and `open("|...")` are
  `Rsh::Error`, and the interpreter is built without popen.
- An empty part of `PATH` is skipped (it is not the current folder); with `PATH` unset rsh uses
  `/usr/local/bin:/usr/bin:/bin`; with `HOME` unset `~` is `/`.

## 7. Files

- Init: `$XDG_CONFIG_HOME/rsh/init.rsh` (else `~/.config/rsh/init.rsh`), interactive only.
- History: `$XDG_STATE_HOME/rsh/history` (else `~/.local/state/rsh/history`), mode 0600, one entry per
  line, a newline in an entry written `\n` and a backslash `\\`. A line that starts with a space, or
  holds what looks like a secret (`NAME=` with TOKEN, SECRET, PASSWORD, PASSWD, PASSPHRASE, API_KEY,
  APIKEY, PRIVATE_KEY, ACCESS_KEY, CREDENTIAL in the name; an Authorization or Bearer header; a known
  token prefix; a private key block), is not kept.
- Both files are used only when they are plain files (never through a link), owned by the effective
  user and not writable by others. As root, or when started set-user-id or set-group-id, rsh takes
  the home from `/etc/passwd` for the effective user, ignores `$HOME` and `$XDG_*` for these files,
  and also refuses files its group can write. Otherwise it says why on standard error and goes on
  without them.

## 8. Build inputs

- The interpreter: the commit and sha256 in `Makefile` (`MRUBY_COMMIT`, `MRUBY_SHA256`).
- What is compiled into it: the gem list and the defines in `build_config.rb` (`MRB_UTF8_STRING`,
  `MRB_STR_LENGTH_MAX=0`, `MRB_ARY_LENGTH_MAX=0`, `MRB_NO_IO_POPEN`).
- The only other library: pcre2 (8-bit), with a match limit of 1,000,000, a depth limit of 1,000,000
  and a heap limit of 256 MB per match (`src/rt.cpp`); no JIT.
- Link mode: `LINK=static` (the default; the CI artifact) is one file with no shared libraries.
  The WolOS package (`packaging.PKGBUILD`) ships `LINK=dynamic`: glibc and libpcre2-8 shared, the C++
  runtime linked in. Both are supported; the package is the one installed as `/usr/bin/rsh` on WolOS.
- CI actions are pinned to a commit.

Changing any of these is a major version; `tests/contract.sh` holds their digest.

## 9. Release gate

A release is tagged only when, on the build machine:

1. `make test` is green. It runs the unit checks, every golden and example, `tests/contract.sh`,
   the doctrine and size check (under 3 MB) and the interactive checks, then builds `make asan`
   (`LINK=dynamic`, `-fsanitize=address,undefined`, leak check on) and runs the unit checks, every
   golden (the audit reproducers among them: `tests/golden/sec_*.rsh`) and the contract on it. Any
   sanitizer report fails the run. (`make test SANITIZE=0` leaves the sanitizer half out for a host
   without the sanitizer libraries; such a run is not a release gate.)
2. `make bench` shows start-up within 5% of the previous release (median of 400 runs of
   `rsh -c nil`), and the size under 3 MB.
3. `CHANGELOG.md` names every change; `VERSION`, `packaging.PKGBUILD` (`pkgver`, and its sha256 once
   the tag exists) agree.
