# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "Metaprogramming": the snippet as printed there (tests/run.sh checks it is unchanged),
# after the Config class it leaves out.
class Config
  def method_missing(name, *args)
    puts "config #{name} = #{args.inspect}"
    self
  end

  def respond_to_missing?(*) = true
end

class Shell                                  # unknown methods run programs
  def method_missing(name, *args)
    ($ #{name} #{args})                      # every argument stays one word: no shell, no splitting
  end
end
Shell.new.echo "a b; rm -rf /"               # prints: a b; rm -rf /

rsh_methods :server, :role                   # a DSL answered by method_missing
Config.new.instance_eval do                 # Config has a method_missing
  server "web 1"                             # a Ruby call, not the program `server`
  echo configured                            # still a command line
end
