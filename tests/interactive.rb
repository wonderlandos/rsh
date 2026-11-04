#!/usr/bin/ruby
# SPDX-License-Identifier: GPL-2.0-or-later
# Drives rsh through a pseudo-terminal: editing, completion, jobs, multi-line input, history search.
# Usage: ruby tests/interactive.rb path/to/rsh
# frozen_string_literal: true

require 'pty'
require 'io/console'
require 'tmpdir'
require 'fileutils'

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
  expect.call('tab completes a file', "ls /etc/hostn\t\r", '/etc/hostname')
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

puts "interactive: #{checks} checks, #{failed.size} failed"
failed.each { |f| warn f }
exit(failed.empty? ? 0 : 1)
