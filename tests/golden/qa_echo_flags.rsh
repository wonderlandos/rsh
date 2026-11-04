# QA-100 QA-RSH-45 (audit L-7): echo takes -E (no escapes) and every bash flag group of n, e and E
# (the last of e and E wins); -e keeps the backslash of an escape it does not know, as bash.
echo -E "a\nb|"
echo -eE "c\nd|"
echo -Ee "e\tf|"
echo -nE "g\n"
echo "|"
echo -e "h\qi\\j|"
echo -x "k|"
echo - "l|"
x = `echo -E "m\nn"`
p x
