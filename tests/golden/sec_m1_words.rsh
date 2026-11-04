# SPDX-License-Identifier: GPL-2.0-or-later
# Audit M1: the built-in interpreter's Kernel functions are Ruby words; `open "notes.txt"` opens the
# file and never runs a program called open. `sleep` stays a command.
File.write("notes.txt", "one\ntwo\n")
f = open "notes.txt"
puts f.read.lines.size
f.close
open "notes.txt"
__dir__
putc "x"
putc 10
p __dir__.nil?
p __callee__
tap { puts "tap is Ruby" }
p itself.equal?(self)
p local_variables.include?(:f)
sleep 0
echo after sleep
