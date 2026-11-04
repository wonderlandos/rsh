# 0.9.1, audit 2 H3 (the CTO's decision): after && and || the next word is read by the same rules
# as at the start of a statement (docs/design.md S1.3 rules 2-5, S1.4, S1.5, S2.8).
def note(text)
  puts "note: #{text}"
end
n = 0
list = []
$ true && n += 1
$ true && n = n + 1
$ true && n=n+1
$ true && list << "pushed"
p n, list
$ true && note "a method of the file"
$ true && note("with parentheses")
$ false || puts("puts with parentheses")
$ false || puts "puts as Ruby's puts (it was the program puts, 127)"
$ true && STDOUT.puts "a Capitalised receiver"
$ true && self.note "self.name"
$ true && $ echo "the escape hatch forces a command"
$ true && echo "a plain word is still a command"
$ true && FOO=env-prefix sh -c 'echo "NAME=value cmd: $FOO"'
$ true && ./no-such-path-zz || echo "a path is a command: $?"
$ true && echo =x
$ false || nil
$ false || true
p $?.exitstatus
sh -c 'exit 1' || exit 0
