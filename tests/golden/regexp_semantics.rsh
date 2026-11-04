#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# What the Regexp fast paths (one UTF-8 check per subject, the loops in C, $~ built once) must keep:
# $~ and $1..$9 after =~, match, sub, gsub, scan and split; empty matches; character offsets in
# multibyte text; invalid UTF-8 (an invalid byte never matches a pattern item, and is kept);
# embedded NUL; CJK; a subject changed inside the block.
p("abc" =~ /b/, $~[0], $1)
p("x12-y345" =~ /(\d+)-(\w)(\d+)/, $~[0], $1, $2, $3, $4, $~.pre_match, $~.post_match)
p("none" =~ /z/, $~, $1)
"k=v" =~ /(?<key>\w)=(?<val>\w)/
p $~[:key], $~["val"], $~.names, $~.named_captures, $~.captures, $~.to_a, $~.size
p $~.begin(0), $~.end(2), $~.byteoffset(1), $~.values_at(1, 2), Regexp.last_match(2)
p "a1b2c3".scan(/\d/), $~[0], "a1b2c3".scan(/([a-z])(\d)/), $1, $2
p "a1b2c3".scan(/x/), $~
r = []
"a1b2".scan(/[a-z](\d)/) { |m| r << [m, $~[0], $1] }
p r
p "a1b22".sub(/\d+/, "<\\0>"), $~[0], "a1b22".gsub(/(\d)/, "[\\1]"), $~[0], $1
p "a1b22".gsub(/(?<d>\d)/, "<\\k<d>>"), "a-b".gsub(/-/, "\\\\"), "ab".gsub(/b/, "\\&\\&")
p "cat hat".gsub(/[ch]at/, "cat" => "dog", "hat" => "cap"), "abc".gsub(/b/) { $~[0].upcase + $~.pre_match }
p "abc".gsub(//, "-"), "abc".scan(//), "abc".split(//), "a,b,,c,,".split(/,/), "a,b,,c,,".split(/,/, -1)
p "a1b2c3".split(/(\d)/), "a b  c".split(/ /), "a b c d".split(/ /, 2), " a b".split(/ /), "".split(/,/)
p "aaa".sub(/a*/, "x"), "baaa".gsub(/a*/, "x"), "héllo wörld" =~ /w/, $~.begin(0), $~.pre_match
p "日本語のテキスト" =~ /テ/, "日本語".scan(/./), "日本語".gsub(/本/, "ほん"), "日本 語".split(/ /), "日本"[/本/]
p "caf\xC3".scan(/./).size, "caf\xC3".scan(/./), "caf\xC3".gsub(/f/, "F").bytes, "a\xFFb\xFFc".split(/b/).size
x = "a\xFF" * 3
p x.gsub(/a/, "b").bytes, x.split(/a/).map(&:bytes), x.scan(/./), "caf\xC3" =~ /\xC3/, ("\xFFé" * 2).scan(/é/).size
p "a\xFFb".split(/b/).map(&:bytes), "a\xFFb" =~ /b/, "\xFF\xFEab" =~ /ab/, "x\xC3y".scan(/\w/)
p "a\0b\0c".split(/\0/), "a\0b" =~ /b/, "a\0b".gsub(/\0/, "0"), "a\0b".scan(/[^\0]/)
p "Hello".match?(/ell/), $~ && $~[0], "Hello".match?(/ell/, 2), "Hello".match(/l+/, 3)[0], "Hello".match(/l/) { |m| m.begin(0) }
p "hello world".index(/o/), "hello world".index(/o/, 5), "abc".start_with?(/a/), "abc".start_with?(/b/), "abc"[/(b)(c)/, 2]
s = "aXbXc"
p s.sub!(/X/, "-"), s, s.gsub!(/X/, "+"), s, s.gsub!(/Q/, "+"), s.sub!(/Q/, "+")
case "web-12"
when /\Aweb-(\d+)\z/ then p [$~[0], $1]
end
p(/A/i =~ "xa", /a.b/m =~ "a\nb", /a b/x =~ "ab", "x\ny" =~ /^y/, "a" !~ /b/)
t = "one two three"
out = t.gsub(/\w+/) { |w| t << "!" if w == "two"; w.upcase }
p out, t
u = "ab" * 3
p u.scan(/b/) { u = "zz" }, u
p "aaa".gsub(/a/) { |m| m.object_id == $~[0].object_id ? "?" : "a" }.size, "x".gsub(/y/, "z"), $~
# Edge cases: empty matches in CJK text, character offsets past 23 bytes, a long subject with an
# invalid byte, frozen subjects in the block loops, replacement escapes, NUL.
p "日本".split(//), "日本".gsub(//, "-"), "日本".scan(//), "日本".sub(//, "-")
p "日本語".split(/(?=本)/), "aééb".scan(/\X/), "é" * 2 =~ /\z/, $~.begin(0)
s = "é" * 30 + "x" + "é" * 5
p(s =~ /x/, $~.begin(0), $~.end(0), s.index(/x/), $~.pre_match.size)
big = ("日本 " * 200) + "\xFF" + "end"
p big.scan(/end/), big.split(/ /).size, big.gsub(/日/, "j").bytesize, big =~ /end/
f = ("ab " * 200).freeze
p f.scan(/a(b)/) { }.size, f.gsub(/b/) { "B" }.count("B"), f.gsub(/b/, "b" => "c").count("c")
f2 = ("ab " * 200 + "\xC3").freeze
p f2.scan(/b/) { }.size, f2.gsub(/b/) { "B" }.count("B"), f2.scan(/./).size
p "x".gsub(/x/, "\\k<nope>") rescue p $!
p "ab".gsub(/(?<n>a)/, "[\\k<n>]"), "ab".gsub(/a/, "\\"), "ab".gsub(/a/, "x\\"), "ab".gsub(/a/, "\\q"), "ab".gsub(/(a)/, "\\9|\\1")
p "aaa".gsub(/a/, "\\0\\0"), "a.b".gsub(/\./, "\\\\."), "abc".sub(/b/, "")
p "foo bar".match(/(?<w>o+)/)&.named_captures, $~.values_at(0, :w)
p "abc".match?(/b/, 2), "abc".match?(/c/, 2), "日本語".match(/語/, 2)&.begin(0), "日本語".match?(/日/, 1)
p "a\nb\nc".scan(/^\w$/), "a\nb".gsub(/$/, "!"), "line1\nline2".split(/\n/)
p (1..3).map { |i| "k#{i}=v#{i}" =~ /k(\d)=v(\d)/ && [$1, $2] }
p "abcabc".gsub(/b/).to_a rescue p $!.class
p "test".tr("t", "T"), "ab12".delete("0-9"), "hello"[1..2]
x = "hello world"
x =~ /(w\w+)/
y = $1
x.gsub!(/o/, "0")
p y, x, $~ && $~[0]
p "aaa".split(/a/), "aaa".split(/a/, -1), "a b c".split(/\s/, 2), "abc".split(/(b)|(z)/)
p "\0\0".scan(/\0/).size, "a\0b".split(/\0/, -1), "x\0y" =~ /y/
# Invalid UTF-8 through the valid pieces (frag_match): anchors, lookbehind, \b at a bad byte; \A and
# \G take the engine's own path.
v = "ab\xFFcd\n\xFFef é\xC3x"
p v.scan(/\w+/), v.scan(/^\w/), v.scan(/\w$/), v.scan(/\b\w/), v.scan(/(?<=\w)\w/), v.scan(/./).size, v.split(/\b/).size
p v =~ /cd$/, v =~ /b\z/, v.gsub(/\w/) { |c| c.upcase }.bytes.size, v.sub(/(?<=\xFF)c/, "C").bytes[3], v.index(/e/)
p v.scan(/\Aa/), v.scan(/\Ac/), v.scan(/\Gab/), "\xFFab".scan(/\A./), v =~ /\Acd/, ("a" * 300 + "\xFF").scan(/a\z/)
