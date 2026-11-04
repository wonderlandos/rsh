# SPDX-License-Identifier: GPL-2.0-or-later
# method_missing both ways.
# 1. A class whose unknown methods run a program: the name is the program, every argument is one
#    word of the command line, whatever it holds (no shell, no splitting). In parentheses the
#    command line is a value (true or false by its status) and a failure does not raise.
class Shell
  def method_missing(name, *args)
    prog = name.to_s.tr("_", "-")
    return super unless prog.match?(/\A[a-z][a-z0-9-]*\z/)
    ($ #{prog} #{args})
  end

  def respond_to_missing?(name, include_private = false)
    name.to_s.match?(/\A[a-z][a-z0-9_]*\z/) || super
  end
end

sh = Shell.new
sh.echo "one argument; rm -rf /", "$HOME", "*"
sh.expr "2", "+", "3"
sh.basename "/a b/c d.txt", ".txt"
sh.echo
p sh.respond_to?(:ls), sh.respond_to?(:Upper)
ok = sh.false
p ok, $?.exitstatus
begin
  sh.Not_a_program
rescue NoMethodError => e
  puts "NoMethodError #{e.name}"
end

# 2. The reverse: a DSL whose words are Ruby methods answered by method_missing. Bare words in
#    the block would be command lines, so rsh_methods names them.
class Config
  rsh_methods :server, :role, :port
  attr_reader :settings

  def initialize(&block)
    @settings = {}
    instance_eval(&block)
  end

  def method_missing(name, *args)
    return super unless %i[server role port].include?(name)
    @settings[name] = args.size == 1 ? args[0] : args
  end

  def respond_to_missing?(name, include_private = false)
    %i[server role port].include?(name) || super
  end
end

c = Config.new do
  server "web 1"
  role :app, :db
  port 8080
  echo "a command line in the same block"
end
p c.settings
p c.respond_to?(:server)
