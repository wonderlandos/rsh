# rsh: shell
# A .rb file with the opt-in on its first line gets the shell layer.
echo hello from the shell layer
name = "x y"
echo #{name} && echo second
nosuchcommand_zz || echo failed-as-command
