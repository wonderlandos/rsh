# SPDX-License-Identifier: GPL-2.0-or-later
# Audit M8: an exception from a loaded text (Rsh.eval, require_relative, load, source) reaches the
# caller after rsh's own clean-up: no leak (the sanitizer run of make test checks it) and the list
# of files being loaded stays right, so a later require_relative still finds its file.
Dir.mkdir("sub")
File.write("sub/bad.rsh", "raise ArgumentError, \"bad file\"\n")
File.write("sub/fails.rsh", "test -e /nonexistent-zz\n")
File.write("good.rsh", "def good = :found_next_to_main\n")
File.write("main.rsh", <<~'RUBY')
  begin
    Rsh.eval("test -e /nonexistent-zz")
  rescue Rsh::Failed => e
    puts "Rsh.eval: #{e.message}"
  end
  begin
    require_relative "sub/bad"
  rescue ArgumentError => e
    puts "require_relative: #{e.message}"
  end
  begin
    require_relative "sub/fails"
  rescue Rsh::Failed => e
    puts "require_relative: #{e.message}"
  end
  # after two failed loads from sub/, a name relative to main.rsh is found next to it
  require_relative "good"
  p good
RUBY
load "main.rsh"
relaxed!
bad = "sub/bad.rsh"  # made at run time: from the current folder (a written-out path is from this file's)
source #{bad}
puts "source: #{$?.exitstatus}"
relaxed!(false)
begin
  load "sub/bad.rsh"
rescue ArgumentError => e
  puts "load: #{e.message}"
end
