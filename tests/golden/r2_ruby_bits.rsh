# 0.1.4: Dir.home (audit H-3), chomp: (R2-11), Process.ppid (L-5), Dir.home("nouser").
p Dir.home == ENV["HOME"]
p Dir.home("root")
begin
  Dir.home("no-such-user-zz")
rescue ArgumentError => e
  puts e.message
end
File.write("lines.txt", "one\ntwo\n")
p File.readlines("lines.txt", chomp: true)
File.foreach("lines.txt", chomp: true) { |l| p l }
File.open("lines.txt") { |f| p f.gets(chomp: true); p f.readlines(chomp: true) }
File.open("lines.txt") { |f| f.each_line(chomp: true) { |l| p l } }
p Process.ppid.is_a?(Integer) && Process.ppid > 0
# 0.1.4 (audits L-5, L-7, L-9): $PROGRAM_NAME is $0; echo -e knows \xHH and \0NNN; an Rsh.eval
# syntax error names its own text.
p $PROGRAM_NAME == $0 && !$PROGRAM_NAME.empty?
echo -e "\x41\0102\x4a\x7a|"
begin
  Rsh.eval("x = (")
rescue SyntaxError => e
  puts e.message
end
begin
  Rsh.eval("1 +\n(", nil, "my.rb", 7)
rescue SyntaxError => e
  puts e.message
end
