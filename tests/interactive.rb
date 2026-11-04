#!/usr/bin/ruby
# SPDX-License-Identifier: GPL-2.0-or-later
# Drives rsh through a pseudo-terminal: editing, completion, jobs, multi-line input, history search.
# Usage: ruby tests/interactive.rb path/to/rsh
# frozen_string_literal: true

require 'pty'
require 'io/console'
require 'tmpdir'
require 'fileutils'
require 'timeout'

rsh = File.expand_path(ARGV[0] || File.join(__dir__, '../build/rsh'))
home = Dir.mktmpdir('rsh-int')
env = { 'HOME' => home, 'TERM' => 'xterm', 'PATH' => '/usr/bin:/bin', 'LC_ALL' => 'C.UTF-8' }
checks = 0
failed = []

PTY.spawn(env, rsh, unsetenv_others: true) do |out, inp, pid|
  drain = lambda do |wait|
    buf = +''
    buf << out.read_nonblock(8192) while IO.select([out], nil, nil, wait)
    buf
  rescue EOFError, Errno::EIO
    buf
  end
  plain = ->(s) { s.gsub(/\e\[[0-9;]*[A-Za-z]/, '') }
  drain.call(1)
  send_line = lambda do |text, wait = 0.4|
    inp.write(text)
    plain.call(drain.call(wait))
  end
  expect = lambda do |label, text, want|
    checks += 1
    got = send_line.call(text)
    failed << "#{label}: wanted #{want.inspect} in #{got.inspect}" unless got.include?(want)
  end
  expect.call('command runs', "echo hi there\r", "\nhi there")
  expect.call('ruby value', "x = 3\r", '=> 3')
  expect.call('ruby uses it', "puts x * 2\r", "\n6")
  expect.call('tab completes a file', "ls /etc/os-relea\t\r", '/etc/os-release')
  expect.call('background job', "sleep 1 &\r", '[1]')
  expect.call('jobs lists it', "jobs\r", 'Running')
  inp.write("[1,2].map do |n|\r")
  checks += 1
  more = plain.call(drain.call(0.4))
  failed << "continuation prompt: #{more.inspect}" unless more.include?('...')
  inp.write("n * 3\r")
  drain.call(0.3)
  expect.call('block finishes', "end\r", '=> [3, 6]')
  expect.call('status in prompt', "sh -c 'exit 4'\r", ' 4 >')
  expect.call('history search', "\x12ech", '(search)')
  inp.write("\x03")
  drain.call(0.3)
  # colours: a known command word is green, an unknown one red
  inp.write("echo")
  coloured = drain.call(0.4)
  checks += 1
  failed << "known command is green: #{coloured.inspect}" unless coloured.include?("\e[32mecho")
  inp.write("\x15zzqq-nothing")
  coloured = drain.call(0.4)
  checks += 1
  failed << "unknown command is red: #{coloured.inspect}" unless coloured.include?("\e[31m")
  inp.write("\x15\x04")
  drain.call(0.3)
  Process.kill('TERM', pid) rescue nil # rubocop:disable Style/RescueModifier
end

hist = File.join(home, '.local/state/rsh/history')
checks += 1
failed << 'history file not written' unless File.exist?(hist)
checks += 1
failed << "history mode #{File.stat(hist).mode.to_s(8)}" if File.exist?(hist) && (File.stat(hist).mode & 0o77) != 0
# 0.1.3 (audit M3, M4, H3): a second session in a fresh home with an unsafe init.rsh, an old history
# file with mode 0644, a folder whose name holds terminal escapes, and lines history must not keep.
home2 = Dir.mktmpdir('rsh-int2')
state = File.join(home2, '.local/state/rsh')
FileUtils.mkdir_p(state)
File.chmod(0o700, state)
hist2 = File.join(state, 'history')
File.write(hist2, "echo old-entry\n")
File.chmod(0o644, hist2)
cfg = File.join(home2, '.config/rsh')
FileUtils.mkdir_p(cfg)
File.write(File.join(cfg, 'init.rsh'), "puts 'INIT-RAN'\n")
File.chmod(0o666, File.join(cfg, 'init.rsh'))
evil = File.join(home2, "e\e[2Jx")
Dir.mkdir(evil)
session = lambda do |h, lines|
  seen = +''
  PTY.spawn(env.merge('HOME' => h), rsh, unsetenv_others: true) do |out, inp, pid|
    read = lambda do |wait|
      buf = +''
      buf << out.read_nonblock(8192) while IO.select([out], nil, nil, wait)
      buf
    rescue EOFError, Errno::EIO
      buf
    end
    seen << read.call(1)
    lines.each do |l|
      inp.write(l)
      seen << read.call(0.4)
    end
    inp.write("\x04")
    read.call(0.3)
    Process.kill('TERM', pid) rescue nil # rubocop:disable Style/RescueModifier
  end
  seen
end
# (the name is not typed: an escape typed on the keyboard is a key, not text)
seen = session.call(home2, ["Dir.chdir(Dir.glob(ENV['HOME'] + '/e*x')[0])\r", "\r", "echo visible-entry\r", " echo space-entry\r",
                            "export MY_TOKEN=abc123\r", "cd\r"])
check = lambda do |label, ok|
  checks += 1
  failed << label unless ok
end
check.call("unsafe init.rsh refused: #{seen[0, 300].inspect}", seen.include?('others can write it') && !seen.include?('INIT-RAN'))
check.call('prompt shows an escape in a folder name as ^[', seen.include?('e^[[2Jx') && !seen.include?("e\e[2Jx"))
kept = File.read(hist2)
check.call("history keeps ordinary lines: #{kept.inspect}", kept.include?('echo visible-entry') && kept.include?('echo old-entry'))
check.call("history drops a line with a leading space: #{kept.inspect}", !kept.include?('space-entry'))
check.call("history drops a secret: #{kept.inspect}", !kept.include?('MY_TOKEN'))
check.call("an old history file gets mode 0600: #{File.stat(hist2).mode.to_s(8)}", (File.stat(hist2).mode & 0o777) == 0o600)
# A history that is a symbolic link is not followed.
home3 = Dir.mktmpdir('rsh-int3')
state3 = File.join(home3, '.local/state/rsh')
FileUtils.mkdir_p(state3)
File.chmod(0o700, state3)
target = File.join(home3, 'elsewhere')
File.write(target, '')
File.symlink(target, File.join(state3, 'history'))
seen = session.call(home3, ["echo via-link\r"])
check.call("history link refused: #{seen[0, 300].inspect}", seen.include?('it is a symbolic link'))
check.call('the link target is not written', File.read(target).empty?)
[home2, home3].each { |d| FileUtils.rm_rf(d) }

# 0.1.4 (QA-100 round 2). A session in a fresh home, a work folder, keys written as given; returns
# what the terminal showed, escapes taken out (raw: with them) and the bytes the shell wrote.
home4 = Dir.mktmpdir('rsh-int4')
work = File.join(home4, 'work')
Dir.mkdir(work)
File.write(File.join(work, 'sp ace'), "spaced\n")
File.write(File.join(work, "quo'te"), "quoted\n")
run = lambda do |keys, wait: 0.5, raw: false|
  seen = +''
  PTY.spawn(env.merge('HOME' => home4), rsh, chdir: work, unsetenv_others: true) do |out, inp, pid|
    read = lambda do |w|
      buf = +''
      buf << out.read_nonblock(65_536) while IO.select([out], nil, nil, w)
      buf
    rescue EOFError, Errno::EIO
      buf
    end
    read.call(0.6)
    keys.each do |k|
      if k.is_a?(Numeric)
        sleep k
        next
      end
      inp.write(k)
      seen << read.call(wait)
    end
    inp.write("\x04")
    seen << read.call(0.3)
    Process.kill('KILL', pid) rescue nil # rubocop:disable Style/RescueModifier
  end
  raw ? seen : seen.gsub(/\e\[[0-9;?]*[A-Za-z~]/, '')
end
# R2-1: the first program that reads the terminal is not stopped by SIGTTIN (it took the terminal
# only after the program had started; 15 of 20 sessions stopped). Twenty sessions (a race: the 0.1.3 binary stopped 1 of 20 here, 15 of 20 in the audit).
stopped = 0
20.times do
  got = run.call(["true\r", "cat\r", 0.2, "hello\r", "\x04"], wait: 0.3)
  stopped += 1 if got.include?('Stopped') || !got.include?('hello')
end
check.call("R2-1: cat after true was stopped (or did not echo) in #{stopped} of 20 sessions", stopped.zero?)
# R2-2: three lines in one write all run (TCSAFLUSH threw away the rest of a paste).
got = run.call(["puts 11\rputs 22\rputs 33\r"], wait: 1.0)
check.call("R2-2: a pasted block runs line by line: #{got.inspect}", got.include?("11\r\n") && got.include?("22\r\n") && got.include?("33\r\n"))
got = run.call(["sleep 0.3\recho typed-ahead\r"], wait: 1.2)
check.call("R2-2: a line typed while a command runs is kept: #{got.inspect}", got.include?("typed-ahead\r\n"))
# R2-8, L-17: a 5,000-character paste is drawn about once, not 5,000 times (12.9 MB before).
got = run.call(['echo ' + ('x' * 5000)], wait: 1.0, raw: true)
check.call("R2-8: a 5,000-byte paste wrote #{got.bytesize} bytes to the terminal", got.bytesize < 50_000)
# M-7, R2-16: a completed name is one word of the line.
got = run.call(["cat sp\t\r", "cat quo\t\r"])
check.call("M-7: completion quotes a name with a space: #{got.inspect}", got.include?('cat sp\\ ace') && got.include?("spaced\r\n"))
check.call("R2-16: completion quotes a name with a quote: #{got.inspect}", got.include?("cat quo\\'te") && got.include?("quoted\r\n"))
# R2-15: after | the word is a command again.
got = run.call(["echo x | wc -\t\x15", "echo x | tr\t\x15"])
check.call("R2-15: completion after | offers commands: #{got.inspect}", got.include?('tr '))
# R2-9: an arrow ends a history search and moves in the line found; its bytes are not text.
got = run.call(["echo second\r", "\x12sec", "\e[D", "X\r"])
check.call("R2-9: Ctrl-R then Left: #{got.inspect}", got.include?("seconXd\r\n") && !got.include?('[D'))
# R2-10: ESC then Enter is Enter.
got = run.call(["echo esc-enter\e\r", "echo next\r"])
check.call("R2-10: ESC then Enter runs the line: #{got.inspect}", got.include?("esc-enter\r\n") && got.include?("next\r\n"))
# M-9: history lists the entries of this session (and the earlier ones: this home has them now).
got = run.call(["echo hist-one\r", "history\r"])
check.call("M-9: history numbers this session's entries and the earlier ones: #{got.inspect}",
           got =~ /\d+  echo hist-one/ && got =~ /\d+  echo second/)
# H-2: Ctrl-C during Ruby code interrupts it (ensure runs) and the shell goes on.
got = run.call(["begin; loop {}; ensure; puts 'ensure-ran'; end\r", 0.3, "\x03", "puts 'still-here'\r"])
check.call("H-2: Ctrl-C during loop {}: #{got.inspect}", got.include?("ensure-ran") && got.include?("still-here\r\n"))
# R2-19: exit with a job running says so once; the second exit leaves.
got = run.call(["sleep 5 &\r", "exit\r", "echo still-in\r", "exit\r"])
check.call("R2-19: exit with a running job warns once: #{got.inspect}", got.include?('jobs still running') && got.include?("still-in\r\n"))
# R2-13: SIGTERM while Ruby code loops: ensure runs, the shell ends by the signal (it ignored TERM
# until -9). The loop is started, TERM comes 0.4 s later.
status = nil
PTY.spawn(env.merge('HOME' => home4), rsh, chdir: work, unsetenv_others: true) do |out, inp, pid|
  sleep 0.6
  inp.write("begin; loop {}; ensure; puts 'term-ens' + 'ure'; end\r")
  sleep 0.4
  Process.kill('TERM', pid)
  seen = +''
  begin
    Timeout.timeout(5) { seen << out.readpartial(4096) while true } # rubocop:disable Style/WhileUntilDo
  rescue EOFError, Errno::EIO
    nil
  rescue Timeout::Error
    seen << 'TIMEOUT'
    Process.kill('KILL', pid)
  end
  status = Process.wait2(pid)[1] rescue nil # rubocop:disable Style/RescueModifier
  check.call("R2-13: TERM during loop {} ran ensure and ended (#{status.inspect}): #{seen.inspect}",
             seen.include?('term-ensure') && !seen.include?('TIMEOUT'))
end
FileUtils.rm_rf(home4)

puts "interactive: #{checks} checks, #{failed.size} failed"
failed.each { |f| warn f }
exit(failed.empty? ? 0 : 1)
