#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# Ruby words the built-in interpreter lacked before 0.1.2: String#[] and #slice with a Regexp,
# File.mtime; and warn and abort (0.1.1), here in a file with the shell layer.
line = "Nov 03 10:00:01 box sshd[1]: Failed password for root from 10.0.0.9 port 22"
p line[/\d+\.\d+\.\d+\.\d+/]
p line[/for (\w+) from (\S+)/, 2]
p line[/for (?<user>\w+)/, "user"]
p line.slice(/port \d+/)
p line[/no such text/]
p $~
p line[0, 3], line[-2..], line["box"]
touch -d "2001-02-03 04:05:06 UTC" stamp
p File.mtime("stamp").class, File.mtime("stamp").to_i
begin
  File.mtime("missing")
rescue SystemCallError => e
  puts "#{e.class}: #{e.message}"
end
warn "a warning", "and another"
puts "still running"
abort "stopped by abort"
puts "never printed"
