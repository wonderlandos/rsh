# SPDX-License-Identifier: GPL-2.0-or-later
# Audit M7: a pattern that would take very long on a text stops with RegexpError; ordinary patterns
# over large texts still match.
evil = Regexp.new("^(a+)+$")
t0 = Time.now
begin
  evil =~ ("a" * 40 + "b")
  puts "matched (wrong)"
rescue RegexpError => e
  puts "RegexpError: #{e.message}"
end
puts "fast: #{Time.now - t0 < 1}"
big = "word " * 200_000 + "end"
p big =~ /end$/
p ("word " * 5000).scan(/w\w+/).size
p ("a" * 100_000) =~ /^(a)*$/
# Arrays longer than the interpreter's old cap (131072 entries)
p (1..200_000).map { |i| i }.size
