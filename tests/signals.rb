#!/usr/bin/ruby
# SPDX-License-Identifier: GPL-2.0-or-later
# rsh signals while a command runs and inside at_exit (0.9.1, audit 2 H1, H2, M1; docs/design.md
# S5.3). Each case starts rsh in its own process group, sends the signal after 0.4 s to rsh alone
# ("pid", as kill -INT, timeout -s INT) or to the whole group ("group", as Ctrl-C), and checks the
# status and the output. Usage: ruby tests/signals.rb path/to/rsh
# frozen_string_literal: true

require 'tmpdir'
require 'timeout'

rsh = File.expand_path(ARGV[0] || File.join(__dir__, '../build/rsh'))
dir = Dir.mktmpdir('rsh-sig')
File.write(File.join(dir, 'int1.rsh'), <<~RSH)
  at_exit { puts "at_exit ran" }
  begin
    sleep 5
  ensure
    puts "ensure ran"
  end
  puts "not reached"
RSH
File.write(File.join(dir, 'exit2.rsh'), <<~RSH)
  at_exit { puts "at_exit"; sleep 3; puts "at_exit end" }
  puts "main"
RSH
failed = []
checks = 0
run = lambda do |label, sig, args, want_status, want_out, group: false|
  checks += 1
  r, w = IO.pipe
  pid = Process.spawn(rsh, *args, chdir: dir, out: w, err: w, pgroup: true)
  w.close
  sleep 0.4
  group ? Process.kill(sig, -pid) : Process.kill(sig, pid)
  out = +''
  status = nil
  begin
    Timeout.timeout(8) do
      out << r.read
      status = Process.wait2(pid)[1]
    end
  rescue Timeout::Error
    Process.kill('KILL', -pid) rescue nil # rubocop:disable Style/RescueModifier
    Process.wait(pid) rescue nil # rubocop:disable Style/RescueModifier
    out << '[HUNG]'
  end
  got = status && (status.signaled? ? 128 + status.termsig : status.exitstatus)
  failed << "#{label}: wanted status #{want_status} and #{want_out.inspect}, got #{got.inspect} and #{out.inspect}" unless got == want_status && out == want_out
end
# H1: INT to rsh alone while a command runs is noted, passed to the command, acted on
run.call('H1 INT (pid) during sleep: ensure, at_exit, 130', 'INT', ['int1.rsh'], 130, "ensure ran\nat_exit ran\n")
run.call('H1 INT (pid) in loop { sleep 0.02 }', 'INT', ['-c', 'loop { sleep 0.02 }'], 130, '')
run.call('H1 INT (pid) during a pipeline', 'INT', ['-c', 'sleep 5 | cat; puts "no"'], 130, '')
run.call('H1 INT (pid) during a capture', 'INT', ['-c', 'x = `sleep 5`; puts "no"'], 130, '')
run.call('H1 INT (pid) with a trap: the trap runs after the command', 'INT', ['-c', 'trap("INT") { puts "got int"; exit 4 }; sleep 1; puts "no"'], 4, "got int\n")
# H2: Ctrl-C to the group: a trap runs, IGNORE goes on, no trap is 130
run.call('H2 Ctrl-C with trap("INT") { }', 'INT', ['-c', 'trap("INT") { puts "got int" }; sleep 5; puts "continued"'], 0, "got int\ncontinued\n", group: true)
run.call('H2 Ctrl-C with trap("INT", "IGNORE")', 'INT', ['-c', 'trap("INT", "IGNORE"); sleep 5; puts "continued"'], 0, "continued\n", group: true)
run.call('H2 Ctrl-C with no trap: ensure, at_exit, 130', 'INT', ['int1.rsh'], 130, "ensure ran\nat_exit ran\n", group: true)
# M1: a signal inside an at_exit block ends rsh by that signal
run.call('M1 TERM inside at_exit: 143', 'TERM', ['exit2.rsh'], 143, "main\nat_exit\n")
run.call('M1 INT inside at_exit: 130', 'INT', ['exit2.rsh'], 130, "main\nat_exit\n")
# unchanged: TERM to rsh alone
run.call('TERM (pid) during sleep: 143', 'TERM', ['int1.rsh'], 143, "ensure ran\nat_exit ran\n")
puts "signals: #{checks} checks, #{failed.size} failed"
failed.each { |f| warn f }
exit(failed.empty? ? 0 : 1)
