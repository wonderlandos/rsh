# 0.1.4 (audits L-16, R2-20, L-2): sh forms are reader mistakes that say what to write instead.
[ -f /etc/hostname ] && echo yes
if [ "$HOME" = x ]; then echo y; fi
out=$(ls)
for f in a b; do echo $f; done
case $1 in
set -eu
alias ll='ls -l'
. ./lib.sh
true && echo hi
echo a |
