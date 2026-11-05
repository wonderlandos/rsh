<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="brand/rsh-wordmark-dark.svg">
    <img src="brand/rsh-wordmark.svg" alt="rsh" width="320">
  </picture>
</p>

**rsh (RubyShell) is Ruby that also runs command lines: one script holds both the logic and the commands.**

```ruby
#!/usr/bin/rsh
# tidy: pack the logs older than a week, then warn about full disks
old = `find logs -name '*.log' -mtime +7 -print0`.split("\0")
if old.any?
  tar -czf old-logs.tgz #{old} && rm #{old}
end
puts "packed #{old.size} files"

usage = `df -P`.lines.drop(1).map(&:split).to_h { |f| [f[5], f[4].to_i] }
full = usage.select { |mount, pct| pct > 90 }.keys
$stderr.puts "almost full: #{full.join(", ")}" if full.any?
```

Line 5 and the two back-quoted parts are commands; everything else is Ruby. `#{old}` gives `tar` one argument per file, so `logs/a b.log` stays one file. If `tar` fails, `rm` does not run (the left side of `&&` is a condition, as in `sh`); if `rm` fails, the script stops there with `rsh: tidy.rsh:5: rm exited 1`.

## Why

A shell script is quick to start and hard to get right: every value is text that the shell splits again, an error is easy to miss, and anything with structure (JSON, a table, a count per name) needs `jq`, `awk` or `sed`, each with its own language. Ruby has the structure but makes running a command a chore. rsh keeps the shell's command lines and gives them Ruby's values, blocks, hashes and regular expressions, in the same file and on the same line.

Here is the same job done both ways. Every rsh snippet below runs as shown.

### A file name with a space

```sh
# sh: "Summer Trip.JPG" becomes two words, mv gets three arguments
for f in $(ls pics/*.JPG); do mv $f ${f%.JPG}.jpg; done
```

```ruby
# rsh: each value is one argument
Dir.glob("pics/*.JPG").each do |photo|
  mv #{photo} #{photo.sub(/\.JPG\z/, ".jpg")}
end
```

### A loop over what find found

```sh
# sh: breaks on names with a new line or a backslash, and runs in a subshell
find src -name '*.txt' | while read f; do grep -c TODO "$f"; done
```

```ruby
# rsh: split on the zero byte, then plain Ruby
`find src -name '*.txt' -print0`.split("\0").each do |file|
  n = File.read(file).scan(/TODO/).size
  puts "#{n} #{file}" if n > 0
end
```

### Stopping on an error

```sh
# sh: set -e is silently off inside a function called from if, and after a !
set -e
build() { make; make test; }
if ! build; then echo "failed" >&2; exit 1; fi
```

```ruby
# rsh: a failing command raises Rsh::Failed, with what it ran and what it said
begin
  make
  make test
rescue Rsh::Failed => e
  $stderr.puts "#{e.argv.join(" ")} failed with #{e.status.exitstatus}: #{e.stderr.strip}"
  exit 1
end
```

### JSON from a web service

```sh
# sh: needs jq and its own query language
curl -fsS "$api/issues" | jq -r '.[] | select(any(.labels[]; .name == "bug")) | "#\(.number) \(.title)"'
```

```ruby
# rsh: JSON is built in
require "json"
issues = JSON.parse(`curl -fsS https://api.github.com/repos/wonderlandos/rsh/issues`)
bugs = issues.select { |i| i["labels"].any? { |l| l["name"] == "bug" } }
bugs.each { |i| puts "##{i["number"]} #{i["title"]}" }
```

### Counting in a log, then acting on it

```sh
# sh: awk for the counting, a while loop for the acting
awk '/Failed password/ { n[$(NF-3)]++ } END { for (ip in n) if (n[ip] >= 2) print ip }' auth.log |
  while read -r ip; do nft add element inet filter blocked "{ $ip }"; done
```

```ruby
# rsh: a regular expression and a hash, then a command per address
seen = Hash.new(0)
`cat auth.log`.scan(/Failed password for (\S+) from (\S+)/) { |user, ip| seen[ip] += 1 }
seen.select { |ip, n| n >= 2 }.each_key do |ip|
  nft add element inet filter blocked "{ #{ip} }"
end
```

A Ruby block can also be a stage of a pipeline: a String it returns replaces the line, `nil` drops it.

```ruby
cat auth.log | { |l| $1 if l =~ /Failed password .* from (\S+)/ } | sort | uniq -c | sort -rn
```

### Try again, waiting longer each time

```sh
# sh: a counter, a test and arithmetic, written again in every script
n=0
until curl -fsS -o page.html "$url"; do
  n=$((n + 1)); [ "$n" -ge 5 ] && { echo "gave up" >&2; exit 1; }
  sleep $((2 ** n))
