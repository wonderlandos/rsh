# SPDX-License-Identifier: GPL-2.0-or-later
# Audit M2: a file whose name starts with - and is matched by a pattern reaches the program as
# ./-name, never as an option. Dir.glob gives the names as Ruby does.
touch -- -rf a.txt
echo *
ls *
rm *
ls -A | wc -l
touch -- -x
p Dir.glob("*")
