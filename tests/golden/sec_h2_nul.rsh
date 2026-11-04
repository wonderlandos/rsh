# SPDX-License-Identifier: GPL-2.0-or-later
# Audit H2: a NUL byte in an argument, an Array element, a variable value or a redirect target is an
# error; before 0.1.3 it silently cut the argument short. A here-document may hold one.
def try(label)
  yield
  puts "#{label}: ran (wrong)"
rescue Rsh::Error => e
  puts "#{label}: #{e.message}"
end

x = "keep\0dropped"
try("argument") { $ printf "[%s]\n" #{x} }
try("in a word") { echo pre-#{x} }
try("array element") { echo #{["ok", x]} }
try("variable value") { V=#{x} printenv V }
try("redirect target") { echo hi > #{x} }
try("system") { system("echo", x) }
try("Rsh.run") { Rsh.run("echo", x) }
begin
  ENV["V"] = x
rescue ArgumentError => e
  puts "ENV: #{e.class}"
end
puts File.exist?("keep") ? "a file keep was made (wrong)" : "no file made"
# A here-document is the program's input, not an argument: a NUL there is passed on.
cat <<~EOT | od -c | head -1
  a#{"\0"}b
EOT
