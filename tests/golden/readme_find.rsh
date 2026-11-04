# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "A loop over what find found": the snippet as printed there (tests/run.sh checks it is unchanged).
# rsh: split on the zero byte, then plain Ruby
`find src -name '*.txt' -print0`.split("\0").each do |file|
  n = File.read(file).scan(/TODO/).size
  puts "#{n} #{file}" if n > 0
end
