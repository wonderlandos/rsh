# QA-100 QA-RSH-51: jobs -p and -l, a wrong option is a mistake. QA-RSH-54: the sh builtins rsh does
# not have say what to write when they are not found (a program of that name on PATH still runs).
sleep 5 &
pid = Rsh.last_pid
jobs -p > pids.txt
puts File.read("pids.txt").to_i == pid
jobs -l > long.txt
puts File.read("long.txt").start_with?("[1]  #{pid}  Running  sleep 5")
jobs > short.txt
puts File.read("short.txt").start_with?("[1]  Running  sleep 5")
Process.kill("TERM", pid)
begin
  jobs -q
rescue Rsh::Failed => e
  puts "jobs -q: #{e.status}"
end
begin
  local x
rescue Rsh::Failed => e
  puts "local: #{e.status}"
end
begin
  declare -a y
rescue Rsh::Failed => e
  puts "declare: #{e.status}"
end
