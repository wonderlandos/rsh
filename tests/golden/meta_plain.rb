# rsh: ruby
# SPDX-License-Identifier: GPL-2.0-or-later
# Plain Ruby (the shell layer off): every bare word is Ruby, so methods made at run time need no
# declaration. Commands go through Rsh.run (one argument each) or Rsh.eval (the shell layer).
class Ops
  %w[status restart].each do |verb|
    define_method(verb) { |unit| "#{verb} #{unit}" }
  end

  def method_missing(name, *args)
    return super unless name.to_s.start_with?("svc_")
    "svc #{name.to_s.delete_prefix("svc_")} #{args.join(" ")}"
  end

  def respond_to_missing?(name, priv = false) = name.to_s.start_with?("svc_") || super

  def all
    [status("web"), restart("db"), svc_reload("cache")]
  end
end

o = Ops.new
o.instance_eval do
  p status("x")
  p svc_ping
end
p o.all
p o.respond_to?(:svc_any), Ops.method_defined?(:restart)

ls = "a local named ls"
p ls
Rsh.run("echo", "one; two", "*")
p Rsh.eval('echo #{ls} | tr a-z A-Z', binding)
code = "def from_eval = :ruby_only\nfrom_eval"
p eval(code)
p Rsh.eval("x = 1\nx + 1")
