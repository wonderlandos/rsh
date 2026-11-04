# SPDX-License-Identifier: GPL-2.0-or-later
# instance_eval / class_eval with blocks that hold command lines, code built in a here-document,
# send and public_send, and the two evals: `eval` is Ruby only, `Rsh.eval` reads the shell layer.
class Box
  def initialize = (@items = [])
  def add(x) = (@items << x; self)
  private def secret = "kept"
end

box = Box.new
box.instance_eval do
  @items << "from instance_eval"
  echo items: #{@items.size}
end
Box.class_eval do
  def list = @items.join(",")
  echo class_eval body runs #{name}
end
puts box.list

# Methods written as text, with #{} filling in the names; class_eval reads it as Ruby.
fields = %w[host port]
Box.class_eval <<~RUBY, __FILE__, __LINE__ + 1
  #{fields.map { |f| "def #{f} = @#{f}\ndef #{f}=(v)\n  @#{f} = v\nend" }.join("\n")}
RUBY
box.host = "h1"
box.port = 22
p [box.host, box.port]
p box.instance_eval("@host"), Box.class_eval("instance_methods(false).size")
def top_level_after_class_eval = :top
p Object.private_method_defined?(:top_level_after_class_eval)
p Box.private_method_defined?(:top_level_after_class_eval, false)

p box.send(:secret), box.public_send(:add, 1).list
begin
  box.public_send(:secret)
rescue NoMethodError
  puts "public_send keeps private private"
end
send :puts, "send at a statement start is Ruby"

# eval: plain Ruby, it sees local variables, and a line `echo x` in it is a Ruby call.
n = 20
p eval("n + 1")
begin
  eval("echo_zz 1")
rescue NoMethodError => e
  puts "eval is Ruby: #{e.class}"
end

# Rsh.eval: the shell layer. At the top level by default; with a binding it sees its locals.
word = "two words"
Rsh.eval("echo top level")
Rsh.eval('echo #{word}', binding)
p Rsh.eval("x = 6 * 7\nx")
r = Rsh.eval(<<~'SH', binding)
  echo #{word} | tr a-z A-Z
  word.size
SH
p r
begin
  Rsh.eval("test -e /nonexistent-zz")
rescue Rsh::Failed => e
  puts "Rsh.eval raises Rsh::Failed: #{e.message}"
end
p Rsh.translate("ls -l").start_with?("__rsh_cmd(")
