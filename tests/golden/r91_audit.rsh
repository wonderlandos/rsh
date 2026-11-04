# 0.9.1, audit 2 of v0.9.0: one case per fix that a golden can show (the signal cases are
# tests/signals.rb, the rest tests/run.sh cli cases "A2-...").
# M2 (decided): a capture whose command fails raises Rsh::Failed, as any failed command; in a
# condition or with relaxed! it gives its output and sets $?.
begin
  x = `sh -c 'echo out; exit 6'`
rescue Rsh::Failed => e
  p [e.message, e.status.exitstatus]
end
relaxed!
x = `sh -c 'echo kept; exit 6'`
p [x, $?.exitstatus]
relaxed!(false)
# M3 (decided): a bare `wait` waits for every job and fails with the first job (in start order) that
# failed: a failing first job is not hidden by a later one that worked.
sh -c 'exit 3' &
sh -c 'exit 0' &
ok = (wait)
p [ok, $?.exitstatus]
sh -c 'exit 0' &
sh -c 'exit 4' &
sh -c 'exit 5' &
begin
  wait
rescue Rsh::Failed => e
  puts e.message
end
# M4: source of a folder or of /dev/null is a mistake (status 1), as the read-ahead says
ok = (source /tmp)
p [ok, $?.exitstatus]
ok = (source /dev/null)
p [ok, $?.exitstatus]
# M6: Sh.cp keeps the system's error, with the destination's name
Sh.mkdir_p("ro")
File.write("src.txt", "hi")
File.chmod(0o555, "ro")
if Process.euid != 0
  begin
    Sh.cp("src.txt", "ro/x")
  rescue SystemCallError => e
    p [e.class, e.message]
  end
else
  p [Errno::EACCES, "Permission denied - ro/x"]
end
File.chmod(0o755, "ro")
# M7: a FIFO or a device is not read by cp (it would block or never end); cp_r says and skips it;
# a big file is copied in pieces with its mode
begin
  Sh.cp("/dev/zero", "zero")
rescue ArgumentError => e
  puts e.message
end
Sh.mkdir_p("withfifo")
File.write("withfifo/plain", "x")
system("mkfifo", "withfifo/pipe")
Sh.cp_r("withfifo", "fifocopy")
p Dir.children("fifocopy").sort, Dir.children(".").grep(/rsh-/)
File.write("big", "x" * 300_000)
File.chmod(0o640, "big")
Sh.cp("big", "big2")
p [File.size("big2"), (File.stat("big2").mode & 0o777).to_s(8)]
# M9: rm_rf walks from each folder's own descriptor; a link inside is removed, never followed
Sh.mkdir_p("tree/a", "keep")
File.write("keep/precious", "x")
File.symlink("../../keep", "tree/a/link")
Sh.rm_rf("tree")
p [File.exist?("tree"), File.exist?("keep/precious")]
# M12: FileUtils.mv of several sources onto a file is refused (they would replace each other)
FileUtils.touch(%w[ma mb mc])
begin
  FileUtils.mv(%w[ma mb], "mc")
rescue ArgumentError => e
  puts e.message
end
p [File.exist?("ma"), File.exist?("mb")]
# L8: cd -- DIR
cd -- withfifo
puts File.basename(Dir.pwd)
cd ..
# L9: Sh.find on a file is ENOTDIR; maxdepth 0 and an unclosed { are said, not leaked as Regexp
begin
  Sh.find("big")
rescue SystemCallError => e
  p e.class
end
[{ maxdepth: 0 }, { name: "a{b" }].each do |opts|
  begin
    Sh.find(".", **opts)
  rescue ArgumentError => e
    puts e.message
  end
end
# L10: Dir.mktmpdir([prefix, suffix]); Sh.chmod takes a number
Dir.mktmpdir(["pre", ".suf"]) { |d| p [File.basename(d).start_with?("pre."), d.end_with?(".suf")] }
begin
  Sh.chmod("u+x", "big")
rescue ArgumentError => e
  puts e.message
end
