# SPDX-License-Identifier: GPL-2.0-or-later
# Audit C2: no text ever reaches /bin/sh. open("|...") and IO.popen are refused with a plain
# Rsh::Error that names the rsh way; open of a file name works.
def try(label)
  yield
  puts "#{label}: ran (wrong)"
rescue Rsh::Error => e
  puts "#{label}: #{e.message}"
end

try("open pipe") { open("|echo via-bin-sh $0").read }
try("open pipe, block") { open("| id") { |f| f.read } }
try("IO.popen") { IO.popen("echo $0").read }
try("IO.popen array") { IO.popen(["echo", "x"]).read }
try("IO._popen") { IO._popen("echo x", "r") }

# A file name still opens (and a name that only contains | is a file).
File.write("notes.txt", "kept\n")
puts open("notes.txt").read
File.write("a|b", "pipe in the middle is a file name\n")
puts open("a|b") { |f| f.read }
# The rsh way: a capture or a command line.
puts `echo captured`
