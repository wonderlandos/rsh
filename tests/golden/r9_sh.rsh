# 0.9.0: Sh, the shell things as Ruby values, and the Ruby-standard FileUtils, Find, Shellwords,
# Dir.mktmpdir; built in, no require (docs/design.md S8). One case per helper.
Sh.mkdir_p("logs/old", "out")
File.write("logs/a.log", "start\nERROR disk\n")
File.write("logs/old/b.log", "error net\n")
File.write("logs/.hidden.log", "x\n")
File.write("logs/c.txt", "plain\n")
# find: name (a pattern on the file name), type, maxdepth, hidden, older/newer (seconds or a Time)
p Sh.find("logs")
p Sh.find("logs", name: "*.log")
p Sh.find("logs", name: "*.{log,txt}", hidden: false)
p Sh.find("logs", type: :dir), Sh.find("logs", maxdepth: 1, type: :file)
p Sh.find("logs", newer: 3600).size, Sh.find("logs", older: 3600), Sh.find("logs", older: Time.now + 60).size
begin
  Sh.find("logs", type: :pipe)
rescue ArgumentError => e
  puts e.message
end
# grep: a Regexp or plain text; "file:" in front with more than one file
p Sh.grep(/error/i, "logs/a.log"), Sh.grep("error", Sh.find("logs", name: "*.log"))
# which, exists?, file?, dir?, age, size, du
p [Sh.which("sh").end_with?("/sh"), Sh.which("no-such-program-zz"), Sh.which("/bin/sh")]
p [Sh.exists?("logs"), Sh.file?("logs"), Sh.dir?("logs"), Sh.age("logs/a.log") < 60, Sh.size("logs/a.log"), Sh.du("logs")]
# read, write, append, lines
Sh.write("out/n.txt", "one\n")
Sh.append("out/n.txt", "two\n")
p Sh.read("out/n.txt"), Sh.lines("out/n.txt")
# touch, cp, cp_r, mv, ln_s, chmod, rm, rm_rf
Sh.touch("out/t", "out/u")
File.chmod(0o750, "out/n.txt")
p Sh.cp("out/n.txt", "out/copy.txt"), (File.stat("out/copy.txt").mode & 0o777).to_s(8)
p Sh.cp("out/n.txt", "logs")
Sh.cp_r("logs", "backup")
p Sh.find("backup").size == Sh.find("logs").size
p Sh.mv("out/u", "out/moved"), Sh.ln_s("n.txt", "out/link"), File.readlink("out/link")
Sh.chmod(0o600, "out/t")
p (File.stat("out/t").mode & 0o777).to_s(8)
Sh.rm("out/t")
Sh.rm_rf("backup", "not-there")
p [Sh.exists?("out/t"), Sh.exists?("backup")]
begin
  Sh.rm("not-there")
rescue SystemCallError => e
  p e.class
end
# tmpdir: private, removed after the block (also when it raises)
kept = Sh.tmpdir do |d|
  File.write("#{d}/f", "x")
  p [File.directory?(d), (File.stat(d).mode & 0o777).to_s(8)]
  d
end
p File.exist?(kept)
# FileUtils: the Ruby names; options are refused, not ignored
FileUtils.mkdir_p("fu/a/b")
FileUtils.touch(["fu/a/b/x", "fu/y"])
FileUtils.cp("fu/y", "fu/a")
FileUtils.mv("fu/a/y", "fu/a/z")
p Sh.find("fu").map { |f| f.sub("fu/", "") }
FileUtils.rm_f("fu/none")
FileUtils.rm_rf("fu")
p File.exist?("fu")
begin
  FileUtils.rm_rf("logs", noop: true)
rescue ArgumentError => e
  puts e.message
end
# Find: the start and everything below, in order; Find.prune skips a folder
Find.find("logs") do |path|
  Find.prune if File.basename(path) == "old"
  puts path
end
p Find.find("logs").count
# Shellwords and Dir.mktmpdir
p Shellwords.escape("it's $HOME"), Shellwords.split(%q(a "b c" 'd e' f\ g)), %w[x y\ z].shelljoin
p "one two".shellsplit, "a b".shellescape
Dir.mktmpdir("t") { |d| p File.basename(d).start_with?("t.") }
require "fileutils"
require "find"
require "shellwords"
require "tmpdir"
# command lines keep their meaning: `find` at a line start is the program
find logs -name '*.txt' | sort
# 0.9.0 review: rm_rf refuses what is almost always an empty variable or the wrong folder.
["", "/", ".", "..", "./", "logs/..", "logs/.", Dir.pwd, "//"].each do |path|
  begin
    Sh.rm_rf(path)
    puts "removed #{path.inspect}"
  rescue ArgumentError => e
    puts e.message.sub(Dir.pwd, "WORK")
  end
end
begin
  FileUtils.rm_rf(["logs/a.log", "."])
rescue ArgumentError => e
  puts "FileUtils: #{e.message}"
end
p File.exist?("logs/a.log")
# cp onto the same file (by name or through a link) is refused and leaves it whole; a copy goes
# through a new file renamed over the old one.
File.write("same.txt", "keep me")
File.symlink("same.txt", "same-link")
["same.txt", "same-link"].each do |d|
  begin
    Sh.cp("same.txt", d)
  rescue ArgumentError => e
    puts e.message
  end
end
p File.read("same.txt"), Dir.children(".").grep(/rsh-/)
# cp_r into itself or below itself is refused.
Sh.mkdir_p("tree/sub")
["tree/sub", "tree"].each do |d|
  begin
    Sh.cp_r("tree", d)
  rescue ArgumentError => e
    puts e.message
  end
end
# a deep tree: find, du, cp_r and rm_rf all walk it
deep = (1..40).map { |i| "d#{i}" }.join("/")
Sh.mkdir_p("deep/#{deep}")
Sh.touch("deep/#{deep}/leaf")
p Sh.find("deep", type: :file).size, Sh.find("deep").size, Sh.du("deep")
Sh.cp_r("deep", "deep2")
p Sh.find("deep2", name: "leaf").size
Sh.rm_rf("deep", "deep2")
p Sh.exists?("deep"), Sh.exists?("deep2")
# tmpdir removes its folder when the block raises too
begin
  Sh.tmpdir do |d|
    $raised_in = d
    File.write("#{d}/x", "1")
    raise "boom"
  end
rescue RuntimeError => e
  p [e.message, File.exist?($raised_in)]
end
# Errno errors carry the system's text
begin
  Sh.find("no-such-dir")
rescue SystemCallError => e
  p [e.class, e.message]
end
begin
  Find.find("no-such-dir") { }
rescue SystemCallError => e
  p [e.class, e.message]
end
# Shellwords as Ruby's: a backslash before a new line stays, a lone final backslash stays
p "a\\\nb".shellsplit, "x \\".shellsplit, "\"a\\\nb\"".shellsplit, Shellwords.escape("a\nb"), Shellwords.escape("")
# respond_to? before anything else loaded them, and require loads
p [Shellwords.respond_to?(:escape), FileUtils.respond_to?(:no_such), Find.respond_to?(:prune)]
