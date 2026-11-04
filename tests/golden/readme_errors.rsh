# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "Stopping on an error": the snippet as printed there (tests/run.sh checks it is unchanged).
# rsh: a failing command raises Rsh::Failed, with what it ran and what it said
begin
  make
  make test
rescue Rsh::Failed => e
  $stderr.puts "#{e.argv.join(" ")} failed with #{e.status.exitstatus}: #{e.stderr.strip}"
  exit 1
end
