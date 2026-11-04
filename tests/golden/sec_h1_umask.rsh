# SPDX-License-Identifier: GPL-2.0-or-later
# Audit H1: umask takes octal digits only; anything else is an error (status 2) and the mask stays.
relaxed!
umask 022
umask
for bad in ["u=rwx,go=rx", "22x", "abc", "8", "", "12345", "-1"]
  umask #{bad}
  puts "umask #{bad.inspect}: status #{$?.exitstatus}"
end
umask 1 2
puts "two masks: status #{$?.exitstatus}"
umask
umask 0077
umask
echo > f
ls -l f | cut -c1-10