done
```

```ruby
# rsh: write it once as a method that takes a block
def with_retries(times: 5)
  times.times do |n|
    return true if yield
    sleep #{2 ** n}
  end
  false
end
with_retries { (curl -fsSO #{ARGV[0]}) } or raise "gave up on #{ARGV[0]}"
```

A command line in brackets is a value: `true` if it worked, `false` if not.

### The same check on several computers

```sh
# sh: you learn that something failed, not where or why
for h in web1 web2 db1; do ssh "$h" systemctl is-active --quiet nginx || down="$down $h"; done
```

rsh rescues each one and keeps the reason:

```ruby
down = []
%w[web1 web2 db1].each do |host|
  begin
    ssh -o BatchMode=yes #{host} systemctl is-active --quiet nginx
  rescue Rsh::Failed => e
    down << "#{host} (#{e.status.exitstatus})"
  end
end
puts down.empty? ? "all up" : "down: #{down.join(", ")}"
```

## Install

From the release source (needs a C++20 compiler, `make`, `bison`, `ruby` and `pcre2`; `ruby` is only used while building):

```sh
curl -fsSLO https://github.com/wonderlandos/rsh/archive/refs/tags/v0.9.1.tar.gz
tar -xzf v0.9.1.tar.gz && cd rsh-0.9.1
make && make test
sudo make install            # /usr/local/bin/rsh and its manual page
```

`make` downloads the Ruby it builds in, at one fixed version, and checks its sha256. From a clone: `make`, then `make install PREFIX=/usr` (or `DESTDIR=stage` for a package). `make uninstall` removes both files.

On WolOS: `wol pkg -- -S rsh`. `wol script run|check|on` then accepts `.rsh` and `.rb` files.

The result is one program, about 3 MB (3.0 to 3.3 MB, by compiler), that starts in about 1.2 ms.

## Run

| What | How |
| --- | --- |
| a script | `rsh deploy.rsh`, or `./deploy.rsh` with `#!/usr/bin/rsh` as its first line |
| a Ruby file | `rsh tool.rb`: `.rb` files mix Ruby and commands the same way |
| a Ruby file as plain Ruby | first line `# rsh: ruby` (after the `#!` line, if any): then no line is a command |
| one line | `rsh -e 'puts Dir.pwd'` (`-c` is the same) |
| interactive | `rsh` on a terminal: line editing, Tab completion, history (Ctrl-R), jobs, colours as you type (`-l` is the same as `-i`; rsh is a script interpreter, not meant as a login shell) |
| check without running | `rsh -n file`, `rsh --lint file` |
| see how each line is read | `rsh --explain file` |

`~/.config/rsh/init.rsh` runs first in the interactive shell; a `def prompt` there sets the prompt.

## How a line is read

1. Only the start of a statement counts: the start of a line, after `;`, `then`, `do`, `else`, `{`, and after `if`, `unless`, `while`, `until`.
2. If the first word is a path (`./build`, `/usr/bin/env`, `~/bin/x`), the line is a command.
3. If the first word is something Ruby knows (a keyword, a local variable, a method from the file, `puts` and friends, a Capitalised name), the line is Ruby.
4. If the word is followed by `.`, `(`, `[`, `=` or `,`, the line is Ruby: `ls.size` is Ruby, `ls -l` is a command.
5. Otherwise it is a command. `$ printf ...` forces a command, `printf(...)` forces Ruby, and `rsh --explain` shows the choice for every line.

The rule looks only at the text, so a script means the same on every computer.

## Safety

- **One value, one argument.** `#{name}` in a command is exactly one argument, whatever it holds: spaces, quotes, `*`, `;` or a new line. An Array gives one argument per element. A value is never split again or expanded as a pattern, and `nil` is an error, not an empty word.
- **Nothing goes through `sh`.** Command lines are read once, when the file is read, and run directly. Back-quotes use the same reader, not `/bin/sh`. `system("prog", "arg")` takes separate arguments. `open("|cmd")` and `IO.popen` are errors, and a NUL byte in an argument is an error, not a shorter argument. (`ssh` still hands its command to the other computer's shell: that is how `ssh` works.)
- **Failures are not silent.** A command that fails raises `Rsh::Failed` with `status`, `argv` and the end of its error output, unless it is a condition (`if`, `while`, left of `&&` or `||`). Uncaught, the script stops with the file, the line and the exit status. `|| true` ignores one failure, `relaxed!` turns raising off for the rest of the script, and `$?` is set either way.
- **No match is an error.** `rm *.tmp` with no `.tmp` file stops instead of removing a file named `*.tmp` (`Rsh.nullglob = true` changes that).
- `rsh --lint` warns about commands that are not on `PATH`, `sh -c` text with a value inside, world-writable `chmod`, fixed `/tmp` names and a value as the first argument of a program (write `rm -- #{f}`); it exits 0 clean, 1 with warnings, 2 with errors.
- Stable: the flags, exit statuses, messages, the reading rule and its word list are frozen ([docs/STABILITY.md](docs/STABILITY.md)); `make test` checks them and runs every test again under the address and undefined-behaviour sanitizers.

## Metaprogramming

Ruby's metaprogramming works in both modes: `define_method`, `method_missing` and `respond_to_missing?`, `send` and `public_send`, `instance_eval` and `class_eval` (with a block or a string), `eval` and `binding`, `prepend`, `include` and `extend` with their hooks, `const_missing`, `ObjectSpace`, procs, lambdas and currying, `Struct`, `Data` and `Comparable`. Refinements (`refine`, `using`) are not in the built-in interpreter.

One thing needs care in a file with the shell layer. rsh decides which lines are command lines when it reads the file, before anything runs, so it cannot see methods that only exist at run time. A bare word at the start of a statement is a command unless the file makes it a Ruby method in a way rsh can read:

- `def name`, or `define_method(:name)`, `attr_reader :name` (and `attr_accessor`, `attr_writer`), `alias_method :name, ...`, `Struct.new(:name, ...)`, `Data.define(:name, ...)`, with the name written out;
- `rsh_methods :name, :other`: for methods made by `method_missing`, or by `define_method` over a list;
- `ruby do ... end` or `ruby { ... }` around code: no bare word inside is a command;
- `ruby!` on its own line: no bare word is a command from there to the end of the file.

`$ cmd` still runs a program in all three places. `rsh --explain` lists the names it reads as methods and the Ruby-only lines.

```ruby
class Shell                                  # unknown methods run programs
  def method_missing(name, *args)
    ($ #{name} #{args})                      # every argument stays one word: no shell, no splitting
  end
end
Shell.new.echo "a b; rm -rf /"               # prints: a b; rm -rf /

rsh_methods :server, :role                   # a DSL answered by method_missing
Config.new.instance_eval do                 # Config has a method_missing
  server "web 1"                             # a Ruby call, not the program `server`
  echo configured                            # still a command line
end
```

`eval` is plain Ruby and never runs a command line. `Rsh.eval(text)` reads its text with the shell layer and runs it (at the top level, or with `Rsh.eval(text, binding)` where that binding is). [`examples/meta/`](examples/meta) has two whole scripts: a deploy language made with `method_missing`, and a task runner whose tasks are made with `define_method`.

## Status

rsh 0.9.0 is the release candidate for 1.0 (with `Sh`, the shell things as Ruby values: `Sh.find("logs", name: "*.log")`, `Sh.rm_rf`, `Sh.tmpdir { }`, and `FileUtils`, `Find`, `Shellwords` built in; [docs/rsh.md](docs/rsh.md)), a version that then changes only for a serious security problem, a large speed-up or a large missing function. [docs/design.md](docs/design.md) is the specification (every rule names its test) and [docs/STABILITY.md](docs/STABILITY.md) the contract: what is frozen (every 1.0 item is met), the known limits and the measured numbers (a 30-minute fuzz of the reader and an hour of the sanitizer build, both clean; no memory growth over 10,000 commands; start-up about 2 ms). Not done yet:

- The Ruby inside rsh is a smaller Ruby: no sockets (use `curl`, `ssh` or `wol`), no big or rational numbers, and `require` knows only `json`, `set`, `time`, `etc`, `fileutils`, `find`, `shellwords` and `tmpdir` (built in); your own files load with `require_relative`.
- Ctrl-Z, `fg` and `bg` work but are not yet covered by the terminal test.
- Packages exist for x86_64 only.

Full description: [docs/rsh.md](docs/rsh.md). Specification: [docs/design.md](docs/design.md); the first design and the options weighed: [docs/design-history.md](docs/design-history.md). Manual page: `man rsh` ([rsh.1](rsh.1)). Changes: [CHANGELOG.md](CHANGELOG.md). Brand: [docs/brand.md](docs/brand.md).

## Licence

GPL-2.0-or-later (see [LICENSE](LICENSE)).
