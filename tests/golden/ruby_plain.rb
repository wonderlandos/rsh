#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# A .rb file is plain Ruby: no line is a command line, whatever its first word.
# rsh: shell   (not on the first line, so it changes nothing)
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
