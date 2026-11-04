# rsh: shell
# `# rsh: shell` on the first line says the default out loud: a .rb file has the shell layer.
echo hello from the shell layer
name = "x y"
echo #{name} && echo second
nosuchcommand_zz || echo failed-as-command
