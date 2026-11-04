#!/usr/bin/rsh
# rsh: ruby
# SPDX-License-Identifier: GPL-2.0-or-later
# `# rsh: ruby` after the shebang: plain Ruby, no line is a command line, whatever its first word.
def twice(x)
  x * 2
end
puts twice(21)
puts ARGV.inspect
puts "echo is only text here"
begin
  nosuchcommand "arg"
rescue NoMethodError
  puts "NoMethodError"
end
words = %w[a b c]
puts words.map { |w| w.upcase }.join("-")
puts <<~TEXT
  ls -l | wc -l
TEXT
