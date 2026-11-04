# 0.9.0 (script interpreter gaps, docs/STABILITY.md "0.9.0 gaps"): one case per fix.
# G2: `export NAME` for a name that is not set is a mistake (it set an empty variable, so the sh habit
# `FOO=1; export FOO` silently exported nothing), and a set one stays as it is.
begin
  export RSH_NOT_SET_ZZ
rescue Rsh::Failed => e
  puts "G2 export: #{e.status.exitstatus} #{ENV.key?("RSH_NOT_SET_ZZ")}"
end
export RSH_SET_ZZ=kept
export RSH_SET_ZZ
puts "G2 kept: #{ENV["RSH_SET_ZZ"]}"
# G7: `source FILE WORDS` gives the file ARGV = WORDS, then the script's ARGV comes back.
File.write("lib.rsh", "puts \"G7 inside: \#{ARGV.inspect}\"\n")
lib = "lib.rsh"  # made at run time, so from the current folder (decision D3 / review 10)
source #{lib} one "two words"
puts "G7 after: #{ARGV.inspect}"
source #{lib}
# G9: file tests, File.stat and the user's ids without a program.
File.write("plain.txt", "12345")
File.chmod(0o640, "plain.txt")
st = File.stat("plain.txt")
p [st.size, st.file?, st.directory?, (st.mode & 0o777).to_s(8), st.ftype, st.owned?, st.uid == Process.euid]
p [File.readable?("plain.txt"), File.executable?("plain.txt"), File.writable?("plain.txt"), File.owned?("plain.txt")]
File.symlink("plain.txt", "link")
p [File.lstat("link").symlink?, File.stat("link").symlink?, File.ftype("link"), File.ftype(".")]
p [Process.uid.is_a?(Integer), Process.euid == Process.uid, Process.gid.is_a?(Integer), Process.egid.is_a?(Integer)]
p File.stat(".").mtime.is_a?(Time)
begin
  File.stat("missing")
rescue SystemCallError => e
  puts "G9 missing: #{e.class}"
end
# G12: what `require "time"` gives in Ruby, for ISO 8601 (it gave nothing: Time.parse and
# Time#iso8601 were missing although the require worked).
require "time"
p Time.utc(2024, 1, 2, 3, 4, 5).iso8601
p Time.utc(2024, 1, 2, 3, 4, 5, 123456).xmlschema(3)
p Time.parse("2024-01-02T04:04:05+01:00").to_i == Time.utc(2024, 1, 2, 3, 4, 5).to_i
p Time.iso8601("2024-01-02T03:04:05.5Z").usec
p Time.parse("2024-01-02").day
begin
  Time.parse("next tuesday")
rescue ArgumentError => e
  puts "G12: #{e.message}"
end
# G13: the require message names every built-in library.
begin
  require "net/http"
rescue LoadError => e
  puts "G13: #{e.message}"
end
