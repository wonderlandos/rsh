#!/usr/bin/rsh
# tidy: pack the logs older than a week, then warn about full disks
old = `find logs -name '*.log' -mtime +7 -print0`.split("\0")
if old.any?
  tar -czf old-logs.tgz #{old} && rm #{old}
end
puts "packed #{old.size} files"

usage = `df -P`.lines.drop(1).map(&:split).to_h { |f| [f[5], f[4].to_i] }
full = usage.select { |mount, pct| pct > 90 }.keys
$stderr.puts "almost full: #{full.join(", ")}" if full.any?

# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, the first example (tests/run.sh checks the text above is unchanged). What it left:
puts Dir.children("logs").sort
puts `tar -tzf old-logs.tgz`.lines.sort
