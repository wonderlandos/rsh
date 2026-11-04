#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# The Ruby side of Regexp loads on first use (start-up does not read it). The first lines run
# before it is loaded: plain String methods, the constants and MatchData; then the stand-ins that
# load it (scan, match?, match, =~), then patterns.
p "a,b,c".split(","), "abc".sub("b", "B"), "abc".gsub("b") { |m| m.upcase }, "abc"[1], "abc".index("c")
p "abc".start_with?("ab"), "abcb".sub!("b", "x"), Regexp::IGNORECASE, MatchData.is_a?(Class)
p "a-b-a".scan("a")
p "x.y".match?("."), "x.y".match(".")[0]
begin
  "abc" =~ "b"
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
p :sym =~ /y/, Regexp.escape("1+1"), Regexp.union("a.", /b+/), Regexp.compile("z").source
p Regexp.last_match(0)
p "a1b22"[/\d+/], "a1b22".split(/\d+/), "a1b22".gsub(/\d/) { |d| (d.to_i * 2).to_s }
p $~ && $~[0]
case "web-12"
when /\Aweb-(\d+)\z/ then puts "host #{$1}"
end
