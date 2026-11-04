# SPDX-License-Identifier: GPL-2.0-or-later
# Audit M5: background jobs of a script are reaped before each command line (no zombies); wait
# still gives an ended job's status; fg and bg without job control are refused.
20.times do
  /bin/true &
end
sleep 0.5
true
zombies = `ps -o stat= --ppid #{Process.pid}`.lines.count { |l| l.start_with?("Z") }
puts "zombies: #{zombies}"
sh -c "exit 3" &
sleep 0.2
true
relaxed!
wait
puts "wait: #{$?.exitstatus}"
sleep 1 &
fg
puts "fg: #{$?.exitstatus}"
bg
puts "bg: #{$?.exitstatus}"
wait
puts "end: #{$?.exitstatus}"
