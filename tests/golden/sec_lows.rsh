# SPDX-License-Identifier: GPL-2.0-or-later
# Audit L1-L5, L10: limits and checks of the command-line layer.
def try(label)
  yield
  puts "#{label}: ok"
rescue Rsh::Error => e
  puts "#{label}: #{e.message}"
end

# L1: a brace pattern stops at 100000 words.
try("braces") do
  echo x{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b} > /dev/null
end
try("Dir.glob braces") { Dir.glob("{a,b}" * 30) }
echo x{a,b}{c,d}

# L2: ** does not follow a link to a parent folder.
mkdir -p d/e
touch d/e/x.txt
ln -s .. d/e/up
echo **/x.txt

# L3: an empty PATH part is not the current folder.
File.write("zzprog", "#!/bin/sh\necho ran-from-cwd\n")
chmod +x zzprog
old = ENV["PATH"]
ENV["PATH"] = ":" + old
puts((zzprog) ? "ran (wrong)" : "zzprog: not found, status #{$?.exitstatus}")
ENV["PATH"] = old

# L4: unset checks the name.
relaxed!
unset A=b
puts "unset A=b: status #{$?.exitstatus}"
relaxed!(false)

# L5: an Array is not joined into a variable or a here-document.
list = %w[a b]
try("NAME=#{list}") do
  V=#{list} printenv V
end
try("here-document") do
  cat <<~EOT
    #{list}
  EOT
end

# L10: the current folder removed.
mkdir gone
cd gone
rmdir ../gone
relaxed!
pwd
puts "pwd: status #{$?.exitstatus}"
cd ..
puts "cd ..: status #{$?.exitstatus}"
