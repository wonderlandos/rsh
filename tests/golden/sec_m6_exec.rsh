# SPDX-License-Identifier: GPL-2.0-or-later
# Audit M6: NAME=value exec prog keeps NAME for prog (before 0.1.3 it was lost).
FOO=kept-for-exec exec sh -c 'echo "FOO=$FOO"; exit 4'
echo not reached
