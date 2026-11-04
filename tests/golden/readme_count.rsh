# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "Counting in a log, then acting on it": the snippet as printed there (tests/run.sh checks it is unchanged).
# rsh: a regular expression and a hash, then a command per address
seen = Hash.new(0)
`cat auth.log`.scan(/Failed password for (\S+) from (\S+)/) { |user, ip| seen[ip] += 1 }
seen.select { |ip, n| n >= 2 }.each_key do |ip|
  nft add element inet filter blocked "{ #{ip} }"
end
