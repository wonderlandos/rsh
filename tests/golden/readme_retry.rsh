# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "Try again, waiting longer each time": the snippet as printed there (tests/run.sh checks it is unchanged).
# rsh: write it once as a method that takes a block
def with_retries(times: 5)
  times.times do |n|
    return true if yield
    sleep #{2 ** n}
  end
  false
end
with_retries { (curl -fsSO #{ARGV[0]}) } or raise "gave up on #{ARGV[0]}"
