# 0.9.0: the rules of docs/design.md (the specification) that had no golden case of their own; each
# block names its rule (S-number).
# S2.1 words and quotes: '' is literal, "" interpolates Ruby, \x escapes one character, a word is glued.
name = "w o"
$ printf '[%s]\n' 'a  #{name}' "b #{name}" c\ d "x"'y'z
# S2.2 one value is one argument; an Array is one argument per element.
list = ["one two", "three"]
$ printf '<%s>\n' #{list} #{name}
# S2.3 $NAME and ${NAME} from ENV; $* is every script argument; $? the last status.
ENV["SPEC_V"] = "env value"
$ printf '%s|%s\n' $SPEC_V ${SPEC_V}x
$ printf '[%s]\n' $*
$ false || echo "status after false: $?"
sh -c 'exit 3' || echo "status after exit 3: $?, then #{$?.inspect} (a value is read when the line starts)"
# S2.4 ~ and ~user; patterns * ? [..] {a,b} ** on unquoted text only.
echo ~root ~/sub
mkdir -p d/e
touch d/a.txt d/b.log d/e/c.txt
echo d/*.txt d/?.log d/[ab].* d/{a,b}.* d/**/*.txt "d/*"
# S2.5 NAME=value cmd sets NAME for that command only.
SPEC_ONE=only-here sh -c 'echo "inside: $SPEC_ONE"'
p ENV["SPEC_ONE"]
# S2.6 pipelines: |, |& (standard error too), a Ruby stage; $?.statuses of every stage.
printf_ok = 1
sh -c 'echo out; echo err >&2' |& sort
seq 4 | { |l| (l.to_i * 10).to_s if l.to_i.even? } | cat
ok = ($ sh -c 'exit 3' | cat)
p [ok, $?.exitstatus, $?.statuses]
# S2.7 redirects: > >> < 2> 2>> 2>&1 &> <<< and here-documents.
echo first > r.txt
echo second >> r.txt
wc -l < r.txt
sh -c 'echo e1 >&2' 2> e.txt
sh -c 'echo e2 >&2' 2>> e.txt
cat e.txt
sh -c 'echo both; echo both-err >&2' &> b.txt
sort b.txt
sh -c 'echo merged >&2' 2>&1 | tr a-z A-Z
tr a-z A-Z <<< "here #{name}"
# S2.8 lists: && || ; and ! (the status turned round); a line goes on after \ and after a final |.
$ true && echo and-ran; $ false || echo or-ran
! grep -q zz-nothing r.txt && echo not-found
echo continued \
  line
echo piped |
  tr a-z A-Z
# S2.9 captures: `cmd` and %x{} use the same reader and give a String; $? is set.
out = `echo captured #{name}`
alt = %x{echo percent-x}
p [out, alt, $?.success?]
# S2.10 a command line is a value: true or false.
p [($ true), ($ false)]
# S3 builtins: cd - and cd dir do ... end, pushd popd dirs, type, export/unset, umask, wait, $! in a line.
start = Dir.pwd
cd d
cd -
puts Dir.pwd == start
cd d do
  puts File.basename(Dir.pwd)
end
pushd d/e
popd
puts Dir.pwd == start
type cd
umask 027
umask
sleep 0.1 &
sh -c 'test -n "$1"' x $!
wait
# S3 system takes separate arguments; Rsh.run the same (true, or Rsh::Failed).
p system("sh", "-c", "exit 0")
p Rsh.run("sh", "-c", "exit 0")
# S2.3 $$ is rsh's process id.
sh -c 'test "$1" = "$2" && echo "pid matches"' x $$ #{Process.pid}
# S2.7 N> for another descriptor; exec with only redirects applies them to rsh itself.
sh -c 'echo to-three >&3' 3> three.txt
cat three.txt
exec 4> four.txt
sh -c 'echo via-four >&4'
cat four.txt
# S4 exec replaces rsh: nothing after it runs, the program's status is rsh's.
exec sh -c 'echo exec-ran; exit 4'
puts "never"
