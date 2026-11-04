# SPDX-License-Identifier: GPL-2.0-or-later
# Audit C1: a command line keeps its own copy of the parsed line. Lines added while it runs (a Ruby
# stage calling system, source through system) must not move it, and system() must not take away
# another file's line. Before 0.1.3: use after free, "no such command line", a crash.

# A Ruby stage that adds many lines while the pipeline runs.
seq 1 3 | { |l| 40.times { system("true") }; "line #{l}" } | cat

# swap.rsh of the audit: b() lost its line after system("source", ...).
File.write("lib2.rsh", "def a\n  echo AUTHOR-WROTE-A\nend\ndef b\n  echo AUTHOR-WROTE-B\nend\n")
system("source", "lib2.rsh")
echo marker-1
a()
b()

# swap2.rsh: then another file; b() still runs what its author wrote (a crash before).
File.write("other.rsh", "echo OTHER-FILE-LINE\n")
system("source", "other.rsh")
b()

# source, then system, then a later line of the sourced file.
File.write("lib.rsh", "def later\n  echo later-runs\nend\n")
system("source", "lib.rsh")
system("true")
later()

# Rsh.eval of command lines inside a pipeline stage: each one adds a line to the table.
seq 4 5 | { |l| 40.times { Rsh.eval("echo in-eval > /dev/null") }; "eval #{l}" } | cat

# source of a file with many command lines, then the rest of the same line.
File.write("many.rsh", (1..60).map { |i| "echo many-#{i} > /dev/null\n" }.join)
many = "many.rsh"  # made at run time, so from the current folder
source #{many} && echo after-source || echo not-reached
