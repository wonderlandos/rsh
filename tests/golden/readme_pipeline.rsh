# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "Counting in a log, then acting on it (a block as a pipeline stage)": the snippet as printed there (tests/run.sh checks it is unchanged).
cat auth.log | { |l| $1 if l =~ /Failed password .* from (\S+)/ } | sort | uniq -c | sort -rn
