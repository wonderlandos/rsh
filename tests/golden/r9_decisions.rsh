# 0.9.0, the CTO's decisions D1-D3 (docs/STABILITY.md section 10).
# D1: a pattern with no match and a redirect that cannot open are a failed command, status 1:
# false in a condition (a no-match says nothing there; an open failure says why on standard error),
# Rsh::Failed outside one, with the reason as its message.
ls *.zzz || echo "no match: the || ran, status $?"
cat < missing.txt || echo "cannot open: the || ran"
p [(ls *.zzz), $?.exitstatus, $?.statuses]
if cat < missing.txt
  puts "never"
else
  puts "if: false"
end
begin
  rm *.tmp
rescue Rsh::Failed => e
  p [e.class, e.message, e.status.exitstatus, e.argv]
end
begin
  echo x > no-such-folder/f
rescue Rsh::Failed => e
  p [e.message, e.status.exitstatus]
end
! ls *.zzz && echo "! turns the no-match round"
relaxed!
ls *.zzz
p $?.exitstatus
relaxed!(false)
# nil, NUL and an Array inside a word stay mistakes in the line (Rsh::Error), in a condition too.
begin
  x = nil
  echo #{x} || echo never
rescue Rsh::Error => e
  p e.class
end
begin
  x = "a\0b"
  echo #{x} || echo never
rescue Rsh::Error => e
  p e.class
end
begin
  list = %w[a b]
  echo -x=#{list} || echo never
rescue Rsh::Error => e
  p e.class
end
# D1 in a pipeline: the stage that cannot start is the failure; Rsh::Failed#argv is that command.
echo x | grep -q x *.zzz || echo "pipeline: the || ran"
begin
  echo x | cat -n > no-such-folder/f
rescue Rsh::Failed => e
  p [e.message, e.argv]
end
# D1 and `&`: nothing starts in the background and the script stops (as 0.1.4), even after ||.
begin
  sleep 5 > no-such-folder/f &
rescue Rsh::Failed => e
  p ["bg", e.message, e.argv, Rsh.last_pid]
end
begin
  echo *.zzz &
rescue Rsh::Failed => e
  p ["bg", e.message]
end
# D2: Rsh.last_pid is the pid of the last `cmd &` (nil before); wait takes a pid as sh does.
p Rsh.last_pid
sh -c 'exit 3' &
pid = Rsh.last_pid
p pid.is_a?(Integer)
sh -c 'test "$1" = "$2"' x $! #{pid}
ok = (wait #{pid})
p [ok, $?.exitstatus]
# D2 with job numbers: `wait %N` is job N, `wait PID` the job of that process (a pid wins).
sleep 0.1 &
sh -c 'exit 5' &
ok = (wait %2)
p [ok, $?.exitstatus]
wait %1
# D3: a `source <literal path>` file is read ahead, relative to this script's folder (when it is
# read and when it runs), so its methods are Ruby at a bare call; a path made at run time keeps the
# current folder.
source r9_decisions.lib
greet "world"
lib = "#{__dir__}/r9_decisions.lib"
source #{lib}
greet("again")
cd /
source r9_decisions.lib
greet "from /, the script's folder still"
